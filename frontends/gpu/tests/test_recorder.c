/* Clip recorder without a GPU: frame arithmetic, ffmpeg command lines, the
 * rgb24 conversion against the PPM writer, the OUT.json sidecar and the
 * clip's light levels with a stand-in ffmpeg, the rawvideo/audio muxing
 * helper end to end (frames piped through ffmpeg and read back with ffmpeg
 * and ffprobe), an HDR clip through ProRes and back with its colour tags,
 * the HDR flags, the worker's capture window with a replay offset, and
 * input recording. The parts that need the real ffmpeg are skipped, not
 * failed, when ffmpeg or ffprobe is missing from PATH. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "recorder.h"
#include "frame_pq.h"
#include "gpu_half.h"
#include "playback.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* The whole of a small text file, or "" when it cannot be read. */
static const char *read_text(const char *path, char *text, size_t n) {
    FILE *f = fopen(path, "rb");
    size_t got = f ? fread(text, 1, n - 1, f) : 0;
    if (f) fclose(f);
    text[got] = 0;
    return text;
}

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
    char json[256];
    CHECK(recorder_json_path("clips/demo.mov", json, sizeof(json)) && !strcmp(json, "clips/demo.json"));
    CHECK(recorder_json_path("v1.2/Demo.MP4", json, sizeof(json)) && !strcmp(json, "v1.2/Demo.json"));
    CHECK(!recorder_json_path("demo.avi", json, sizeof(json)));
    CHECK(!recorder_json_path("demo.mov", json, 9));

    RecorderOptions options = { .output = "out/clip.mov", .ffmpeg = "ffmpeg-test", .seconds = 2,
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
    /* PAL: 100 frames of 50.007 Hz, cut at the video's end. */
    const char *const mux[] = { "ffmpeg-test", "-y", "-nostdin", "-loglevel", "error",
        "-i", "v.mov", "-f", "f32le", "-ar", "44100", "-ac", "1", "-i", "a.f32le",
        "-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-t", "1.999720",
        "-movflags", "+faststart", "out/clip.mov", NULL };
    if (!argv_equals(&cmd, mux)) { CHECK(!"mux argv"); print_argv(&cmd); }
    char text[512];
    const char *prefix = "ffmpeg-test -y -nostdin -loglevel error -i v.mov -f f32le -ar 44100";
    recorder_command_text(&cmd, text, sizeof(text));
    CHECK(!strncmp(text, prefix, strlen(prefix)));
    options.seconds = 0.001;
    CHECK(!recorder_mux_command(&cmd, &options, "v.mov", "a.f32le"));
    options.seconds = 2;
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
    /* HDR reads its own variable and never the SDR one. */
    env = (RecorderOptions){ .hdr = true };
    recorder_options_from_env(&env);
    CHECK(!strcmp(env.codec_args, RECORDER_HDR_CODEC_ARGS) && env.white_nits == 203);
    setenv("MYNES_RECORD_HDR_CODEC_ARGS", "-c:v libx265 -pix_fmt yuv420p10le", 1);
    env = (RecorderOptions){ .hdr = true, .white_nits = 100 };
    recorder_options_from_env(&env);
    CHECK(!strcmp(env.codec_args, "-c:v libx265 -pix_fmt yuv420p10le") && env.white_nits == 100);
    env = (RecorderOptions){0};
    recorder_options_from_env(&env);
    CHECK(!strcmp(env.codec_args, "-c:v prores_ks"));
    unsetenv("MYNES_FFMPEG"); unsetenv("MYNES_RECORD_CODEC_ARGS"); unsetenv("MYNES_RECORD_HDR_CODEC_ARGS");

    /* HDR: 16-bit PQ Y'CbCr in, tagged BT.2020 limited range in and out,
     * and no scale filter to convert it again. */
    options = (RecorderOptions){ .output = "out/clip.mov", .ffmpeg = "ffmpeg-test", .hdr = true,
        .seconds = 2, .codec_args = RECORDER_HDR_CODEC_ARGS, .region = 1, .width = 1920, .height = 1440 };
    CHECK(recorder_encode_command(&cmd, &options, "out/clip.mov.video.mov"));
    const char *const encode_hdr[] = { "ffmpeg-test", "-y", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "yuv444p16le", "-video_size", "1920x1440", "-r", "50.007",
        "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
        "-color_range", "tv", "-i", "-",
        "-c:v", "prores_ks", "-profile:v", "4", "-pix_fmt", "yuv444p10le", "-vendor", "apl0",
        "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
        "-color_range", "tv", "-an", "out/clip.mov.video.mov", NULL };
    if (!argv_equals(&cmd, encode_hdr)) { CHECK(!"HDR encode argv"); print_argv(&cmd); }
    CHECK(recorder_mux_command(&cmd, &options, "v.mov", "a.f32le"));
    if (!argv_equals(&cmd, mux)) { CHECK(!"HDR mux argv"); print_argv(&cmd); }
}

