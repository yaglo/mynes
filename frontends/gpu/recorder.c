/* Clip recorder: see recorder.h. The GPU side of a recording is the capture
 * sink in gpu_render.c; everything here is plain POSIX so the frame
 * arithmetic, the ffmpeg command lines and the muxing helper run in tests. */
#define _POSIX_C_SOURCE 200809L
#include "recorder.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* --- Frame arithmetic --- */

/* The decimals ffmpeg receives; the frame count uses the same values so the
 * clip length and the stream rate agree. PAL region is any non-zero value,
 * as in the playback worker. */
const char *recorder_rate_string(int region) { return region ? "50.007" : "60.0988"; }
double recorder_rate(int region) { return region ? 50.007 : 60.0988; }

unsigned recorder_frame_count(double seconds, int region) {
    if (!(seconds > 0) || !isfinite(seconds)) return 0;
    double frames = round(seconds * recorder_rate(region));
    return frames > 1e9 ? 0 : (unsigned)frames;
}

/* --- Command construction --- */

void recorder_options_from_env(RecorderOptions *options) {
    if (!options->ffmpeg) options->ffmpeg = getenv("MYNES_FFMPEG");
    if (!options->ffmpeg || !*options->ffmpeg) options->ffmpeg = "ffmpeg";
    if (!options->codec_args) options->codec_args = getenv("MYNES_RECORD_CODEC_ARGS");
    if (!options->codec_args || !*options->codec_args) options->codec_args = RECORDER_DEFAULT_CODEC_ARGS;
}

/* The container extension, or NULL when it is not one ffmpeg muxes as
 * QuickTime/MP4; both containers take the same H.264 stream. */
static const char *container_extension(const char *output) {
    const char *slash = strrchr(output, '/');
    const char *dot = strrchr(slash ? slash : output, '.');
    if (!dot || dot == output || dot[-1] == '/') return NULL;
    if (strcasecmp(dot, ".mov") && strcasecmp(dot, ".mp4")) return NULL;
    return dot;
}

bool recorder_temp_paths(const char *output, char *video, char *audio, char *log, size_t n) {
    const char *ext = container_extension(output);
    if (!ext) return false;
    int v = snprintf(video, n, "%s.video%s", output, ext);
    int a = snprintf(audio, n, "%s.audio.f32le", output);
    int l = snprintf(log, n, "%s.ffmpeg.log", output);
    return v > 0 && (size_t)v < n && a > 0 && (size_t)a < n && l > 0 && (size_t)l < n;
}

bool recorder_command_add(RecorderCommand *cmd, const char *arg) {
    size_t len = strlen(arg) + 1;
    if (cmd->argc >= RECORDER_ARGV_MAX || cmd->used + len > sizeof(cmd->storage)) return false;
    memcpy(cmd->storage + cmd->used, arg, len);
    cmd->argv[cmd->argc++] = cmd->storage + cmd->used;
    cmd->argv[cmd->argc] = NULL;
    cmd->used += len;
    return true;
}

bool recorder_command_add_split(RecorderCommand *cmd, const char *args) {
    int added = 0;
    for (const char *p = args; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char word[512];
        size_t len = (size_t)(p - start);
        if (len >= sizeof(word)) return false;
        memcpy(word, start, len); word[len] = 0;
        if (!recorder_command_add(cmd, word)) return false;
        added++;
    }
    return added > 0;
}

static bool add_all(RecorderCommand *cmd, const char *const *args) {
    for (; *args; args++) if (!recorder_command_add(cmd, *args)) return false;
    return true;
}

bool recorder_encode_command(RecorderCommand *cmd, const RecorderOptions *options,
                             const char *video_path) {
    char size[32];
    memset(cmd, 0, sizeof(*cmd));
    if (!options->ffmpeg || !options->codec_args || options->width <= 0 || options->height <= 0) return false;
    snprintf(size, sizeof(size), "%dx%d", options->width, options->height);
    /* "-i -" is what makes ffmpeg treat stdin as data rather than a
     * terminal; the input rate is set before it so no frame is duplicated
     * or dropped to reach the output rate. -loglevel error keeps the log
     * to what an error message should show. */
    const char *const head[] = { options->ffmpeg, "-y", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", size,
        "-r", recorder_rate_string(options->region), "-i", "-", NULL };
    const char *const tail[] = { "-an", video_path, NULL };
    return add_all(cmd, head) && recorder_command_add_split(cmd, options->codec_args) &&
           add_all(cmd, tail);
}

