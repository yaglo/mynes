#include "playback.h"
#include "audio_sync.h"
#include "signal_format.h"
#include "gpu_presentation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { PLAYBACK_PICTURES = 3 };

struct Playback {
    NES *nes;
    SDL_GPUDevice *gpu;
    AudioGPUChain *audio;
    SDL_AudioStream *stream;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    bool active, busy, stop;
    /* `busy` stays set from the first frame after resume until pause is
     * acknowledged; `emulating` covers only the unlocked stretch in which a
     * frame runs, and `hold` counts main-thread visits waiting for the
     * console (see playback_with_console). */
    bool emulating;
    unsigned hold;
    PlaybackControls controls;
    PlaybackFrame frames[PLAYBACK_PICTURES];
    unsigned first_picture, picture_count;
    unsigned number, limit, capture_from;
    /* `number` advances outside the lock while a frame runs; `produced` is
     * its copy taken under the lock once the picture is queued. */
    unsigned produced;
    unsigned review_start_frame;
    struct { unsigned frame, buttons; } review_events[128];
    unsigned review_event_count, review_event_index, review_buttons;
    /* Added to every script row: a recording's rows count from its first
     * captured picture rather than from power-on. */
    unsigned replay_offset;
    float input[AUDIO_BLOCK_CAPACITY], output[AUDIO_BLOCK_CAPACITY];
    /* On RF the set's sound detector hears the picture: the frame's
     * per-line level makes the block's buzz, added by either backend. */
    float aux[AUDIO_BLOCK_CAPACITY];
    bool aux_valid;
    AudioVideoFrame frame_load;
    int count, fade;
    float energy;
    AudioState state;
    AudioRateCtrl rate;
    /* capture takes every frame (MYNES_AUDIO_CAPTURE); capture_window only
     * the frames from capture_from on, for a recording. Both owned elsewhere
     * except capture, which this file opened. */
    FILE *capture, *capture_window, *trace;
    uint16_t border[242][2];   /* the frame in progress's border, see run_frame */
};

/* One frame, noting the border each raster line gets. Raster line r starts
 * at PPU dot 277 of PPU line r - 1, so the left border of line r (dots 49
 * to 64) is drawn in PPU line r - 1's dots 326 to 340 and the right border
 * (from dot 321) in line r's dots 257 to 267; lines 240 and 241 are border
 * across. When the PPU leaves line s the entry is taken for the left border
 * of raster line s + 1 and the right border of line s, so a palette write in
 * line s's horizontal blanking reaches its right border up to 70 dots early.
 * Line 241 is drawn after the frame completes at its dot 1 and takes the
 * entry then. */
static void run_frame(NES *nes, uint16_t border[242][2]) {
    PPU *ppu = &nes->ppu;
    ppu->frame_complete = false;
    int line = ppu->scanline;
    while (!ppu->frame_complete) {
        nes_step(nes);
        if (ppu->scanline != line) {
            uint16_t entry = ppu_backdrop_entry(ppu);
            int next = line == ppu->prerender_line ? 0 : line + 1;
            if (line < 242) border[line][1] = entry;
            if (next < 242) border[next][0] = entry;
            line = ppu->scanline;
        }
    }
    border[241][1] = ppu_backdrop_entry(ppu);
}

/* Deterministic controller replay for offscreen visual reviews. Each row is
 * an emulated frame number and a hexadecimal controller mask, held until the
 * next row. Parse before the worker starts; ordinary playback reads no file. */
static bool load_review_input(Playback *p, const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return SDL_SetError("Cannot open review input: %s", path);
    unsigned frame, buttons, previous = 0;
    int fields;
    p->review_event_count = p->review_event_index = p->review_buttons = 0;
    while ((fields = fscanf(file, "%u %x", &frame, &buttons)) == 2) {
        if (frame <= previous || buttons > 255 || p->review_event_count == 128) {
            fclose(file);
            return SDL_SetError("Review input requires ascending frames and 8-bit masks (max 128 rows)");
        }
        unsigned index = p->review_event_count++;
        p->review_events[index].frame = frame;
        p->review_events[index].buttons = buttons;
        previous = frame;
    }
    bool valid = fields == EOF && !ferror(file) && p->review_event_count != 0;
    fclose(file);
    return valid || SDL_SetError("Malformed or empty review input: %s", path);
}