/* --record-hdr, --record-headroom and --record-hdr-white as main checks them. */
static void test_flags(void) {
    double v = 0;
    CHECK(recorder_parse_number("4", 1, 10000, &v) && v == 4);
    CHECK(recorder_parse_number("1.6", 1, 10000, &v) && fabs(v - 1.6) < 1e-12);
    CHECK(recorder_parse_number("10000", 1, 10000, &v) && v == 10000);
    v = 7;
    CHECK(!recorder_parse_number("0.5", 1, 10000, &v) && v == 7);
    CHECK(!recorder_parse_number("10001", 1, 10000, &v));
    CHECK(!recorder_parse_number("", 1, 10000, &v) && !recorder_parse_number("4x", 1, 10000, &v));
    CHECK(!recorder_parse_number("nan", 1, 10000, &v) && !recorder_parse_number("inf", 1, 10000, &v));

    CHECK(recorder_flags_error(true, true, false, 0, 0, NULL) == NULL);
    CHECK(recorder_flags_error(true, true, false, 4, 203, NULL) == NULL);
    CHECK(recorder_flags_error(true, false, false, 2, 0, NULL) == NULL);   /* SDR from an EDR render */
    CHECK(recorder_flags_error(true, false, true, 0, 0, NULL) == NULL);
    CHECK(recorder_flags_error(false, false, false, 0, 0, NULL) == NULL);
    const char *e;
    CHECK((e = recorder_flags_error(false, true, false, 0, 0, NULL)) && !strcmp(e, "--record-hdr requires --record"));
    CHECK((e = recorder_flags_error(true, true, true, 0, 0, NULL)) && strstr(e, "--sdr"));
    CHECK((e = recorder_flags_error(false, false, false, 4, 0, NULL)) && !strcmp(e, "--record-headroom requires --record"));
    CHECK((e = recorder_flags_error(true, false, true, 4, 0, NULL)) && strstr(e, "no effect with --sdr"));
    CHECK((e = recorder_flags_error(true, false, false, 0, 203, NULL)) && strstr(e, "requires --record-hdr"));
    /* The PQ peak, with the flag, the environment or the defaults. */
    CHECK(recorder_flags_error(true, true, false, 49.2, 0, NULL) == NULL);
    CHECK((e = recorder_flags_error(true, true, false, 49.3, 0, NULL)) && strstr(e, "10008 nits"));
    CHECK((e = recorder_flags_error(true, true, false, 0, 0, "60")) && strstr(e, "12180 nits"));
    CHECK(recorder_flags_error(true, true, false, 4, 0, "60") == NULL);   /* the flag wins */
    CHECK((e = recorder_flags_error(true, true, false, 0, 5000, NULL)) && strstr(e, "20000 nits"));
    CHECK(recorder_flags_error(true, false, false, 0, 0, "60") == NULL);  /* SDR has no PQ peak */

    /* Flag, then MYNES_OFFSCREEN_HEADROOM, then 4.0 for HDR and 1.6. */
    CHECK(recorder_offscreen_headroom(0, false, NULL) == 1.6f);
    CHECK(recorder_offscreen_headroom(0, true, NULL) == 4.0f);
    CHECK(recorder_offscreen_headroom(0, true, "2.5") == 2.5f);
    CHECK(recorder_offscreen_headroom(0, false, "0.5") == 1.0f);
    CHECK(recorder_offscreen_headroom(3, true, "2.5") == 3.0f);
    CHECK(recorder_offscreen_headroom(1.25, false, NULL) == 1.25f);
}

