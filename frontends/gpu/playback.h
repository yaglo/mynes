/* Emulation/audio clock and a bounded three-picture queue. The main thread
 * owns the display and menus; this worker owns NES while playback is active.
 * Low latency keeps at most one picture ahead of the display instead. */
#ifndef GPU_PLAYBACK_H
#define GPU_PLAYBACK_H
#include "nes/nes.h"
#include "nes/rom.h"
#include "audio_gpu.h"
#include <stdio.h>

typedef struct Playback Playback;
typedef struct {
    AudioChain audio;
    APUAnalog analog;
    int region;
    bool gpu_audio;
    int presentation_mode;
    bool display_paced;
    float display_hz;
    /* Fast-forward multiplier. Values below 1 (including a zero-initialised
     * struct) run at normal speed; above 1 the worker stops queueing audio
     * and stops waiting for the display. */
    float speed;
    /* Queue one picture ahead of a paced display instead of three, and hand
     * a free-running renderer the newest picture rather than the oldest.
     * Ignored by capture and review runs, whose frame sequences are compared
     * bit-for-bit against the three-picture FIFO. */
    bool low_latency;
    uint8_t controller[2];
} PlaybackControls;
typedef struct {
    uint8_t rgb[256 * 240 * 3];
    uint16_t codes[256 * 240];
    unsigned number;
    int phase;
    uint16_t backdrop;
    /* The border the 2C02 drew on each raster line, as 9-bit entries with
     * emphasis: [line][0] left of the picture (dots 49 to 64), [line][1]
     * right of it (from dot 321, and across the whole line on lines 240 and
     * 241). See playback.c, run_frame. */
    uint16_t border[242][2];
    float audio_energy;
    uint64_t emulation_ticks;
    uint64_t start_ns, ready_ns, audio_ns;
} PlaybackFrame;

/* The 9-bit entry the 2C02 puts out as its border now: the backdrop at
 * $3F00, or with rendering off and v in palette space the entry v points at,
 * after greyscale, with the emphasis bits. */
uint16_t playback_border_entry(const PPU *ppu);

Playback *playback_create(NES *nes, SDL_GPUDevice *gpu, AudioGPUChain *audio,
                          SDL_AudioStream *stream, unsigned frame_limit, unsigned capture_from);
void playback_controls(Playback *p, const PlaybackControls *controls);
/* Pausing waits for a frame boundary. Core/ROM mutation is safe after return. */
void playback_pause(Playback *p);
/* Both leave playback paused. Loading creates a fresh console; reset retains
 * the cartridge and RAM. The caller owns ROM storage and resumes afterward. */
void playback_load_cartridge(Playback *p, const ROM *rom, int region);
void playback_reset_console(Playback *p);
void playback_resume(Playback *p);
/* Run fn on the console from the caller's thread while playback continues:
 * waits for the frame in progress, holds the next one back, and returns
 * once fn is done. This is the only safe way to read or replace console
 * state (battery RAM, save states) without pausing. Returns the number of
 * frames emulated when fn ran, so a new time line (a loaded state) can be
 * numbered from there. */
unsigned playback_with_console(Playback *p, void (*fn)(NES *nes, void *user), void *user);
/* Frames whose controller input the worker has already sampled; the next
 * controls change is first seen by frame number this plus one. */
unsigned playback_frames_sampled(Playback *p);
/* Scripted player-1 input (rows "frame hexmask", ascending, at most 128;
 * the MYNES_REVIEW_INPUT_SCRIPT format), replacing any script loaded from
 * the environment. Before the worker first resumes. False with SDL_GetError. */
bool playback_load_input_script(Playback *p, const char *path);
/* Arm a no-drop capture window for a recording: from picture `first` on
 * the worker produces one picture at a time and waits for it to be read,
 * writes the audio of pictures first..last to `audio` (float32 mono at
 * AUDIO_STREAM_RATE) and stops after picture `last`. Scripted input rows
 * count from `first` (row 1 is picture `first`). Call while paused, before
 * those frames run. */
void playback_arm_capture(Playback *p, unsigned first, unsigned last, FILE *audio);
/* playback_with_console for a visit that starts a new time line (a loaded
 * save state). When fn returns true, the pictures and audio queued before
 * the visit are dropped and the audio fades back in, all before the hold
 * is released, so the first frame the worker emulates afterwards is the
 * next picture read, numbered one past the return value. When fn returns
 * false nothing is dropped. Playback stays paused or running as it was. */
unsigned playback_restart(Playback *p, bool (*fn)(NES *nes, void *user), void *user);
bool playback_read(Playback *p, PlaybackFrame *frame);
/* Pictures produced but not yet read; diagnostics and tests only. */
unsigned playback_queued(Playback *p);
void playback_destroy(Playback *p);
#endif