bool recorder_mux_command(RecorderCommand *cmd, const RecorderOptions *options,
                          const char *video_path, const char *audio_path) {
    memset(cmd, 0, sizeof(*cmd));
    if (!options->ffmpeg || !options->output) return false;
    /* The APU capture is float32 mono at the worker's stream rate. -shortest
     * trims the AAC encoder padding; faststart puts the index first so the
     * clip streams from a web page. */
    const char *const args[] = { options->ffmpeg, "-y", "-nostdin", "-loglevel", "error",
        "-i", video_path, "-f", "f32le", "-ar", "44100", "-ac", "1", "-i", audio_path,
        "-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-shortest",
        "-movflags", "+faststart", options->output, NULL };
    return add_all(cmd, args);
}

void recorder_command_text(const RecorderCommand *cmd, char *out, size_t n) {
    size_t used = 0;
    if (!n) return;
    out[0] = 0;
    for (int i = 0; i < cmd->argc && used < n; i++) {
        int len = snprintf(out + used, n - used, "%s%s", i ? " " : "", cmd->argv[i]);
        if (len < 0) break;
        used += (size_t)len;
    }
}

/* --- Running ffmpeg --- */

typedef struct { pid_t pid; int stdin_fd; } Child;

/* Start cmd with its output streams appended to log_path. With want_stdin
 * the child's stdin is a pipe whose write end is returned; the child
 * inherits nothing else of ours. */