/* Options an HDR recording refuses before it starts ffmpeg. */
static void test_hdr_options(const char *directory) {
    char output[512], error[512];
    snprintf(output, sizeof(output), "%s/refused.mov", directory);
    RecorderOptions options = { .output = output, .seconds = 0.1, .region = 0, .width = W, .height = H,
        .hdr = true, .headroom = 4, .ffmpeg = "/nonexistent/ffmpeg" };
    options.white_nits = -203;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "at most 10000 nits"));
    options.white_nits = 10001;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "at most 10000 nits"));
    /* 49.3 x 203 reaches 10008 nits. */
    options.white_nits = 0; options.headroom = 49.3;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "above the 10000-nit PQ peak"));
    options.white_nits = 1000; options.headroom = 10;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "cannot run /nonexistent/ffmpeg"));
    options.white_nits = 1000; options.headroom = 10.5;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "10500 nits"));
    /* ProRes has no .mp4 mapping in ffmpeg. */
    snprintf(output, sizeof(output), "%s/refused.mp4", directory);
    options.white_nits = 0; options.headroom = 4;
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "needs a .mov output"));
    /* With a codec that fits .mp4 the options pass and only ffmpeg is missing. */
    options.codec_args = "-c:v libx265 -pix_fmt yuv420p10le";
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "cannot run /nonexistent/ffmpeg"));
    options.headroom = 49.2;   /* 9988 nits */
    CHECK(recorder_create(&options, error, sizeof(error)) == NULL && strstr(error, "cannot run /nonexistent/ffmpeg"));
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
    /* The sidecar of an SDR clip: no light levels, the render's headroom. */
    char json[600], text[1024];
    snprintf(json, sizeof(json), "%s/clip.json", directory);
    CHECK(!strcmp(read_text(json, text, sizeof(text)),
        "{\n  \"frames\": 30,\n  \"rate\": 60.0988,\n  \"width\": 64,\n  \"height\": 48,\n"
        "  \"hdr\": false,\n  \"white_nits\": 100,\n  \"headroom\": 1\n}\n"));
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
    /* The audio ends within one AAC frame of the video. */
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

    /* The default codec arguments produce the documented master format.
     * Two seconds with the worker's sample count: the muxed AAC track ends
     * 0.7 ms before the video, and every H.264 frame, with its reordering
     * delay, must still reach the file. */
    snprintf(output, sizeof(output), "%s/master.mp4", directory);
    options.output = output; options.codec_args = NULL; options.seconds = 2;
    options.headroom = 1.6f;   /* an EDR target, tone-mapped into the SDR file */
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        CHECK(recorder_frames(r) == 120);
        for (unsigned f = 0; f < 120; f++) {
            fill_frame(rgba, f, false);
            FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
            CHECK(recorder_push_frame(r, &image));
        }
        write_sine(recorder_audio_file(r), (unsigned)lround(120 * 44100 / 60.0988));
        finished = recorder_finish(r, error, sizeof(error));
        if (!finished) fprintf(stderr, "  finish: %s\n", error);
        CHECK(finished);
        recorder_destroy(r);
        CHECK(probe(output, "v:0", "codec_name,pix_fmt,nb_frames", info, sizeof(info)));
        CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "h264"));
        CHECK(probe_value(info, "pix_fmt", value, sizeof(value)) && !strcmp(value, "yuv444p"));
        CHECK(probe_value(info, "nb_frames", value, sizeof(value)) && atoi(value) == 120);
        if (atoi(value) != 120) fprintf(stderr, "  master.mp4 holds %s of 120 frames\n", value);
        snprintf(json, sizeof(json), "%s/master.json", directory);
        CHECK(strstr(read_text(json, text, sizeof(text)), "\"frames\": 120,") != NULL);
        CHECK(strstr(text, "\"headroom\": 1.6\n}") != NULL && !strstr(text, "max_cll"));
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
        snprintf(json, sizeof(json), "%s/short.json", directory);
        CHECK(!file_exists(json));   /* no sidecar without a clip */
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

/* An HDR clip of six frames, each four 16-pixel columns of half floats as
 * the target holds them; ProRes codes flat 8x8 blocks closely. The first
 * is SDR white, 4x white, BT.709 red and black. The clip's brightest pixel (5.0, 1015 nits) and its brightest
 * frame average (1.5, 1.5, 1.5 and 1.25: 291.8 nits) sit in two other
 * frames, and the last frame is dark, so neither the last frame's levels
 * nor an average over frames gives OUT.json's values. */
