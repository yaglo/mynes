/*
 * Debug Tap Implementation — GPU Buffer Readback for SwiftUI Visualiser
 * =====================================================================
 *
 * Manages per-stage GPU buffer readback for debugging and visualization.
 * Buffers are allocated lazily on first capture; disabled taps have zero cost.
 */

#include "debug_tap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Tap Manager State
 * ============================================================================ */

typedef struct {
    /* Ownership: both chains are pointers to external structs, not copied. */
    SDL_GPUDevice *gpu;
    SignalChain *video_chain;
    SignalChain *audio_chain;

    /* Tap points: one per stage across both chains. */
    DebugTap *taps;
    int num_taps;
    int capacity;

    /* Enable/disable per stage. */
    bool *enabled;

    /* CPU-side buffers (allocated lazily per tap). */
    float **buffers;

    /* Timing statistics. */
    int total_captures;
    double total_readback_us;
    double peak_readback_us;
    double avg_readback_us;
} DebugTapManagerImpl;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

DebugTapManager *debug_tap_create(SDL_GPUDevice *gpu,
                                   SignalChain *video_chain,
                                   SignalChain *audio_chain) {
    DebugTapManagerImpl *mgr = (DebugTapManagerImpl *)calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;

    mgr->gpu = gpu;
    mgr->video_chain = video_chain;
    mgr->audio_chain = audio_chain;

    /* Calculate total number of stages. */
    int video_stages = video_chain ? video_chain->num_stages : 0;
    int audio_stages = audio_chain ? audio_chain->num_stages : 0;
    int total_stages = video_stages + audio_stages;

    if (total_stages == 0) {
        /* No chains provided. Manager is still valid but empty. */
        return (DebugTapManager *)mgr;
    }

    /* Allocate tap points and state. */
    mgr->capacity = total_stages;
    mgr->taps = (DebugTap *)calloc(total_stages, sizeof(DebugTap));
    mgr->enabled = (bool *)calloc(total_stages, sizeof(bool));
    mgr->buffers = (float **)calloc(total_stages, sizeof(float *));

    if (!mgr->taps || !mgr->enabled || !mgr->buffers) {
        free(mgr->taps);
        free(mgr->enabled);
        free(mgr->buffers);
        free(mgr);
        return NULL;
    }

    /* Initialize tap metadata from chains. */
    mgr->num_taps = 0;

    /* Video chain stages. */
    if (video_chain) {
        for (int i = 0; i < video_chain->num_stages; i++) {
            DebugTap *tap = &mgr->taps[mgr->num_taps];
            const ChainStage *stage = &video_chain->stages[i];

            tap->stage_index = mgr->num_taps;
            tap->stage_name = stage->name;
            tap->chain_name = "video";
            tap->data = NULL;
            tap->sample_count = 0;
            tap->samples_per_line = 0;
            tap->lines = 240;
            tap->frame_number = 0;
            tap->capture_timestamp_ns = 0;
            tap->gpu_dispatch_us = 0.0;
            tap->readback_us = 0.0;
            tap->stage_enabled = stage->enabled;
            tap->stage_bypassed = stage->bypass;
            tap->is_valid = false;
            tap->error_msg = NULL;

            if (stage->params_size > 0 && stage->params_size <= 128) {
                memcpy(tap->stage_params, stage->params, stage->params_size);
                tap->stage_params_size = stage->params_size;
            }

            mgr->num_taps++;
        }
    }

    /* Audio chain stages. */
    if (audio_chain) {
        for (int i = 0; i < audio_chain->num_stages; i++) {
            DebugTap *tap = &mgr->taps[mgr->num_taps];
            const ChainStage *stage = &audio_chain->stages[i];

            tap->stage_index = mgr->num_taps;
            tap->stage_name = stage->name;
            tap->chain_name = "audio";
            tap->data = NULL;
            tap->sample_count = 0;
            tap->samples_per_line = 0;
            tap->lines = 1;  /* Audio is 1D. */
            tap->frame_number = 0;
            tap->capture_timestamp_ns = 0;
            tap->gpu_dispatch_us = 0.0;
            tap->readback_us = 0.0;
            tap->stage_enabled = stage->enabled;
            tap->stage_bypassed = stage->bypass;
            tap->is_valid = false;
            tap->error_msg = NULL;

            if (stage->params_size > 0 && stage->params_size <= 128) {
                memcpy(tap->stage_params, stage->params, stage->params_size);
                tap->stage_params_size = stage->params_size;
            }

            mgr->num_taps++;
        }
    }

    return (DebugTapManager *)mgr;
}

