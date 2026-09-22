/* Pixel-exact clip recorder for hidden (--offscreen) playback. Every
 * emulated frame becomes one rawvideo frame on an ffmpeg child's stdin
 * (rgb24, or rgb48 BT.2020 PQ for an HDR recording), the worker writes the
 * audio of the same frames to a temporary file, and a second ffmpeg run
 * muxes both into the requested .mov/.mp4. OUT.json beside the clip records
 * its frame count, rate, size and light levels. The ffmpeg command lines,
 * the frame arithmetic and the input recorder are plain functions so they
 * can be tested without a GPU. */
#ifndef GPU_RECORDER_H
#define GPU_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "frame_capture.h"

/* Portable near-lossless master. MYNES_RECORD_CODEC_ARGS replaces the whole
 * string; "-c:v h264_videotoolbox -b:v 90M -pix_fmt yuv420p" is the fast
 * hardware option on macOS. */
#define RECORDER_DEFAULT_CODEC_ARGS "-c:v libx264 -preset veryfast -crf 12 -pix_fmt yuv444p"
/* HDR master: 10-bit 4:4:4 ProRes 4444, which QuickTime and editors read;
 * .mov only. MYNES_RECORD_HDR_CODEC_ARGS replaces it (MYNES_RECORD_CODEC_ARGS
 * is for SDR recordings only) and must choose a 10-bit or deeper format. */
#define RECORDER_HDR_CODEC_ARGS "-c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le -vendor apl0"
/* ITU-R BT.2408 reference white, where an HDR recording puts 1.0. */
#define RECORDER_HDR_WHITE_NITS 203.0

typedef struct {
    const char *output;      /* --record OUT: .mov or .mp4 */
    double      seconds;     /* --record-seconds N */
    unsigned    after;       /* --record-after F: emulated frames run before the first captured one */
    int         region;      /* SIGNAL_REGION_NTSC or SIGNAL_REGION_PAL, sets the frame rate */
    int         width, height;   /* the offscreen target, hence every video frame */
    const char *ffmpeg;      /* executable; NULL means MYNES_FFMPEG, else "ffmpeg" from PATH */
    const char *codec_args;  /* NULL means MYNES_RECORD_CODEC_ARGS (MYNES_RECORD_HDR_CODEC_ARGS
                              * for HDR), else the default */
    double      headroom;    /* the render's headroom (1.0 on an SDR target), for OUT.json */
    bool        hdr;         /* BT.2020 PQ frames from the half-float target */
    double      white_nits;  /* HDR: nits of SDR white (1.0); 0 means RECORDER_HDR_WHITE_NITS */
} RecorderOptions;

/* One ffmpeg argv with the storage its entries point into. */
#define RECORDER_ARGV_MAX 64
typedef struct {
    char  *argv[RECORDER_ARGV_MAX + 1];  /* NULL-terminated */
    int    argc;
    char   storage[4096];
    size_t used;
} RecorderCommand;

/* --- Frame arithmetic --- */

/* The region's frame rate exactly as ffmpeg is told it: 60.0988 or 50.007. */
const char *recorder_rate_string(int region);
double      recorder_rate(int region);
/* round(seconds * rate); zero for a duration that is not positive. */
unsigned    recorder_frame_count(double seconds, int region);

/* --- Command-line flags --- */

/* A --record-headroom or --record-hdr-white value: the whole text is a
 * finite number from min to max. */
bool        recorder_parse_number(const char *text, double min, double max, double *value);
/* The HDR recording flags against --record, --sdr and the 10000-nit PQ
 * peak: NULL when they can be used together, else the message. headroom and
 * white_nits are 0 when the flag was not given; headroom_env is
 * MYNES_OFFSCREEN_HEADROOM. */
const char *recorder_flags_error(bool record, bool hdr, bool sdr, double headroom, double white_nits,
                                 const char *headroom_env);
/* The hidden render's headroom: --record-headroom when given (above 0),
 * else MYNES_OFFSCREEN_HEADROOM (at least 1), else 4.0 for an HDR
 * recording and 1.6 otherwise. */
float       recorder_offscreen_headroom(double headroom, bool hdr, const char *env);

/* --- Command construction --- */

/* Fill the ffmpeg, codec_args and white_nits fields that are still unset
 * from the environment, then from the built-in defaults. */