static bool spawn_child(const RecorderCommand *cmd, const char *log_path, bool want_stdin,
                        Child *child, char *error, size_t error_size) {
    int fds[2] = { -1, -1 };
    child->pid = 0; child->stdin_fd = -1;
    if (want_stdin && pipe(fds)) {
        snprintf(error, error_size, "pipe: %s", strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (want_stdin) {
        posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_addopen(&actions, 2, log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, 2, 1);
    /* posix_spawnp searches PATH for a bare name and reports a missing
     * executable here rather than in a child we would have to inspect. */
    int err = posix_spawnp(&child->pid, cmd->argv[0], &actions, NULL, cmd->argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (want_stdin) close(fds[0]);
    if (err) {
        if (want_stdin) close(fds[1]);
        snprintf(error, error_size, "cannot run %s: %s", cmd->argv[0], strerror(err));
        return false;
    }
    if (want_stdin) {
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        child->stdin_fd = fds[1];
    }
    return true;
}

/* Exit status, or -1 when the child was killed by a signal. */
static int wait_child(Child *child) {
    int status = 0;
    if (!child->pid) return -1;
    while (waitpid(child->pid, &status, 0) < 0)
        if (errno != EINTR) { child->pid = 0; return -1; }
    child->pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The last whole lines of the log, bounded by the message buffer. */
static void log_tail(const char *log_path, char *out, size_t n) {
    FILE *f = fopen(log_path, "rb");
    long size = 0, keep = (long)n - 1;
    if (keep > 1500) keep = 1500;
    if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
    long start = size > keep ? size - keep : 0;
    size_t got = 0;
    if (f && size > 0 && !fseek(f, start, SEEK_SET)) got = fread(out, 1, (size_t)(size - start), f);
    if (f) fclose(f);
    out[got] = 0;
    if (start > 0) {
        /* A cut first line is misleading; drop it when a complete one follows. */
        char *nl = strchr(out, '\n');
        if (nl && nl[1]) memmove(out, nl + 1, strlen(nl + 1) + 1);
    }
    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;
    if (!out[0]) snprintf(out, n, "(no ffmpeg output)");
}

static void status_error(const char *what, int status, const char *log_path,
                         char *error, size_t error_size) {
    char tail[1600];
    log_tail(log_path, tail, sizeof(tail));
    if (status < 0) snprintf(error, error_size, "ffmpeg %s was killed:\n%s", what, tail);
    else snprintf(error, error_size, "ffmpeg %s exited with status %d:\n%s", what, status, tail);
}

bool recorder_run(const RecorderCommand *cmd, const char *log_path, char *error, size_t error_size) {
    Child child;
    if (!spawn_child(cmd, log_path, false, &child, error, error_size)) return false;
    int status = wait_child(&child);
    if (status == 0) return true;
    status_error("mux", status, log_path, error, error_size);
    return false;
}

static bool write_all(int fd, const uint8_t *data, size_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        data += n; size -= (size_t)n;
    }
    return true;
}

/* --- The recorder proper --- */

struct Recorder {
    RecorderOptions options;
    char     output[1024], ffmpeg[512], codec_args[1024];
    char     video_path[1100], audio_path[1100], log_path[1100];
    unsigned first, last, frames, written;
    Child    encoder;
    FILE    *audio;
    uint8_t *rgb;
    char     error[512];
};

static void remove_temps(const Recorder *r) {
    unlink(r->video_path);
    unlink(r->audio_path);
    unlink(r->log_path);
}

Recorder *recorder_create(const RecorderOptions *options, char *error, size_t error_size) {
    Recorder *r = calloc(1, sizeof(*r));
    if (!r) { snprintf(error, error_size, "out of memory"); return NULL; }
    r->options = *options;
    recorder_options_from_env(&r->options);
    snprintf(r->output, sizeof(r->output), "%s", options->output ? options->output : "");
    snprintf(r->ffmpeg, sizeof(r->ffmpeg), "%s", r->options.ffmpeg);
    snprintf(r->codec_args, sizeof(r->codec_args), "%s", r->options.codec_args);
    r->options.output = r->output;
    r->options.ffmpeg = r->ffmpeg;
    r->options.codec_args = r->codec_args;
    r->encoder.stdin_fd = -1;
    r->frames = recorder_frame_count(options->seconds, options->region);
    r->first = options->after + 1;
    r->last = options->after + r->frames;
    if (!r->output[0] || !recorder_temp_paths(r->output, r->video_path, r->audio_path,
                                              r->log_path, sizeof(r->video_path))) {
        snprintf(error, error_size, "the recording must be a .mov or .mp4 file");
        goto fail;
    }
    if (!r->frames || r->last < r->first) {
        snprintf(error, error_size, "--record-seconds must cover at least one frame");
        goto fail;
    }
    if (options->width <= 0 || options->height <= 0) {
        snprintf(error, error_size, "recording needs the offscreen target size");
        goto fail;
    }
    r->rgb = malloc((size_t)options->width * options->height * 3);
    FILE *log = fopen(r->log_path, "w");
    if (log) fclose(log);
    r->audio = fopen(r->audio_path, "wb");
    if (!r->rgb || !log || !r->audio) {
        snprintf(error, error_size, "cannot create %s: %s", r->audio_path, strerror(errno));
        goto fail;
    }
    RecorderCommand cmd;
    if (!recorder_encode_command(&cmd, &r->options, r->video_path)) {
        snprintf(error, error_size, "the codec arguments do not fit an ffmpeg command line");
        goto fail;
    }
    /* A dying encoder must surface as a write error, not end this process. */
    signal(SIGPIPE, SIG_IGN);
    if (!spawn_child(&cmd, r->log_path, true, &r->encoder, error, error_size)) goto fail;
    char text[sizeof(cmd.storage) + RECORDER_ARGV_MAX];
    recorder_command_text(&cmd, text, sizeof(text));
    fprintf(stderr, "Recording %u frames (%.3f s at %s fps) to %s\n", r->frames,
            r->frames / recorder_rate(options->region), recorder_rate_string(options->region), r->output);
    fprintf(stderr, "Recording: %s\n", text);
    return r;
fail:
    if (r->audio) fclose(r->audio);
    remove_temps(r);
    free(r->rgb);
    free(r);
    return NULL;
}

unsigned recorder_first_frame(const Recorder *r) { return r->first; }
unsigned recorder_last_frame(const Recorder *r) { return r->last; }
unsigned recorder_frames(const Recorder *r) { return r->frames; }
unsigned recorder_frames_written(const Recorder *r) { return r->written; }
FILE *recorder_audio_file(Recorder *r) { return r->audio; }

bool recorder_want_frame(const Recorder *r, unsigned number) {
    return number >= r->first && number <= r->last && r->written < r->frames;
}

bool recorder_complete(const Recorder *r) { return r->written >= r->frames; }

bool recorder_push_frame(void *user, const FrameCaptureImage *image) {
    Recorder *r = user;
    if (r->error[0]) return false;
    if (image->width != r->options.width || image->height != r->options.height) {
        snprintf(r->error, sizeof(r->error), "frame %dx%d does not match the %dx%d recording",
                 image->width, image->height, r->options.width, r->options.height);
        return false;
    }
    if (r->written >= r->frames) {
        snprintf(r->error, sizeof(r->error), "more frames than the %u requested", r->frames);
        return false;
    }
    if (!frame_capture_rgb24(image, r->rgb)) {
        snprintf(r->error, sizeof(r->error), "cannot convert frame %u", r->written + 1);
        return false;
    }
    size_t bytes = (size_t)image->width * image->height * 3;
    if (!write_all(r->encoder.stdin_fd, r->rgb, bytes)) {
        snprintf(r->error, sizeof(r->error), "ffmpeg stopped reading video at frame %u: %s",
                 r->written + 1, strerror(errno));
        return false;
    }
    r->written++;
    if (r->written % 60 == 0)
        fprintf(stderr, "Recording: %u/%u frames\n", r->written, r->frames);
    return true;
}

bool recorder_finish(Recorder *r, char *error, size_t error_size) {
    bool ok = true;
    error[0] = 0;
    if (r->audio && fclose(r->audio)) {
        snprintf(error, error_size, "cannot write %s: %s", r->audio_path, strerror(errno));
        ok = false;
    }
    r->audio = NULL;
    /* End of stdin ends the encode; ffmpeg then flushes and closes the file. */
    if (r->encoder.stdin_fd >= 0) close(r->encoder.stdin_fd);
    r->encoder.stdin_fd = -1;
    int status = r->encoder.pid ? wait_child(&r->encoder) : 0;
    if (status != 0) {
        /* The encoder's own complaint explains any write error better. */
        status_error("encode", status, r->log_path, error, error_size);
        ok = false;
    } else if (r->error[0]) {
        snprintf(error, error_size, "%s", r->error);
        ok = false;
    } else if (r->written < r->frames) {
        snprintf(error, error_size, "only %u of %u frames were captured", r->written, r->frames);
        ok = false;
    }
    if (ok) {
        RecorderCommand cmd;
        if (!recorder_mux_command(&cmd, &r->options, r->video_path, r->audio_path)) {
            snprintf(error, error_size, "the output path does not fit an ffmpeg command line");
            ok = false;
        } else {
            ok = recorder_run(&cmd, r->log_path, error, error_size);
        }
    }
    if (ok)
        fprintf(stderr, "Recorded %u frames (%.3f s) to %s\n", r->written,
                r->written / recorder_rate(r->options.region), r->output);
    remove_temps(r);
    return ok;
}

void recorder_destroy(Recorder *r) {
    if (!r) return;
    if (r->audio) fclose(r->audio);
    if (r->encoder.stdin_fd >= 0) close(r->encoder.stdin_fd);
    if (r->encoder.pid) {
        /* Nothing will mux this; do not leave an encoder writing a file. */
        kill(r->encoder.pid, SIGTERM);
        wait_child(&r->encoder);
        remove_temps(r);
    }
    free(r->rgb);
    free(r);
}

/* --- Input recording --- */

bool input_record_open(InputRecord *r, const char *path) {
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->file = fopen(r->path, "w");
    return r->file != NULL;
}

/* Flushed per row so a crash keeps what was played up to it. */
static bool write_row(InputRecord *r, unsigned frame, uint8_t mask) {
    r->written_frame = frame;
    r->written_mask = mask;
    r->rows++;
    return fprintf(r->file, "%u %02x\n", frame, mask) > 0 && !fflush(r->file);
}

bool input_record_update(InputRecord *r, unsigned frame, uint8_t mask) {
    bool ok = true;
    if (!r->file) return true;
    if (!frame) frame = 1;   /* the loader rejects frame 0 */
    if (r->pending && frame > r->pending_frame) {
        if (r->pending_mask != r->written_mask) ok = write_row(r, r->pending_frame, r->pending_mask);
        r->pending = false;
    }
    if (r->pending) {
        r->pending_mask = mask;   /* the frame has not started: the last value is what it samples */
    } else if (mask != r->written_mask) {
        r->pending = true;
        r->pending_frame = frame;
        r->pending_mask = mask;
    }
    return ok;
}

bool input_record_restart(InputRecord *r) {
    if (!r->file) return true;
    /* A held row belongs to the time line being abandoned. */
    r->pending = false;
    r->written_frame = 0;
    r->written_mask = 0;
    r->rows = 0;
    r->file = freopen(r->path, "w", r->file);
    return r->file != NULL;
}

bool input_record_close(InputRecord *r) {
    bool ok = true;
    if (!r->file) return true;
    if (r->pending && r->pending_mask != r->written_mask)
        ok = write_row(r, r->pending_frame, r->pending_mask);
    r->pending = false;
    if (fclose(r->file)) ok = false;
    r->file = NULL;
    return ok;
}