void debug_tap_destroy(DebugTapManager *mgr) {
    if (!mgr) return;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;

    /* Free all allocated tap buffers. */
    if (impl->buffers) {
        for (int i = 0; i < impl->num_taps; i++) {
            free(impl->buffers[i]);
        }
        free(impl->buffers);
    }

    free(impl->taps);
    free(impl->enabled);
    free(impl);
}

/* ============================================================================
 * Enable/Disable
 * ============================================================================ */

bool debug_tap_set_enabled(DebugTapManager *mgr, int stage_index, bool enabled) {
    if (!mgr) return false;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    if (stage_index < 0 || stage_index >= impl->num_taps) {
        return false;
    }

    SignalChain *chain = (strcmp(impl->taps[stage_index].chain_name, "video") == 0)
                         ? impl->video_chain
                         : impl->audio_chain;
    int chain_stage_idx = stage_index;
    if (chain == impl->audio_chain && impl->video_chain) {
        chain_stage_idx -= impl->video_chain->num_stages;
    }

    impl->enabled[stage_index] = enabled;
    if (chain && impl->gpu) {
        if (!chain_set_stage_capture(chain, impl->gpu, chain_stage_idx, enabled)) {
            impl->enabled[stage_index] = false;
            return false;
        }
    }

    /* If disabling, free the buffer. */
    if (!enabled && impl->buffers[stage_index]) {
        free(impl->buffers[stage_index]);
        impl->buffers[stage_index] = NULL;
    }

    return true;
}

void debug_tap_set_all_enabled(DebugTapManager *mgr, bool video_enabled,
                                bool audio_enabled) {
    if (!mgr) return;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;

    for (int i = 0; i < impl->num_taps; i++) {
        bool enable = (strcmp(impl->taps[i].chain_name, "video") == 0)
                      ? video_enabled
                      : audio_enabled;
        debug_tap_set_enabled(mgr, i, enable);
    }
}

bool debug_tap_is_enabled(const DebugTapManager *mgr, int stage_index) {
    if (!mgr) return false;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    if (stage_index < 0 || stage_index >= impl->num_taps) {
        return false;
    }

    return impl->enabled[stage_index];
}

int debug_tap_enabled_count(const DebugTapManager *mgr) {
    if (!mgr) return 0;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    int count = 0;

    for (int i = 0; i < impl->num_taps; i++) {
        if (impl->enabled[i]) count++;
    }

    return count;
}

/* ============================================================================
 * Capture and Readback
 * ============================================================================ */

void debug_tap_capture(DebugTapManager *mgr, SDL_GPUDevice *gpu,
                       uint64_t frame_number) {
    if (!mgr || !gpu) return;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    uint64_t capture_time_ns = SDL_GetTicksNS();
    uint64_t t0 = SDL_GetPerformanceCounter();

    for (int i = 0; i < impl->num_taps; i++) {
        if (!impl->enabled[i]) continue;

        DebugTap *tap = &impl->taps[i];
        SignalChain *chain = (strcmp(tap->chain_name, "video") == 0)
                             ? impl->video_chain
                             : impl->audio_chain;

        if (!chain) {
            tap->is_valid = false;
            tap->error_msg = "invalid chain";
            continue;
        }

        /* Map stage index back to chain-local index. */
        int chain_stage_idx;
        if (strcmp(tap->chain_name, "video") == 0) {
            chain_stage_idx = i;
        } else {
            /* Audio chain index is offset by video stages. */
            chain_stage_idx = i - (impl->video_chain ? impl->video_chain->num_stages : 0);
        }

        if (chain_stage_idx < 0 || chain_stage_idx >= chain->num_stages) {
            tap->is_valid = false;
            tap->error_msg = "out of range stage index";
            continue;
        }

        SDL_GPUBuffer *source_buf = chain_get_stage_capture_buffer(chain, chain_stage_idx);
        if (!source_buf) {
            tap->is_valid = false;
            tap->error_msg = "no captured stage snapshot";
            continue;
        }

        /* Stages whose output is wider than the main signal buffer
         * (e.g. Matrix Decode writes interleaved RGB, 3× size) publish
         * that via stage->snapshot_size. Pull the authoritative byte
         * count from the chain so we don't truncate on readback. */
        uint32_t buffer_bytes = chain_get_stage_snapshot_size(chain, chain_stage_idx);
        int sample_count = (int)(buffer_bytes / sizeof(float));

        /* Allocate CPU buffer lazily. */
        if (!impl->buffers[i]) {
            impl->buffers[i] = (float *)malloc(buffer_bytes);
            if (!impl->buffers[i]) {
                tap->is_valid = false;
                tap->error_msg = "malloc failed for tap buffer";
                continue;
            }
        }

        /* Download GPU buffer to CPU. */
        uint64_t t_read0 = SDL_GetPerformanceCounter();
        bool ok = gpu_buffer_download(gpu, source_buf, impl->buffers[i],
                                      buffer_bytes);
        uint64_t t_read1 = SDL_GetPerformanceCounter();

        if (!ok) {
            tap->is_valid = false;
            tap->error_msg = "gpu_buffer_download failed";
            continue;
        }

        /* Update tap metadata. */
        tap->data = impl->buffers[i];
        tap->sample_count = sample_count;
        tap->frame_number = frame_number;
        tap->capture_timestamp_ns = capture_time_ns;
        /* Refresh the 2D layout each frame. Video chains carry a
         * non-zero samples_per_line, audio chains leave it at 0; the
         * init path only set this once (and to 0), so without this
         * the visualiser could never interpret video buffers as 2D
         * waveforms, especially across NTSC/PAL width changes. */
        if (chain->samples_per_line > 0) {
            tap->samples_per_line = chain->samples_per_line;
            tap->lines = (tap->samples_per_line > 0)
                         ? sample_count / tap->samples_per_line
                         : 0;
        } else {
            tap->samples_per_line = 0;
            tap->lines = 1;
        }

        /* Copy current stage state. */
        const ChainStage *stage = &chain->stages[chain_stage_idx];
        tap->stage_enabled = stage->enabled;
        tap->stage_bypassed = stage->bypass;
        tap->gpu_dispatch_us = stage->timing_us;

        if (stage->params_size > 0 && stage->params_size <= 128) {
            memcpy(tap->stage_params, stage->params, stage->params_size);
            tap->stage_params_size = stage->params_size;
        }

        /* Compute readback time. */
        uint64_t freq = SDL_GetPerformanceFrequency();
        double readback_us = (double)(t_read1 - t_read0) * 1e6 / freq;
        tap->readback_us = readback_us;

        tap->is_valid = true;
        tap->error_msg = NULL;
    }

    uint64_t t1 = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    double total_capture_us = (double)(t1 - t0) * 1e6 / freq;

    impl->total_captures++;
    impl->total_readback_us += total_capture_us;
    if (total_capture_us > impl->peak_readback_us) {
        impl->peak_readback_us = total_capture_us;
    }
    impl->avg_readback_us = impl->total_readback_us / impl->total_captures;
}