enum { HDR_FRAMES = 6 };
static const uint16_t hdr_columns[HDR_FRAMES][4][3] = {
    { { 0x3c00, 0x3c00, 0x3c00 }, { 0x4400, 0x4400, 0x4400 }, { 0x3c00, 0, 0 }, { 0, 0, 0 } },
    { { 0x3800, 0x3800, 0x3800 }, { 0x3400, 0x3400, 0x3400 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 0, 0, 0 }, { 0, 0, 0 }, { 0x4500, 0x4500, 0x4500 }, { 0, 0, 0 } },
    { { 0x3e00, 0x3e00, 0x3e00 }, { 0x3e00, 0x3e00, 0x3e00 }, { 0x3e00, 0x3e00, 0x3e00 },
      { 0x3d00, 0x3d00, 0x3d00 } },
    { { 0x3800, 0x3800, 0x3800 }, { 0x3400, 0x3400, 0x3400 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 0x3800, 0x3800, 0x3800 }, { 0x3400, 0x3400, 0x3400 }, { 0, 0, 0 }, { 0, 0, 0 } } };
static void push_hdr_clip(Recorder *r) {
    static uint16_t rgba[W * H * 4];
    for (unsigned f = 0; f < HDR_FRAMES; f++) {
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint16_t *px = rgba + ((size_t)y * W + x) * 4;
            memcpy(px, hdr_columns[f][x / 16], sizeof(hdr_columns[f][0]));
            px[3] = 0x3c00;
        }
        FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .hdr = true, .white_level = 1 };
        CHECK(recorder_push_frame(r, &image));
    }
}

/* OUT.json of that clip at 203 nits and headroom 5: max_cll 5 x 203 and
 * max_fall 203 x (1.5 x 3 + 1.25) / 4. */
static const char hdr_json[] =
    "{\n  \"frames\": 6,\n  \"rate\": 60.0988,\n  \"width\": 64,\n  \"height\": 48,\n"
    "  \"hdr\": true,\n  \"white_nits\": 203,\n  \"headroom\": 5,\n"
    "  \"max_cll\": 1015,\n  \"max_fall\": 292\n}\n";

static bool write_script(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(body, f) >= 0;
    return !fclose(f) && ok && chmod(path, 0755) == 0;
}

/* The sidecar needs no real ffmpeg: a stand-in that drains its stdin and
 * creates its last argument, the encode's video or the mux's output, lets
 * a recording run to OUT.json. */
static void test_sidecar(const char *directory) {
    char stub[512], failing_mux[512], output[512], json[512], text[1024], error[2048];
    snprintf(stub, sizeof(stub), "%s/ffmpeg-stub", directory);
    snprintf(failing_mux, sizeof(failing_mux), "%s/ffmpeg-mux-fails", directory);
    const char *body = "#!/bin/sh\ncat >/dev/null\nfor last; do :; done\n: > \"$last\"\n";
    char failing[256];
    snprintf(failing, sizeof(failing), "%scase \" $* \" in *\" -nostdin \"*) echo mux failed >&2; exit 1;; esac\n", body);
    CHECK(write_script(stub, body) && write_script(failing_mux, failing));

    snprintf(output, sizeof(output), "%s/stub.mov", directory);
    snprintf(json, sizeof(json), "%s/stub.json", directory);
    RecorderOptions options = { .output = output, .ffmpeg = stub, .seconds = 0.5, .region = 0,
        .width = W, .height = H, .headroom = 1.6 };
    Recorder *r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    static uint8_t rgba[W * H * 4];
    for (unsigned n = 0; n < FRAMES; n++) {
        fill_frame(rgba, n, false);
        FrameCaptureImage image = { .pixels = rgba, .width = W, .height = H, .white_level = 1 };
        CHECK(recorder_push_frame(r, &image));
    }
    write_sine(recorder_audio_file(r), (unsigned)lround(FRAMES * 44100 / 60.0988));
    bool finished = recorder_finish(r, error, sizeof(error));
    if (!finished) fprintf(stderr, "  finish: %s\n", error);
    CHECK(finished);
    recorder_destroy(r);
    /* An SDR clip: no light levels, the render's headroom. */
    CHECK(!strcmp(read_text(json, text, sizeof(text)),
        "{\n  \"frames\": 30,\n  \"rate\": 60.0988,\n  \"width\": 64,\n  \"height\": 48,\n"
        "  \"hdr\": false,\n  \"white_nits\": 100,\n  \"headroom\": 1.6\n}\n"));

    snprintf(output, sizeof(output), "%s/stub-hdr.mov", directory);
    snprintf(json, sizeof(json), "%s/stub-hdr.json", directory);
    options = (RecorderOptions){ .output = output, .ffmpeg = stub, .seconds = 0.1, .region = 0,
        .width = W, .height = H, .hdr = true, .headroom = 5 };
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    CHECK(recorder_frames(r) == HDR_FRAMES);
    push_hdr_clip(r);
    write_sine(recorder_audio_file(r), 4410);
    finished = recorder_finish(r, error, sizeof(error));
    if (!finished) fprintf(stderr, "  finish: %s\n", error);
    CHECK(finished);
    recorder_destroy(r);
    CHECK(!strcmp(read_text(json, text, sizeof(text)), hdr_json));
    if (strcmp(text, hdr_json)) fprintf(stderr, "  %s", text);

    /* A recording removes an older OUT.json when it starts, so one that
     * fails leaves none: here the mux fails after it has created the
     * output, as a full disk does. */
    options.ffmpeg = failing_mux;
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    CHECK(!file_exists(json));
    push_hdr_clip(r);
    write_sine(recorder_audio_file(r), 4410);
    CHECK(!recorder_finish(r, error, sizeof(error)) && strstr(error, "mux exited with status 1"));
    recorder_destroy(r);
    CHECK(file_exists(output) && !file_exists(json));
}

