/* Clip recorder without a GPU: frame arithmetic, ffmpeg command lines, the
 * rgb24 conversion against the PPM writer, the rawvideo/audio muxing helper
 * end to end (frames piped through ffmpeg and read back with ffmpeg and
 * ffprobe), the worker's capture window with a replay offset, and input
 * recording. The ffmpeg parts are skipped, not failed, when ffmpeg or
 * ffprobe is missing from PATH. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "recorder.h"
#include "playback.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"recorder FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

enum { W = 64, H = 48, FRAMES = 30 };   /* 30 frames: 0.5 s at 60.0988 */

static bool argv_equals(const RecorderCommand *cmd, const char *const *expected) {
    int i = 0;
    for (; expected[i]; i++)
        if (i >= cmd->argc || strcmp(cmd->argv[i], expected[i])) return false;
    return i == cmd->argc && cmd->argv[i] == NULL;
}

static void print_argv(const RecorderCommand *cmd) {
    char text[4096];
    recorder_command_text(cmd, text, sizeof(text));
    fprintf(stderr, "  argv: %s\n", text);
}

/* One line "key=value" from ffprobe output, or NULL. */
static const char *probe_value(const char *output, const char *key, char *value, size_t n) {
    size_t klen = strlen(key);
    for (const char *p = output; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *end = strchr(p + klen + 1, '\n');
            size_t len = end ? (size_t)(end - p - klen - 1) : strlen(p + klen + 1);
            if (len >= n) len = n - 1;
            memcpy(value, p + klen + 1, len); value[len] = 0;
            return value;
        }
    }
    return NULL;
}

static bool probe(const char *path, const char *stream, const char *entries, char *out, size_t n) {
    char command[2048];
    snprintf(command, sizeof(command),
             "ffprobe -v error -select_streams %s -show_entries stream=%s -of default=noprint_wrappers=1 '%s'",
             stream, entries, path);
    FILE *p = popen(command, "r");
    if (!p) return false;
    size_t got = fread(out, 1, n - 1, p);
    out[got] = 0;
    return pclose(p) == 0 && got > 0;
}

static bool file_exists(const char *path) { return access(path, F_OK) == 0; }

static bool tool_available(const char *tool) {
    char command[256];
    snprintf(command, sizeof(command), "%s -version >/dev/null 2>&1", tool);
    return system(command) == 0;
}

/* Frame f, pixel (x, y): distinct per frame so order and count are provable. */
static void fill_frame(uint8_t *rgba, unsigned f, bool bgra) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *px = rgba + ((size_t)y * W + x) * 4;
        uint8_t r = (uint8_t)(x * 4), g = (uint8_t)(y * 5), b = (uint8_t)(f * 32 + 7);
        px[0] = bgra ? b : r; px[1] = g; px[2] = bgra ? r : b; px[3] = 255;
    }
}

static void write_sine(FILE *audio, unsigned samples) {
    for (unsigned i = 0; i < samples; i++) {
        float v = 0.3f * sinf((float)i * 0.05f);
        fwrite(&v, sizeof(v), 1, audio);
    }
}

static void test_arithmetic(void) {
    CHECK(!strcmp(recorder_rate_string(0), "60.0988"));
    CHECK(!strcmp(recorder_rate_string(1), "50.007"));
    CHECK(recorder_frame_count(10, 0) == 601);      /* 600.988 rounds up */
    CHECK(recorder_frame_count(1, 1) == 50);        /* 50.007 rounds down */
    CHECK(recorder_frame_count(0.5, 0) == 30);
    CHECK(recorder_frame_count(0, 0) == 0);
    CHECK(recorder_frame_count(-1, 0) == 0);
    CHECK(recorder_frame_count(NAN, 0) == 0);
}