const DebugTap *debug_tap_get(const DebugTapManager *mgr, int stage_index) {
    if (!mgr) return NULL;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    if (stage_index < 0 || stage_index >= impl->num_taps) {
        return NULL;
    }

    if (!impl->enabled[stage_index]) {
        return NULL;
    }

    return &impl->taps[stage_index];
}

/* ============================================================================
 * Metadata Queries
 * ============================================================================ */

int debug_tap_total_count(const DebugTapManager *mgr) {
    if (!mgr) return 0;
    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    return impl->num_taps;
}

int debug_tap_video_stage_count(const DebugTapManager *mgr) {
    if (!mgr) return 0;
    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    return impl->video_chain ? impl->video_chain->num_stages : 0;
}

int debug_tap_audio_stage_count(const DebugTapManager *mgr) {
    if (!mgr) return 0;
    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    return impl->audio_chain ? impl->audio_chain->num_stages : 0;
}

const char *debug_tap_get_stage_name(const DebugTapManager *mgr,
                                      int stage_index) {
    if (!mgr) return "";
    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    if (stage_index < 0 || stage_index >= impl->num_taps) {
        return "";
    }
    return impl->taps[stage_index].stage_name ? impl->taps[stage_index].stage_name
                                               : "";
}

const char *debug_tap_get_chain_name(const DebugTapManager *mgr,
                                      int stage_index) {
    if (!mgr) return "unknown";
    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    if (stage_index < 0 || stage_index >= impl->num_taps) {
        return "unknown";
    }
    return impl->taps[stage_index].chain_name ? impl->taps[stage_index].chain_name
                                               : "unknown";
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

DebugTapStats debug_tap_get_stats(const DebugTapManager *mgr) {
    DebugTapStats stats = {0};

    if (!mgr) return stats;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    stats.total_captures = impl->total_captures;
    stats.total_readback_us = impl->total_readback_us;
    stats.peak_readback_us = impl->peak_readback_us;
    stats.avg_readback_us = impl->avg_readback_us;
    stats.current_enabled_count = debug_tap_enabled_count(mgr);

    return stats;
}

void debug_tap_reset_stats(DebugTapManager *mgr) {
    if (!mgr) return;

    DebugTapManagerImpl *impl = (DebugTapManagerImpl *)mgr;
    impl->total_captures = 0;
    impl->total_readback_us = 0.0;
    impl->peak_readback_us = 0.0;
    impl->avg_readback_us = 0.0;
}