/* Captures and scripted reviews compare frame sequences bit-for-bit, so
 * they always keep the three-picture FIFO whatever the controls ask for. */
static bool low_latency_active(const Playback *p) {
    return p->controls.low_latency && !p->capture_from &&
           !p->review_event_count && !p->review_start_frame;
}

/* Matched-refresh hold lets the display clock the worker; every other
 * pacing mode runs the emulation clock freely and the renderer catches up. */
static bool display_backpressure(const Playback *p) {
    return p->controls.display_paced && p->controls.speed <= 1;
}

static void sample(void *user, float value) {
    Playback *p = user;
    if (p->count < AUDIO_BLOCK_CAPACITY) p->input[p->count++] = value;
}

static void reset_audio(Playback *p) {
    if (p->stream) {
        SDL_ClearAudioStream(p->stream);
        SDL_SetAudioStreamFrequencyRatio(p->stream, 1);
    }
    memset(&p->rate, 0, sizeof(p->rate));
    memset(&p->state, 0, sizeof(p->state));
    p->count = 0;
    p->fade = AUDIO_STREAM_RATE / 200;
    p->energy = 0;
}

static void submit_audio(Playback *p, const PlaybackControls *c, Uint64 deadline) {
    if (!p->count) return;
    bool processed = false;
    /* Fast-forward keeps the filter state moving on the CPU but never queues:
     * the device drains in real time, so anything queued at 8x would only
     * pile up as latency. The GPU block's deadline wait is skipped as well. */
    bool fast = c->speed > 1;
    /* An expired GPU block never enters the stream. Reuse its buffers only
     * after the fence signals; CPU fallback starts with the same input state. */
    if (p->audio && p->audio->pending) audio_gpu_poll(p->audio, p->gpu, NULL, NULL);
    const float *aux = p->aux_valid ? p->aux : NULL;
    if (c->gpu_audio && !fast && p->audio && !p->audio->pending) {
        if (audio_gpu_begin(p->audio, p->gpu, &c->audio, &p->state, p->input, aux, p->count)) {
            Uint64 latest = SDL_GetTicksNS() + 3000000;
            if (deadline > 2000000 && deadline - 2000000 < latest) latest = deadline - 2000000;
            do {
                int result = audio_gpu_poll(p->audio, p->gpu, &p->state, p->output);
                if (result) { processed = result > 0; break; }
                if (SDL_GetTicksNS() >= latest) break;
                SDL_Delay(1);
            } while (true);
        }
    }
    if (!processed) audio_chain_process_aux(&c->audio, &p->state, p->input, aux, p->output, p->count);
    if (p->stream && !fast) {
        int queued = SDL_GetAudioStreamQueued(p->stream) / (int)sizeof(float);
        if (audio_sync_stale(queued, p->count)) {
            SDL_ClearAudioStream(p->stream);
            memset(&p->rate, 0, sizeof(p->rate));
            p->fade = AUDIO_STREAM_RATE / 200;
        }
    }
    for (int i = 0; i < p->count; ++i) {
        float v = fmaxf(-1, fminf(1, p->output[i] * 1.5f));
        p->energy = .9995f * p->energy + .0005f * v * v;
        if (p->fade > 0) v *= 1 - (float)p->fade-- / (AUDIO_STREAM_RATE / 200);
        p->output[i] = v;
    }
    if (p->capture) fwrite(p->output, sizeof(float), p->count, p->capture);
    /* number already counts the frame these samples belong to. */
    if (p->capture_window && p->number >= p->capture_from)
        fwrite(p->output, sizeof(float), p->count, p->capture_window);
    if (p->trace) fprintf(p->trace, "%u,%d,%d,%.7f,%d,%llu\n", p->number, p->count,
        p->stream ? SDL_GetAudioStreamQueued(p->stream) / (int)sizeof(float) : 0,
        p->stream ? SDL_GetAudioStreamFrequencyRatio(p->stream) : 1, processed,
        (unsigned long long)SDL_GetTicksNS());
    if (p->stream && !fast) SDL_PutAudioStreamData(p->stream, p->output, p->count * sizeof(float));
}