static void test_paths_and_commands(void) {
    char video[256], audio[256], log[256];
    CHECK(recorder_temp_paths("clips/demo.mov", video, audio, log, sizeof(video)));
    CHECK(!strcmp(video, "clips/demo.mov.video.mov"));
    CHECK(!strcmp(audio, "clips/demo.mov.audio.f32le"));
    CHECK(!strcmp(log, "clips/demo.mov.ffmpeg.log"));
    CHECK(recorder_temp_paths("Demo.MP4", video, audio, log, sizeof(video)));
    CHECK(!strcmp(video, "Demo.MP4.video.MP4"));
    CHECK(!recorder_temp_paths("demo.avi", video, audio, log, sizeof(video)));
    CHECK(!recorder_temp_paths("demo", video, audio, log, sizeof(video)));
    CHECK(!recorder_temp_paths("dir.mov/demo", video, audio, log, sizeof(video)));
    CHECK(!recorder_temp_paths("demo.mov", video, audio, log, 8));

    RecorderOptions options = { .output = "out/clip.mov", .ffmpeg = "ffmpeg-test",
        .codec_args = RECORDER_DEFAULT_CODEC_ARGS, .region = 0, .width = 3840, .height = 2880 };
    RecorderCommand cmd;
    CHECK(recorder_encode_command(&cmd, &options, "out/clip.mov.video.mov"));
    const char *const encode[] = { "ffmpeg-test", "-y", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", "3840x2880", "-r", "60.0988", "-i", "-",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "12", "-pix_fmt", "yuv444p",
        "-an", "out/clip.mov.video.mov", NULL };
    if (!argv_equals(&cmd, encode)) { CHECK(!"encode argv"); print_argv(&cmd); }
    options.region = 1;
    options.codec_args = "  -c:v h264_videotoolbox\t-b:v 90M   -pix_fmt yuv420p ";
    CHECK(recorder_encode_command(&cmd, &options, "v.mov"));
    const char *const encode_pal[] = { "ffmpeg-test", "-y", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-video_size", "3840x2880", "-r", "50.007", "-i", "-",
        "-c:v", "h264_videotoolbox", "-b:v", "90M", "-pix_fmt", "yuv420p", "-an", "v.mov", NULL };
    if (!argv_equals(&cmd, encode_pal)) { CHECK(!"PAL/override argv"); print_argv(&cmd); }
    options.codec_args = "   ";
    CHECK(!recorder_encode_command(&cmd, &options, "v.mov"));
    options.codec_args = RECORDER_DEFAULT_CODEC_ARGS;
    CHECK(recorder_mux_command(&cmd, &options, "v.mov", "a.f32le"));
    const char *const mux[] = { "ffmpeg-test", "-y", "-nostdin", "-loglevel", "error",
        "-i", "v.mov", "-f", "f32le", "-ar", "44100", "-ac", "1", "-i", "a.f32le",
        "-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-shortest",
        "-movflags", "+faststart", "out/clip.mov", NULL };
    if (!argv_equals(&cmd, mux)) { CHECK(!"mux argv"); print_argv(&cmd); }
    char text[512];
    const char *prefix = "ffmpeg-test -y -nostdin -loglevel error -i v.mov -f f32le -ar 44100";
    recorder_command_text(&cmd, text, sizeof(text));
    CHECK(!strncmp(text, prefix, strlen(prefix)));
    /* The argv is bounded; a pathological override fails rather than truncates. */
    char many[2048]; many[0] = 0;
    for (int i = 0; i < 60; i++) strcat(many, "-x ");
    options.codec_args = many;
    CHECK(!recorder_encode_command(&cmd, &options, "v.mov"));

    /* Environment defaults fill only what the caller left unset. */
    unsetenv("MYNES_FFMPEG"); unsetenv("MYNES_RECORD_CODEC_ARGS");
    RecorderOptions env = {0};
    recorder_options_from_env(&env);
    CHECK(!strcmp(env.ffmpeg, "ffmpeg") && !strcmp(env.codec_args, RECORDER_DEFAULT_CODEC_ARGS));
    setenv("MYNES_FFMPEG", "/opt/ffmpeg/bin/ffmpeg", 1);
    setenv("MYNES_RECORD_CODEC_ARGS", "-c:v prores_ks", 1);
    env = (RecorderOptions){ .ffmpeg = "explicit" };
    recorder_options_from_env(&env);
    CHECK(!strcmp(env.ffmpeg, "explicit") && !strcmp(env.codec_args, "-c:v prores_ks"));
    unsetenv("MYNES_FFMPEG"); unsetenv("MYNES_RECORD_CODEC_ARGS");
}