/* BT.2020 non-constant-luminance Y'CbCr of a PQ R'G'B' triple in 10-bit
 * limited-range codes (ITU-R BT.2100): Y' 64..940, Cb and Cr 64..960. */
static void ycbcr10(const double e[3], double out[3]) {
    double y = 0.2627 * e[0] + 0.6780 * e[1] + 0.0593 * e[2];
    out[0] = 64 + 876 * y;
    out[1] = 512 + 896 * (e[2] - y) / 1.8814;
    out[2] = 512 + 896 * (e[0] - y) / 1.4746;
}

/* An HDR clip through the default ProRes master and back: the stored
 * BT.2020 PQ codes, the stream tags, the frame count and the sidecar. */
static void test_hdr_end_to_end(const char *directory) {
    char output[512], decoded[600], json[600], text[1024], error[2048];
    snprintf(output, sizeof(output), "%s/hdr.mov", directory);
    snprintf(decoded, sizeof(decoded), "%s/hdr.yuv", directory);
    snprintf(json, sizeof(json), "%s/hdr.json", directory);
    RecorderOptions options = { .output = output, .seconds = 0.1, .after = 0, .region = 0,
        .width = W, .height = H, .hdr = true, .headroom = 5 };
    Recorder *r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (!r) { fprintf(stderr, "  %s\n", error); return; }
    static uint8_t sdr[W * H * 4];
    CHECK(recorder_frames(r) == HDR_FRAMES);
    push_hdr_clip(r);
    write_sine(recorder_audio_file(r), 4410);
    bool finished = recorder_finish(r, error, sizeof(error));
    if (!finished) fprintf(stderr, "  finish: %s\n", error);
    CHECK(finished);
    recorder_destroy(r);

    char info[2048], value[128];
    CHECK(probe(output, "v:0", "codec_name,codec_tag_string,profile,nb_frames,r_frame_rate,width,height,"
                "color_range,color_space,color_transfer,color_primaries", info, sizeof(info)));
    CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "prores"));
    CHECK(probe_value(info, "codec_tag_string", value, sizeof(value)) && !strcmp(value, "ap4h"));
    CHECK(probe_value(info, "profile", value, sizeof(value)) && !strcmp(value, "4444"));
    CHECK(probe_value(info, "nb_frames", value, sizeof(value)) && atoi(value) == 6);
    CHECK(probe_value(info, "width", value, sizeof(value)) && atoi(value) == W);
    CHECK(probe_value(info, "color_range", value, sizeof(value)) && !strcmp(value, "tv"));
    CHECK(probe_value(info, "color_space", value, sizeof(value)) && !strcmp(value, "bt2020nc"));
    CHECK(probe_value(info, "color_transfer", value, sizeof(value)) && !strcmp(value, "smpte2084"));
    CHECK(probe_value(info, "color_primaries", value, sizeof(value)) && !strcmp(value, "bt2020"));
    if (probe_value(info, "r_frame_rate", value, sizeof(value))) {
        long num = 0, den = 1;
        CHECK(sscanf(value, "%ld/%ld", &num, &den) == 2 && den > 0 && fabs((double)num / den - 60.0988) < 1e-4);
    } else CHECK(false);
    CHECK(probe(output, "a:0", "codec_name", info, sizeof(info)));
    CHECK(probe_value(info, "codec_name", value, sizeof(value)) && !strcmp(value, "aac"));

    /* The codes of the first frame as a player reads them: decoded in the
     * ProRes 4444 decoder's own 12 bits with no colour conversion, against
     * BT.2020 limited range worked out here. Black is 64/512/512, SDR white
     * Y' 573 and 4x white Y' 703 in 10 bits. A conversion back to RGB
     * through ffmpeg would undo a gain error in ffmpeg's forward step. */
    char command[2048];
    snprintf(command, sizeof(command), "ffmpeg -y -loglevel error -i '%s' -frames:v 1 "
             "-f rawvideo -pix_fmt yuv444p12le '%s'", output, decoded);
    CHECK(system(command) == 0);
    FILE *f = fopen(decoded, "rb");
    static uint8_t got[W * H * 6];
    CHECK(f && fread(got, 1, sizeof(got), f) == sizeof(got));
    if (f) fclose(f);
    float red[3], one[3] = { 1, 0, 0 };
    frame_pq_bt2020(one, red);
    const double pq[4][3] = {
        { frame_pq_encode(203), frame_pq_encode(203), frame_pq_encode(203) },
        { frame_pq_encode(812), frame_pq_encode(812), frame_pq_encode(812) },
        { frame_pq_encode(203 * red[0]), frame_pq_encode(203 * red[1]), frame_pq_encode(203 * red[2]) },
        { 0, 0, 0 } };
    double expected[4][3];
    for (int x = 0; x < 4; x++) ycbcr10(pq[x], expected[x]);
    CHECK(lround(expected[0][0]) == 573 && lround(expected[1][0]) == 703);
    CHECK(lround(expected[3][0]) == 64 && lround(expected[3][1]) == 512 && lround(expected[3][2]) == 512);
    /* ProRes leaves up to about one code of noise in these flat columns;
     * the column means carry the level. */
    double worst = 0, worst_mean = 0;
    for (int c = 0; c < 3; c++) for (int column = 0; column < 4; column++) {
        double sum = 0;
        for (int y = 0; y < H; y++) for (int x = column * 16; x < column * 16 + 16; x++) {
            size_t i = (((size_t)c * H + y) * W + x) * 2;
            double error = (got[i] | got[i + 1] << 8) / 4.0 - expected[column][c];
            worst = fmax(worst, fabs(error));
            sum += error;
        }
        worst_mean = fmax(worst_mean, fabs(sum / (16 * H)));
    }
    CHECK(worst_mean < 0.5 && worst < 1.5);
    fprintf(stderr, "recorder: HDR ProRes codes within %.2f of a 10-bit code, column means within %.2f\n",
            worst, worst_mean);

    CHECK(!strcmp(read_text(json, text, sizeof(text)), hdr_json));
    if (strcmp(text, hdr_json)) fprintf(stderr, "  %s", text);

    /* An 8-bit target cannot feed an HDR recording. */
    snprintf(output, sizeof(output), "%s/hdr-from-sdr.mov", directory);
    r = recorder_create(&options, error, sizeof(error));
    CHECK(r != NULL);
    if (r) {
        FrameCaptureImage eight = { .pixels = sdr, .width = W, .height = H, .white_level = 1 };
        CHECK(!recorder_push_frame(r, &eight));
        CHECK(!recorder_finish(r, error, sizeof(error)) && strstr(error, "half-float display target"));
        recorder_destroy(r);
        snprintf(json, sizeof(json), "%s/hdr-from-sdr.json", directory);
        CHECK(!file_exists(output) && !file_exists(json));
    }
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
    test_flags();
    test_hdr_options(directory);
    test_sidecar(directory);
    if (tool_available("ffmpeg") && tool_available("ffprobe")) {
        test_end_to_end(directory);
        test_hdr_end_to_end(directory);
    } else fprintf(stderr, "recorder: ffmpeg/ffprobe not on PATH, end-to-end muxing not tested\n");
    test_capture_window(directory);
    test_input_record(directory);
    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", directory);
    if (system(command)) {}
    SDL_Quit();
    printf("Recorder regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