static int run(void *user) {
    Playback *p = user;
    Uint64 deadline = 0;
    int last_region = -1;
    bool last_fast = false;
    SDL_LockMutex(p->mutex);
    while (!p->stop) {
        if (!p->active) {
            if (p->busy) reset_audio(p);
            p->busy = false;
            deadline = 0;
            last_region = -1;
            last_fast = false;
            SDL_BroadcastCondition(p->condition);
            SDL_WaitCondition(p->condition, p->mutex);
            continue;
        }
        /* A main-thread visit owns the console until it releases the hold;
         * the deadline logic below absorbs the few milliseconds it takes. */
        if (p->hold) {
            SDL_WaitCondition(p->condition, p->mutex);
            continue;
        }
        /* Offline paired screenshots must consume both actual PPU frames.
         * Normal playback retains a few consecutive phases across short UI stalls.
         * Matched-refresh hold applies backpressure instead of dropping a phase;
         * low latency tightens that to one picture so the frame the renderer
         * takes was emulated right after the previous one was shown.
         * Fast-forward outruns the display on purpose and overwrites the oldest.
         * Pause/stop still wake this wait; other modes keep the audio clock free. */
        unsigned hold = low_latency_active(p) ? 1 : PLAYBACK_PICTURES;
        bool capture_wait = p->capture_from && p->number >= p->capture_from && p->picture_count;
        if (capture_wait || (display_backpressure(p) && p->picture_count >= hold)) {
            /* Captures run as fast as their offscreen reader. Display
             * backpressure keeps the deadline: it is the ceiling below. */
            if (capture_wait) deadline = 0;
            SDL_WaitCondition(p->condition, p->mutex);
            continue;
        }
        PlaybackControls c = p->controls;
        Uint64 now = SDL_GetTicksNS();
        Uint64 native_period = (Uint64)(signal_region_frame_ms(c.region) * 1000000.0);
        Uint64 period = gpu_presentation_playback_period(c.presentation_mode, native_period,
            c.display_hz, c.display_paced);
        bool fast = c.speed > 1;
        if (fast) period = (Uint64)((double)period / c.speed);
        /* A matched display clocks the worker through its reads, but a macOS
         * window that is covered or minimized hands out drawables at about
         * 120 Hz. Follow reads at most one period early and never faster
         * than the period; a late read rebases instead of banking catch-up
         * frames, so visible play starts each frame right after the read. */
        bool paced = display_backpressure(p);
        if (!deadline || now > deadline + 3 * period || (paced && now > deadline)) deadline = now;
        Uint64 lead = paced ? period : 0;
        if (now + lead < deadline) {
            Sint32 remaining_ms=(Sint32)((deadline-lead-now+999999)/1000000);
            SDL_WaitConditionTimeout(p->condition, p->mutex, remaining_ms);
            continue;
        }
        /* 0.4% headroom: a panel whose vblank runs slightly faster than its
         * nominal rate never meets the ceiling, and a run held at the ceiling
         * stays inside the audio rate controller's 0.5% range. */
        deadline += paced ? period - period / 256 : period;
        p->busy = true;
        p->emulating = true;
        SDL_UnlockMutex(p->mutex);
        if (c.region != last_region) {
            ppu_set_region(&p->nes->ppu, c.region ? PPU_REGION_PAL : PPU_REGION_NTSC);
            apu_set_region(&p->nes->apu, c.region);
            last_region = c.region;
        }
        while (p->review_event_index < p->review_event_count &&
               p->review_events[p->review_event_index].frame + p->replay_offset <= p->number + 1) {
            p->review_buttons = p->review_events[p->review_event_index++].buttons;
        }
        p->nes->controller[0] = p->review_event_count ? p->review_buttons : c.controller[0];
        p->nes->controller[1] = c.controller[1];
        if (p->review_start_frame && p->number+1 >= p->review_start_frame
            && p->number+1 < p->review_start_frame+2) p->nes->controller[0] |= 0x08;
        bool dac_changed = p->nes->apu.analog.dac_nonlinearity != c.analog.dac_nonlinearity;
        p->nes->apu.analog = c.analog;
        if (dac_changed) apu_build_dac_tables(&p->nes->apu);
        p->nes->apu.filter_config = (APUFilterConfig){1, 1, 1};
        p->nes->apu.sample_rate = AUDIO_STREAM_RATE;
        if (p->stream && last_fast && !fast) {
            /* Whatever was queued before fast-forward is stale by now. Restart
             * the rate controller and fade so the resampler settles from 1.0
             * rather than chasing the emptied queue. */
            SDL_ClearAudioStream(p->stream);
            SDL_SetAudioStreamFrequencyRatio(p->stream, 1);
            memset(&p->rate, 0, sizeof(p->rate));
            p->fade = AUDIO_STREAM_RATE / 200;
        }
        last_fast = fast;
        if (p->stream && !fast) {
            int queued = SDL_GetAudioStreamQueued(p->stream);
            if (queued >= 0) SDL_SetAudioStreamFrequencyRatio(p->stream,
                audio_sync_ratio(&p->rate, queued / (int)sizeof(float), period / 1e9)
                * (float)((double)native_period / period));
        }
        p->count = 0;
        Uint64 start_ns = SDL_GetTicksNS(), start = SDL_GetPerformanceCounter();
        run_frame(p->nes, p->border);
        Uint64 duration = SDL_GetPerformanceCounter() - start;
        ++p->number;
        Uint64 audio_start=SDL_GetTicksNS();
        p->aux_valid = c.audio.rf_sound.enabled && p->count > 0;
        if (p->aux_valid) {
            audio_video_frame_from_codes(&p->frame_load, p->nes->ppu.index_framebuffer, c.region);
            audio_chain_rf_buzz(&c.audio, &p->frame_load, p->aux, p->count);
        }
        submit_audio(p, &c, deadline);
        Uint64 ready_ns=SDL_GetTicksNS();
        SDL_LockMutex(p->mutex);
        p->emulating = false;
        p->produced = p->number;
        if (p->capture_from && p->number <= p->capture_from)
            p->picture_count = p->first_picture = 0;
        if (p->picture_count == PLAYBACK_PICTURES) {
            p->first_picture = (p->first_picture + 1) % PLAYBACK_PICTURES;
            p->picture_count--;
        }
        PlaybackFrame *picture = &p->frames[(p->first_picture + p->picture_count) % PLAYBACK_PICTURES];
        memcpy(picture->rgb, p->nes->ppu.framebuffer, sizeof(picture->rgb));
        memcpy(picture->codes, p->nes->ppu.index_framebuffer, sizeof(picture->codes));
        picture->number = p->number;
        picture->backdrop = (p->nes->ppu.palette[0] & (p->nes->ppu.mask & 1 ? 0x30 : 0x3f))
                         | ((p->nes->ppu.mask & 0xe0) << 1);
        memcpy(picture->border, p->border, sizeof(picture->border));
        picture->audio_energy = p->energy;
        picture->emulation_ticks = duration;
        picture->start_ns=start_ns;
        picture->ready_ns=ready_ns;
        picture->audio_ns=ready_ns-audio_start;
        /* Convert the region's hardware master divider to PPU dots. */
        uint64_t dots = p->nes->ppu.next_dot_master_tick / (c.region ? 5 : 4);
        uint64_t position = (uint64_t)p->nes->ppu.scanline * 341 + p->nes->ppu.dot;
        picture->phase = dots >= position ? (int)(((dots - position) % 12) * (c.region ? 10 : 8) % 12) : -1;
        p->picture_count++;
        if (p->limit && p->number >= p->limit) p->active = false;
        SDL_BroadcastCondition(p->condition);
    }
    SDL_UnlockMutex(p->mutex);
    return 0;
}