void recorder_options_from_env(RecorderOptions *options);
/* Temporary files beside the output: <out>.video.<ext>, <out>.audio.f32le
 * and <out>.ffmpeg.log. False when the output is not .mov or .mp4. */
bool recorder_temp_paths(const char *output, char *video, char *audio, char *log, size_t n);
/* The sidecar: OUT.mov or OUT.mp4 gives OUT.json. False when the output is
 * not .mov or .mp4. */
bool recorder_json_path(const char *output, char *json, size_t n);
bool recorder_command_add(RecorderCommand *cmd, const char *arg);
/* Append a whitespace-separated argument string; false when it is empty. */
bool recorder_command_add_split(RecorderCommand *cmd, const char *args);
/* The encode run: rgb24 frames on stdin at the region rate, no audio. HDR
 * takes rgb48 PQ frames tagged BT.2020, converts them to YCbCr with the
 * BT.2020 matrix in limited range and tags the stream the same way. */
bool recorder_encode_command(RecorderCommand *cmd, const RecorderOptions *options,
                             const char *video_path);
/* The mux run: the encoded video plus float32 mono 44100 Hz audio into the
 * output as AAC 256k, cut to the length of options->seconds in whole
 * frames. False when that is not one frame. */
bool recorder_mux_command(RecorderCommand *cmd, const RecorderOptions *options,
                          const char *video_path, const char *audio_path);
/* One line for logs and error messages. */
void recorder_command_text(const RecorderCommand *cmd, char *out, size_t n);

/* --- Running ffmpeg --- */

/* Run one command to completion with stdin closed and both output streams
 * appended to log_path. False, with the log's tail in error, on any failure. */
bool recorder_run(const RecorderCommand *cmd, const char *log_path, char *error, size_t error_size);

/* --- The recorder proper --- */

typedef struct Recorder Recorder;

/* Starts the encoder and opens the audio file. NULL, with the reason in
 * error, when ffmpeg cannot start or the options are unusable. */
Recorder *recorder_create(const RecorderOptions *options, char *error, size_t error_size);
unsigned  recorder_first_frame(const Recorder *r);   /* after + 1 */
unsigned  recorder_last_frame(const Recorder *r);    /* after + frames */
unsigned  recorder_frames(const Recorder *r);
unsigned  recorder_frames_written(const Recorder *r);
/* Hand this to the playback worker for the recorded frames' audio. */
FILE     *recorder_audio_file(Recorder *r);
/* True when the worker's picture `number` (1-based) is part of the clip. */
bool      recorder_want_frame(const Recorder *r, unsigned number);
/* GPURenderCtx capture sink: converts one final display image to rgb24, or
 * to rgb48 PQ for HDR, and writes it to the encoder. user is the Recorder. */
bool      recorder_push_frame(void *user, const FrameCaptureImage *image);
bool      recorder_complete(const Recorder *r);
/* Ends the encoder, muxes with the audio, writes OUT.json and removes the
 * temporary files. Call after the worker has stopped writing audio. Fails
 * when a frame is missing, a write failed or either ffmpeg run did. */
bool      recorder_finish(Recorder *r, char *error, size_t error_size);
/* Releases everything; kills a still-running encoder and removes leftovers. */
void      recorder_destroy(Recorder *r);

/* --- Input recording ---
 * Rows "frame hexmask" in the replay format, one per emulated frame whose
 * player-1 mask differs from the previous row's. A row is held until the
 * frame number moves on, so several changes within one frame collapse to the
 * last one and the rows stay strictly ascending as the loader requires. */
typedef struct {
    FILE    *file;
    char     path[1024];
    unsigned written_frame;
    uint8_t  written_mask;
    bool     pending;
    unsigned pending_frame;
    uint8_t  pending_mask;
    unsigned rows;
} InputRecord;

bool input_record_open(InputRecord *r, const char *path);
/* frame: the next emulated frame (1-based, from the current time line's
 * start) that will sample `mask`. Returns false on a write error. */
bool input_record_update(InputRecord *r, unsigned frame, uint8_t mask);
/* A new time line (reset, state load): truncate and count from 1 again. */
bool input_record_restart(InputRecord *r);
/* Writes any held row. False on a write error; the record is closed anyway. */
bool input_record_close(InputRecord *r);

#endif /* GPU_RECORDER_H */