/* rgb24 must be the PPM's pixels: compare with frame_capture_write. */
static void test_rgb24(const char *directory) {
    uint8_t rgba[W * H * 4], bgra[W * H * 4], rgb[W * H * 3], rgb_b[W * H * 3];
    fill_frame(rgba, 3, false); fill_frame(bgra, 3, true);
    FrameCaptureImage a = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
    FrameCaptureImage b = { .pixels = bgra, .width = W, .height = H, .bgra = true, .white_level = 1 };
    CHECK(frame_capture_rgb24(&a, rgb) && frame_capture_rgb24(&b, rgb_b));
    CHECK(!memcmp(rgb, rgb_b, sizeof(rgb)));
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 3 * 32 + 7 && rgb[3] == 4);
    uint16_t hdr[6 * 4] = { 0x3c00, 0, 0, 0x3c00,  0, 0x3800, 0, 0x3c00,  0, 0, 0x4000, 0x3c00,
                            0x3400, 0x3400, 0x3400, 0x3c00,  0xfc00, 0, 0, 0x3c00,  0x3c00, 0x3c00, 0x3c00, 0x3c00 };
    FrameCaptureImage c = { .pixels = hdr, .width = 3, .height = 2, .hdr = true, .white_level = 1 };
    uint8_t hdr_rgb[18];
    CHECK(frame_capture_rgb24(&c, hdr_rgb));
    /* 1.0 -> 255, 0.5 -> sRGB 188, 2.0 clips, 0.25 -> 137, -inf -> 0. */
    CHECK(hdr_rgb[0] == 255 && hdr_rgb[1] == 0 && hdr_rgb[4] == 188 && hdr_rgb[8] == 255);
    CHECK(hdr_rgb[9] == 137 && hdr_rgb[12] == 0 && hdr_rgb[17] == 255);
    char path[512];
    snprintf(path, sizeof(path), "%s/hdr.ppm", directory);
    CHECK(frame_capture_write(&c, path));
    FILE *f = fopen(path, "rb");
    if (f) {
        int w, h, max; uint8_t ppm[18];
        CHECK(fscanf(f, "P6\n%d %d\n%d", &w, &h, &max) == 3 && fgetc(f) == '\n');
        CHECK(fread(ppm, 1, 18, f) == 18 && !memcmp(ppm, hdr_rgb, 18));
        fclose(f);
    } else CHECK(false);
    snprintf(path, sizeof(path), "%s/sdr.ppm", directory);
    CHECK(frame_capture_write(&b, path));
    f = fopen(path, "rb");
    if (f) {
        int w, h, max; static uint8_t ppm[W * H * 3];
        CHECK(fscanf(f, "P6\n%d %d\n%d", &w, &h, &max) == 3 && fgetc(f) == '\n');
        CHECK(fread(ppm, 1, sizeof(ppm), f) == sizeof(ppm) && !memcmp(ppm, rgb, sizeof(ppm)));
        fclose(f);
    } else CHECK(false);
}