Playback *playback_create(NES *nes, SDL_GPUDevice *gpu, AudioGPUChain *audio,
                          SDL_AudioStream *stream, unsigned frame_limit, unsigned capture_from) {
    Playback *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->nes = nes; p->gpu = gpu; p->audio = audio; p->stream = stream; p->limit = frame_limit; p->capture_from = capture_from;
    const char *start_frame=getenv("MYNES_REVIEW_START_FRAME");
    if(start_frame) p->review_start_frame=(unsigned)strtoul(start_frame,NULL,10);
    const char *review_input = getenv("MYNES_REVIEW_INPUT_SCRIPT");
    if (review_input && !load_review_input(p, review_input)) { free(p); return NULL; }
    p->mutex = SDL_CreateMutex();
    p->condition = SDL_CreateCondition();
    const char *capture = getenv("MYNES_AUDIO_CAPTURE"), *trace = getenv("MYNES_AUDIO_TRACE");
    if (capture) p->capture = fopen(capture, "wb");
    if (trace) p->trace = fopen(trace, "w");
    if (p->trace) fprintf(p->trace, "frame,samples,queued,ratio,gpu,time_ns\n");
    reset_audio(p);
    apu_set_audio_callback(&nes->apu, sample, p);
    if (p->mutex && p->condition) p->thread = SDL_CreateThread(run, "NES playback", p);
    if (!p->thread) { playback_destroy(p); return NULL; }
    return p;
}

