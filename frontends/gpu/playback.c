#include "playback.h"
#include "audio_sync.h"
#include "signal_format.h"
#include "gpu_presentation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct Playback {
    NES *nes;
    SDL_GPUDevice *gpu;
    AudioGPUChain *audio;
    SDL_AudioStream *stream;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    bool active, busy, stop, fresh;
    PlaybackControls controls;
    PlaybackFrame frame;
    unsigned number, limit, capture_from;
    unsigned review_start_frame;
    struct { unsigned frame, buttons; } review_events[128];
    unsigned review_event_count, review_event_index, review_buttons;
    float input[AUDIO_BLOCK_CAPACITY], output[AUDIO_BLOCK_CAPACITY];
    int count, fade;
    float energy;
    AudioState state;
    AudioRateCtrl rate;
    FILE *capture, *trace;
};

/* Deterministic controller replay for offscreen visual reviews. Each row is
 * an emulated frame number and a hexadecimal controller mask, held until the
 * next row. Parse before the worker starts; ordinary playback reads no file. */
static bool load_review_input(Playback *p, const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return SDL_SetError("Cannot open review input: %s", path);
    unsigned frame, buttons, previous = 0;
    int fields;
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
    /* An expired GPU block never enters the stream. Reuse its buffers only
     * after the fence signals; CPU fallback starts with the same input state. */
    if (p->audio && p->audio->pending) audio_gpu_poll(p->audio, p->gpu, NULL, NULL);
    if (c->gpu_audio && p->audio && !p->audio->pending) {
        if (audio_gpu_begin(p->audio, p->gpu, &c->audio, &p->state, p->input, p->count)) {
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
    if (!processed) audio_chain_process(&c->audio, &p->state, p->input, p->output, p->count);
    if (p->stream) {
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
    if (p->trace) fprintf(p->trace, "%u,%d,%d,%.7f,%d,%llu\n", p->number, p->count,
        p->stream ? SDL_GetAudioStreamQueued(p->stream) / (int)sizeof(float) : 0,
        p->stream ? SDL_GetAudioStreamFrequencyRatio(p->stream) : 1, processed,
        (unsigned long long)SDL_GetTicksNS());
    if (p->stream) SDL_PutAudioStreamData(p->stream, p->output, p->count * sizeof(float));
}

static int run(void *user) {
    Playback *p = user;
    Uint64 deadline = 0;
    int last_region = -1;
    SDL_LockMutex(p->mutex);
    while (!p->stop) {
        if (!p->active) {
            if (p->busy) reset_audio(p);
            p->busy = false;
            deadline = 0;
            last_region = -1;
            SDL_BroadcastCondition(p->condition);
            SDL_WaitCondition(p->condition, p->mutex);
            continue;
        }
        /* Offline paired screenshots must consume both actual PPU frames.
         * Normal playback always overwrites the mailbox with the newest frame. */
        if (p->capture_from && p->number >= p->capture_from && p->fresh) {
            SDL_WaitCondition(p->condition, p->mutex);
            continue;
        }
        PlaybackControls c = p->controls;
        Uint64 now = SDL_GetTicksNS();
        Uint64 native_period = (Uint64)(signal_region_frame_ms(c.region) * 1000000.0);
        Uint64 period = gpu_presentation_period_ns(c.presentation_mode, native_period);
        if (!deadline || now > deadline + 3 * period) deadline = now;
        if (now < deadline) {
            Sint32 remaining_ms=(Sint32)((deadline-now+999999)/1000000);
            SDL_WaitConditionTimeout(p->condition, p->mutex, remaining_ms);
            continue;
        }
        deadline += period;
        p->busy = true;
        SDL_UnlockMutex(p->mutex);
        if (c.region != last_region) {
            ppu_set_region(&p->nes->ppu, c.region ? PPU_REGION_PAL : PPU_REGION_NTSC);
            apu_set_region(&p->nes->apu, c.region);
            last_region = c.region;
        }
        while (p->review_event_index < p->review_event_count &&
               p->review_events[p->review_event_index].frame <= p->number + 1) {
            p->review_buttons = p->review_events[p->review_event_index++].buttons;
        }
        p->nes->controller[0] = p->review_event_count ? p->review_buttons : c.controller;
        if (p->review_start_frame && p->number+1 >= p->review_start_frame
            && p->number+1 < p->review_start_frame+2) p->nes->controller[0] |= 0x08;
        bool dac_changed = p->nes->apu.analog.dac_nonlinearity != c.analog.dac_nonlinearity;
        p->nes->apu.analog = c.analog;
        if (dac_changed) apu_build_dac_tables(&p->nes->apu);
        p->nes->apu.filter_config = (APUFilterConfig){1, 1, 1};
        p->nes->apu.sample_rate = AUDIO_STREAM_RATE;
        if (p->stream) {
            int queued = SDL_GetAudioStreamQueued(p->stream);
            if (queued >= 0) SDL_SetAudioStreamFrequencyRatio(p->stream,
                audio_sync_ratio(&p->rate, queued / (int)sizeof(float), period / 1e9)
                * (float)((double)native_period / period));
        }
        p->count = 0;
        Uint64 start_ns = SDL_GetTicksNS(), start = SDL_GetPerformanceCounter();
        uint64_t cpu_before=p->nes->cpu.cycles;
        uint64_t dots_before=p->nes->ppu.next_dot_master_tick/4;
        nes_run_frame(p->nes);
        Uint64 duration = SDL_GetPerformanceCounter() - start;
        /* Convert APU cycles to wall-clock audio using the core's observed
         * CPU/PPU cadence. The current core advances three dots per CPU cycle
         * in PAL too; assuming hardware's 16:5 ratio overproduces audio by
         * 6.67%. This adapter fixes stream duration, not core PAL timing. */
        uint64_t dots=p->nes->ppu.next_dot_master_tick/4-dots_before;
        uint64_t cycles=p->nes->cpu.cycles-cpu_before;
        if(dots && cycles) {
            double dot_hz=signal_region_sample_rate_hz(c.region)/(c.region ? 10 : 8);
            int clock=(int)llround(cycles*dot_hz/dots);
            if(clock>1000000 && clock<2500000 && abs(clock-p->nes->apu.cpu_clock)>200) {
                p->nes->apu.sample_accumulator=(int)((int64_t)p->nes->apu.sample_accumulator*clock/p->nes->apu.cpu_clock);
                p->nes->apu.cpu_clock=clock;
            }
        }
        ++p->number;
        Uint64 audio_start=SDL_GetTicksNS();
        submit_audio(p, &c, deadline);
        Uint64 ready_ns=SDL_GetTicksNS();
        SDL_LockMutex(p->mutex);
        memcpy(p->frame.rgb, p->nes->ppu.framebuffer, sizeof(p->frame.rgb));
        memcpy(p->frame.codes, p->nes->ppu.index_framebuffer, sizeof(p->frame.codes));
        p->frame.number = p->number;
        p->frame.backdrop = (p->nes->ppu.palette[0] & (p->nes->ppu.mask & 1 ? 0x30 : 0x3f))
                         | ((p->nes->ppu.mask & 0xe0) << 1);
        p->frame.audio_energy = p->energy;
        p->frame.emulation_ticks = duration;
        p->frame.start_ns=start_ns;
        p->frame.ready_ns=ready_ns;
        p->frame.audio_ns=ready_ns-audio_start;
        /* The core's master-tick unit is four ticks per dot in both regions. */
        dots = p->nes->ppu.next_dot_master_tick / 4;
        uint64_t position = (uint64_t)p->nes->ppu.scanline * 341 + p->nes->ppu.dot;
        p->frame.phase = dots >= position ? (int)(((dots - position) % 12) * (c.region ? 10 : 8) % 12) : -1;
        p->fresh = true;
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
    p->controls = *controls;
    SDL_UnlockMutex(p->mutex);
}
void playback_pause(Playback *p) {
    SDL_LockMutex(p->mutex);
    p->active = false;
    SDL_BroadcastCondition(p->condition);
    while (p->busy) SDL_WaitCondition(p->condition, p->mutex);
    p->fresh = false;
    SDL_UnlockMutex(p->mutex);
}
void playback_resume(Playback *p) {
    SDL_LockMutex(p->mutex);
    p->active = !p->limit || p->number < p->limit;
    SDL_BroadcastCondition(p->condition);
    SDL_UnlockMutex(p->mutex);
}
bool playback_read(Playback *p, PlaybackFrame *frame) {
    SDL_LockMutex(p->mutex);
    bool fresh = p->fresh;
    if (fresh) { *frame = p->frame; p->fresh = false; SDL_BroadcastCondition(p->condition); }
    SDL_UnlockMutex(p->mutex);
    return fresh;
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