/* Pipe frames through the real ffmpeg, mux with audio, read everything back. */
static void test_end_to_end(const char *directory) {
    char output[512], video[600], audio[600], log[600], decoded[600], error[2048];
    snprintf(output, sizeof(output), "%s/clip.mov", directory);
    snprintf(decoded, sizeof(decoded), "%s/decoded.rgb", directory);
    CHECK(recorder_temp_paths(output, video, audio, log, sizeof(video)));
    const unsigned frames = FRAMES;
    RecorderOptions options = { .output = output, .seconds = 0.5, .after = 2, .region = 0,
        .width = W, .height = H, .codec_args = "-c:v rawvideo -pix_fmt rgb24" };
    Recorder *r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    CHECK(recorder_frames(r) == frames && recorder_first_frame(r) == 3 && recorder_last_frame(r) == 32);
    CHECK(!recorder_want_frame(r, 2) && recorder_want_frame(r, 3) && recorder_want_frame(r, 32) && !recorder_want_frame(r, 33));
    CHECK(file_exists(log));   /* the encoder is running against its log */
    static uint8_t rgba[W * H * 4], expected[FRAMES * W * H * 3];
    for (unsigned f = 0; f < frames; f++) {
        fill_frame(rgba, f, f % 2 == 1);
        FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .bgra = f % 2 == 1, .white_level = 1 };
        CHECK(frame_capture_rgb24(&image, expected + (size_t)f * W * H * 3));
        CHECK(recorder_push_frame(r, &image));
        CHECK(recorder_frames_written(r) == f + 1);
        CHECK(recorder_complete(r) == (f + 1 == frames));
    }
    CHECK(!recorder_want_frame(r, 10));   /* full: nothing more is wanted */
    /* A frame beyond the count is a caller bug; the recording fails rather
     * than silently growing. */
    FrameCaptureImage extra = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
    CHECK(!recorder_push_frame(r, &extra));
    write_sine(recorder_audio_file(r), (unsigned)lround(frames * 44100 / 60.0988));
    bool finished = recorder_finish(r, error, sizeof(error));
    CHECK(!finished && strstr(error, "more frames") != NULL);
    recorder_destroy(r);
    CHECK(!file_exists(video) && !file_exists(audio) && !file_exists(log));

    /* The same again without the deliberate error: the clip must be exact. */
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    for (unsigned f = 0; f < frames; f++) {
        fill_frame(rgba, f, f % 2 == 1);
        FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .bgra = f % 2 == 1, .white_level = 1 };
        CHECK(recorder_push_frame(r, &image));
    }
    /* The worker writes exactly the recorded frames' samples. */
    write_sine(recorder_audio_file(r), (unsigned)lround(frames * 44100 / 60.0988));
    finished = recorder_finish(r, error, sizeof(error));
    if (!finished) fprintf(stderr, "  finish: %s\n", error);
    CHECK(finished);
    recorder_destroy(r);
    CHECK(!file_exists(video) && !file_exists(audio) && !file_exists(log));
    CHECK(file_exists(output));
    char info[2048], value[128];
    CHECK(probe(output, "v:0", "codec_name,pix_fmt,nb_frames,r_frame_rate,width,height,duration", info, sizeof(info)));
    CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "rawvideo"));
    CHECK(probe_value(info, "pix_fmt", value, sizeof(value)) && !strcmp(value, "rgb24"));
    CHECK(probe_value(info, "nb_frames", value, sizeof(value)) && atoi(value) == (int)frames);
    CHECK(probe_value(info, "width", value, sizeof(value)) && atoi(value) == W);
    CHECK(probe_value(info, "height", value, sizeof(value)) && atoi(value) == H);
    if (probe_value(info, "r_frame_rate", value, sizeof(value))) {
        long num = 0, den = 1;
        CHECK(sscanf(value, "%ld/%ld", &num, &den) == 2 && den > 0);
        CHECK(fabs((double)num / (double)den - 60.0988) < 1e-4);
    } else CHECK(false);
    CHECK(probe_value(info, "duration", value, sizeof(value)) && fabs(atof(value) - frames / 60.0988) < 1e-3);
    CHECK(probe(output, "a:0", "codec_name,sample_rate,channels,duration", info, sizeof(info)));
    CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "aac"));
    CHECK(probe_value(info, "sample_rate", value, sizeof(value)) && atoi(value) == 44100);
    CHECK(probe_value(info, "channels", value, sizeof(value)) && atoi(value) == 1);
    /* -shortest may drop the padded final AAC frame: within one of them. */
    CHECK(probe_value(info, "duration", value, sizeof(value)) && fabs(atof(value) - frames / 60.0988) < 0.03);
    char command[2048];
    snprintf(command, sizeof(command), "ffmpeg -y -loglevel error -i '%s' -f rawvideo -pix_fmt rgb24 '%s'", output, decoded);
    CHECK(system(command) == 0);
    FILE *f = fopen(decoded, "rb");
    if (f) {
        static uint8_t got[sizeof(expected) + 1];
        size_t n = fread(got, 1, sizeof(got), f);
        fclose(f);
        CHECK(n == sizeof(expected));
        CHECK(n == sizeof(expected) && !memcmp(got, expected, sizeof(expected)));
    } else CHECK(false);

    /* The default codec arguments produce the documented master format. */
    snprintf(output, sizeof(output), "%s/master.mp4", directory);
    options.output = output; options.codec_args = NULL; options.seconds = 0.1;
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        CHECK(recorder_frames(r) == 6);
        for (unsigned f = 0; f < 6; f++) {
            fill_frame(rgba, f, false);
            FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
            CHECK(recorder_push_frame(r, &image));
        }
        write_sine(recorder_audio_file(r), 4410);
        finished = recorder_finish(r, error, sizeof(error));
        if (!finished) fprintf(stderr, "  finish: %s\n", error);
        CHECK(finished);
        recorder_destroy(r);
        CHECK(probe(output, "v:0", "codec_name,pix_fmt,nb_frames", info, sizeof(info)));
        CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "h264"));
        CHECK(probe_value(info, "pix_fmt", value, sizeof(value)) && !strcmp(value, "yuv444p"));
        CHECK(probe_value(info, "nb_frames", value, sizeof(value)) && atoi(value) == 6);
    } else fprintf(stderr, "  %s\n", error);

    /* Failures: too few frames, a broken encoder, a mismatched size, no ffmpeg. */
    snprintf(output, sizeof(output), "%s/short.mov", directory);
    options.output = output; options.codec_args = "-c:v rawvideo -pix_fmt rgb24"; options.seconds = 0.1;
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        fill_frame(rgba, 0, false);
        FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
        CHECK(recorder_push_frame(r, &image));
        FrameCaptureImage wrong = { .pixels = rgba, .width = W / 2, .height = H, .white_level = 1 };
        CHECK(!recorder_push_frame(r, &wrong));
        CHECK(!recorder_finish(r, error, sizeof(error)));
        CHECK(strstr(error, "does not match") != NULL);
        recorder_destroy(r);
        CHECK(!file_exists(output));
    }
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        for (unsigned f = 0; f < 2; f++) {
            fill_frame(rgba, f, false);
            FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
            CHECK(recorder_push_frame(r, &image));
        }
        CHECK(!recorder_finish(r, error, sizeof(error)));
        CHECK(strstr(error, "only 2 of 6") != NULL);
        recorder_destroy(r);
        CHECK(!file_exists(video) && !file_exists(audio) && !file_exists(log));
    }
    options.codec_args = "-c:v no_such_encoder_xyz";
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        /* ffmpeg rejects the encoder and exits; the write side sees the
         * closed pipe sooner or later, and finish reports ffmpeg's words. */
        for (unsigned f = 0; f < 6; f++) {
            fill_frame(rgba, f, false);
            FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
            recorder_push_frame(r, &image);
        }
        CHECK(!recorder_finish(r, error, sizeof(error)));
        CHECK(strstr(error, "exited with status") != NULL);
        CHECK(strstr(error, "no_such_encoder_xyz") != NULL);
        recorder_destroy(r);
    }
    options.codec_args = NULL; options.ffmpeg = "/nonexistent/ffmpeg";
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r == NULL);
    CHECK(strstr(error, "cannot run /nonexistent/ffmpeg") != NULL);
    if (r) recorder_destroy(r);
    options.ffmpeg = NULL; options.output = "clip.avi";
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL);
    CHECK(strstr(error, ".mov or .mp4") != NULL);
    options.output = output; options.seconds = 0.001;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL);
    CHECK(strstr(error, "at least one frame") != NULL);
}

