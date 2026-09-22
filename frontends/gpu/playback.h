/* Emulation/audio clock and a bounded three-picture queue. The main thread
 * owns the display and menus; this worker owns NES while playback is active. */
#ifndef GPU_PLAYBACK_H
#define GPU_PLAYBACK_H
#include "nes/nes.h"
#include "nes/rom.h"
#include "audio_gpu.h"

typedef struct Playback Playback;
typedef struct {
    AudioChain audio;
    APUAnalog analog;
    int region;
    bool gpu_audio;
    int presentation_mode;
    bool display_paced;
    float display_hz;
    uint8_t controller;
} PlaybackControls;
typedef struct {
    uint8_t rgb[256 * 240 * 3];
    uint16_t codes[256 * 240];
    unsigned number;
    int phase;
    uint16_t backdrop;
    float audio_energy;
    uint64_t emulation_ticks;
    uint64_t start_ns, ready_ns, audio_ns;
} PlaybackFrame;

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
bool playback_read(Playback *p, PlaybackFrame *frame);
void playback_destroy(Playback *p);
#endif