void playback_controls(Playback *p, const PlaybackControls *controls) {
    SDL_LockMutex(p->mutex);
    /* Backpressure and the deadline wait both depend on these; wake the
     * worker so a speed change takes effect on the next frame, not the next
     * picture read. */
    bool pacing_changed = p->controls.display_paced != controls->display_paced ||
                          p->controls.speed != controls->speed ||
                          p->controls.low_latency != controls->low_latency;
    p->controls = *controls;
    if (pacing_changed) SDL_BroadcastCondition(p->condition);
    SDL_UnlockMutex(p->mutex);
}
void playback_pause(Playback *p) {
    SDL_LockMutex(p->mutex);
    p->active = false;
    SDL_BroadcastCondition(p->condition);
    while (p->busy) SDL_WaitCondition(p->condition, p->mutex);
    p->picture_count = p->first_picture = 0;
    SDL_UnlockMutex(p->mutex);
}
void playback_resume(Playback *p) {
    SDL_LockMutex(p->mutex);
    p->active = !p->limit || p->number < p->limit;
    SDL_BroadcastCondition(p->condition);
    SDL_UnlockMutex(p->mutex);
}
/* Returns with the mutex locked and no frame in progress; the hold keeps
 * the worker from starting one while the wait releases the mutex. */
static void hold_console(Playback *p) {
    SDL_LockMutex(p->mutex);
    p->hold++;
    while (p->emulating) SDL_WaitCondition(p->condition, p->mutex);
}
static unsigned release_console(Playback *p) {
    unsigned frames = p->produced;
    p->hold--;
    SDL_BroadcastCondition(p->condition);
    SDL_UnlockMutex(p->mutex);
    return frames;
}
unsigned playback_with_console(Playback *p, void (*fn)(NES *nes, void *user), void *user) {
    hold_console(p);
    fn(p->nes, user);
    return release_console(p);
}
unsigned playback_frames_sampled(Playback *p) {
    SDL_LockMutex(p->mutex);
    /* A frame in progress took its controls before emulating was set. */
    unsigned frames = p->produced + (p->emulating ? 1 : 0);
    SDL_UnlockMutex(p->mutex);
    return frames;
}
bool playback_load_input_script(Playback *p, const char *path) {
    return load_review_input(p, path);
}
void playback_arm_capture(Playback *p, unsigned first, unsigned last, FILE *audio) {
    SDL_LockMutex(p->mutex);
    p->capture_from = first;
    p->limit = last;
    p->capture_window = audio;
    p->replay_offset = first ? first - 1 : 0;
    SDL_UnlockMutex(p->mutex);
}
unsigned playback_restart(Playback *p, bool (*fn)(NES *nes, void *user), void *user) {
    hold_console(p);
    /* Pictures already queued show the time line before the jump; the audio
     * queued for them would play over the restored machine's first frames.
     * Both go before the hold is released: a frame the worker ran in
     * between would come from the new time line and be dropped with them. */
    if (fn(p->nes, user)) {
        p->picture_count = p->first_picture = 0;
        reset_audio(p);
    }
    return release_console(p);
}
void playback_load_cartridge(Playback *p, const ROM *rom, int region) {
    playback_pause(p);
    /* A reset flag alone cannot escape a jammed CPU microinstruction, and
     * leaves the old cartridge's DMA/NMI/bus state alive. Cartridge changes
     * start a new machine while retaining the worker and its audio stream. */
    nes_init(p->nes);
    nes_load_mapper(p->nes,rom->mapper,rom->prg_rom,rom->prg_size,
                    rom->chr_rom,rom->chr_size,rom->mirroring);
    nes_rom_apply_trainer(rom,&p->nes->mapper);
    ppu_set_region(&p->nes->ppu,region ? PPU_REGION_PAL : PPU_REGION_NTSC);
    nes_reset(p->nes);
    apu_set_region(&p->nes->apu,region);
    apu_set_audio_callback(&p->nes->apu,sample,p);
    reset_audio(p);
}
void playback_reset_console(Playback *p) {
    playback_pause(p);
    NES *nes=p->nes;
    /* Frontend reset: nes_reset restarts the CPU reset sequence even from
     * KIL, retaining cartridge/work RAM and the shared master-clock timeline.
     * The frontend also drops the strobe and resets the mapper. */
    nes->controller_strobe=0;
    if(nes->mapper_loaded) {
        mapper_reset(&nes->mapper);
        nes->mapper.irq_pending=false;
        nes->ppu.mirroring=mapper_get_mirroring(&nes->mapper);
    }
    nes_reset(nes);
    reset_audio(p);
}
bool playback_read(Playback *p, PlaybackFrame *frame) {
    SDL_LockMutex(p->mutex);
    bool fresh = p->picture_count != 0;
    if (fresh) {
        /* A free-running worker may be several pictures ahead of a renderer
         * that stalled. Low latency shows the newest and drops the rest; the
         * renderer's elapsed-frame count keeps phosphor decay honest. Under
         * display backpressure the queue never holds a stale picture, so
         * oldest-first keeps consecutive carrier phases there. */
        unsigned skip = low_latency_active(p) && !display_backpressure(p)
                      ? p->picture_count - 1 : 0;
        p->first_picture = (p->first_picture + skip) % PLAYBACK_PICTURES;
        p->picture_count -= skip;
        *frame = p->frames[p->first_picture];
        p->first_picture = (p->first_picture + 1) % PLAYBACK_PICTURES;
        p->picture_count--;
        SDL_BroadcastCondition(p->condition);
    }
    SDL_UnlockMutex(p->mutex);
    return fresh;
}
unsigned playback_queued(Playback *p) {
    SDL_LockMutex(p->mutex);
    unsigned queued = p->picture_count;
    SDL_UnlockMutex(p->mutex);
    return queued;
}
void playback_destroy(Playback *p) {
    if (!p) return;
    if (p->thread) {
        SDL_LockMutex(p->mutex); p->stop = true;
        SDL_BroadcastCondition(p->condition); SDL_UnlockMutex(p->mutex);
        SDL_WaitThread(p->thread, NULL);
    }
    apu_set_audio_callback(&p->nes->apu, NULL, NULL);
    if (p->capture) fclose(p->capture);
    if (p->trace) fclose(p->trace);
    SDL_DestroyCondition(p->condition); SDL_DestroyMutex(p->mutex);
    free(p);
}