static bool next(Playback *p, PlaybackFrame *frame) {
    Uint64 timeout = SDL_GetTicks() + 2000;
    do {
        if (playback_read(p, frame)) return true;
        SDL_Delay(1);
    } while (SDL_GetTicks() < timeout);
    return false;
}

static void note_frames(NES *nes, void *user) { *(uint64_t *)user = nes->ppu.frame; }

/* The worker's side of a recording: only pictures first..last reach the
 * reader without a drop, their audio alone reaches the file, script rows
 * count from `first`, and the worker stops after `last`. */
static void test_capture_window(const char *directory) {
    NES *nes = calloc(1, sizeof(*nes));
    uint8_t *prg = calloc(32768, 1), *chr = calloc(8192, 1);
    prg[0] = 0x4c; prg[1] = 0; prg[2] = 0x80;   /* JMP $8000: a stable synthetic cartridge */
    prg[0x7ffc] = 0; prg[0x7ffd] = 0x80;
    nes_init(nes); nes_load_mapper(nes, 0, prg, 32768, chr, 8192, 0); nes_reset(nes);
    SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 1, .freq = 44100 };
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, &spec);
    char script[512], audio_path[512];
    snprintf(script, sizeof(script), "%s/replay.txt", directory);
    snprintf(audio_path, sizeof(audio_path), "%s/window.f32le", directory);
    FILE *f = fopen(script, "w");
    fputs("1 80\n3 00\n", f); fclose(f);
    /* Row 1 lands on picture `first` = 3, row 3 on picture 5: after picture
     * 4 Right is held, after picture 5 it is released. */
    for (unsigned last = 2; last <= 5; last++) {
        Playback *p = playback_create(nes, NULL, NULL, stream, 0, 0);
        CHECK(p != NULL);
        if (!p) continue;
        CHECK(playback_load_input_script(p, script));
        FILE *audio = fopen(audio_path, "wb");
        CHECK(audio != NULL);
        playback_arm_capture(p, 3, last, audio);
        PlaybackControls controls = { .analog = nes->apu.analog, .low_latency = true };
        controls.controller[0] = 0x01;   /* ignored: the script owns player 1 */
        audio_chain_init_preset(&controls.audio, 0, 0, 0);
        playback_controls(p, &controls); playback_resume(p);
        PlaybackFrame frame;
        unsigned previous = 0, seen_from_first = 0;
        /* Pictures before `first` may be dropped; from `first` on every one
         * arrives in order, and none after `last`. */
        while (previous < last && next(p, &frame)) {
            CHECK(frame.number > previous && frame.number <= last);
            if (frame.number >= 3) {
                CHECK(seen_from_first ? frame.number == previous + 1 : frame.number == 3);
                seen_from_first++;
            }
            previous = frame.number;
        }
        if (last >= 3) CHECK(previous == last);
        SDL_Delay(60);   /* a few frame periods: the stopped worker queues nothing more */
        CHECK(!playback_read(p, &frame));
        CHECK(seen_from_first == (last >= 3 ? last - 2 : 0));
        CHECK(playback_frames_sampled(p) == last);
        uint64_t ppu_frames = 0;
        CHECK(playback_with_console(p, note_frames, &ppu_frames) == last);
        CHECK(nes->controller[0] == (last == 3 || last == 4 ? 0x80 : 0x00));
        playback_pause(p);
        playback_destroy(p);
        fclose(audio);
        f = fopen(audio_path, "rb");
        CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            long samples = ftell(f) / (long)sizeof(float);
            fclose(f);
            /* 733.8 samples per NTSC frame for pictures 3..last only. */
            double expected = last >= 3 ? (last - 2) * 44100 / 60.0988 : 0;
            CHECK(fabs(samples - expected) <= 2 + (last >= 3 ? last - 2 : 0));
        }
    }
    /* Without arming, MYNES_REVIEW_INPUT_SCRIPT semantics are unchanged:
     * row 1 applies to frame 1. */
    Playback *p = playback_create(nes, NULL, NULL, stream, 2, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(playback_load_input_script(p, script));
        PlaybackControls controls = { .analog = nes->apu.analog };
        audio_chain_init_preset(&controls.audio, 0, 0, 0);
        playback_controls(p, &controls); playback_resume(p);
        PlaybackFrame frame = {0};
        while (frame.number < 2 && next(p, &frame)) {}
        CHECK(frame.number == 2);
        playback_pause(p);
        CHECK(nes->controller[0] == 0x80);
        playback_destroy(p);
    }
    f = fopen(script, "w"); fputs("3 01\n2 00\n", f); fclose(f);
    p = playback_create(nes, NULL, NULL, stream, 2, 0);
    CHECK(p != NULL);
    if (p) { CHECK(!playback_load_input_script(p, script)); playback_destroy(p); }
    SDL_DestroyAudioStream(stream);
    free(nes); free(prg); free(chr);
}

static bool file_is(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    char got[256] = "";
    if (!f) return false;
    size_t n = fread(got, 1, sizeof(got) - 1, f);
    got[n] = 0;
    fclose(f);
    if (strcmp(got, expected)) fprintf(stderr, "  file holds %s", got);
    return !strcmp(got, expected);
}

static void test_input_record(const char *directory) {
    char path[512];
    snprintf(path, sizeof(path), "%s/input.txt", directory);
    InputRecord rec;
    CHECK(input_record_open(&rec, path));
    CHECK(input_record_update(&rec, 1, 0x00));      /* no change from the initial mask */
    CHECK(input_record_update(&rec, 1, 0x80));      /* pressed and released within one frame: */
    CHECK(input_record_update(&rec, 1, 0x00));      /* the frame never sees it */
    CHECK(input_record_update(&rec, 2, 0x00));
    CHECK(input_record_update(&rec, 2, 0x08));
    CHECK(input_record_update(&rec, 2, 0x0c));      /* the last value before the frame wins */
    CHECK(input_record_update(&rec, 3, 0x0c));
    CHECK(file_is(path, "2 0c\n"));
    CHECK(input_record_update(&rec, 5, 0x0c));
    CHECK(input_record_update(&rec, 5, 0x00));
    CHECK(input_record_update(&rec, 6, 0x00));
    CHECK(input_record_update(&rec, 0, 0x01));      /* frame 0 is not a valid row */
    CHECK(file_is(path, "2 0c\n5 00\n"));
    CHECK(input_record_close(&rec));
    CHECK(file_is(path, "2 0c\n5 00\n1 01\n"));    /* the held row is flushed on close */
    CHECK(rec.file == NULL);
    CHECK(input_record_open(&rec, path));
    CHECK(input_record_update(&rec, 4, 0x10));
    CHECK(input_record_update(&rec, 9, 0x10));
    CHECK(file_is(path, "4 10\n"));
    CHECK(input_record_restart(&rec));              /* a new time line: rows start again */
    CHECK(file_is(path, ""));
    CHECK(input_record_update(&rec, 1, 0x10));      /* still held after the load: a row at 1 */
    CHECK(input_record_update(&rec, 7, 0x10));
    CHECK(input_record_update(&rec, 7, 0x00));
    CHECK(input_record_close(&rec));
    CHECK(file_is(path, "1 10\n7 00\n"));
    /* What was recorded loads as a replay script. */
    NES *nes = calloc(1, sizeof(*nes));
    nes_init(nes);
    SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 1, .freq = 44100 };
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, &spec);
    Playback *p = playback_create(nes, NULL, NULL, stream, 1, 0);
    CHECK(p != NULL);
    if (p) { CHECK(playback_load_input_script(p, path)); playback_destroy(p); }
    SDL_DestroyAudioStream(stream);
    free(nes);
}

int main(void) {
    if (!SDL_Init(SDL_INIT_AUDIO)) return 1;
    char directory[] = "/tmp/mynes-recorder-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    test_arithmetic();
    test_paths_and_commands();
    test_rgb24(directory);
    if (tool_available("ffmpeg") && tool_available("ffprobe")) test_end_to_end(directory);
    else fprintf(stderr, "recorder: ffmpeg/ffprobe not on PATH, end-to-end muxing not tested\n");
    test_capture_window(directory);
    test_input_record(directory);
    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", directory);
    if (system(command)) {}
    SDL_Quit();
    printf("Recorder regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
