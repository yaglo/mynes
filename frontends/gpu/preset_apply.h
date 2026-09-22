/*
 * preset_apply.h -- Preset application, OSD menu callbacks, overlay compositing
 */
#ifndef PRESET_APPLY_H
#define PRESET_APPLY_H

#include <stdbool.h>
#include <SDL3/SDL.h>
#include "signal_precompute.h"
#include "video_chain.h"
#include "video_gpu.h"
#include "audio_chain.h"
#include "presets.h"
#include "chain_vis.h"
#include "gpu_render.h"
#include "config.h"
#include "nes/nes.h"
#include "nes/osd.h"

typedef struct {
    SignalPrecompute *sig_state;
    VideoChain       *video_chain;
    AudioChain       *audio_chain;
    VideoGPUChain    *video_gpu_chain;
    SDL_GPUDevice    *gpu;
    bool             *gpu_video_enabled;
    bool             *gpu_audio_enabled;
    int              *use_gpu_audio;
    int               display_bypass;
    int              *current_preset;
    MynesConfig      *config;
    int               region;
    PPU              *display_ppu;
    APUAnalog        *analog_controls;
    ChainVis         *chain_vis;
    /* Filled by main after shader resolution so preset_apply can do a
     * full destroy+init of the GPU chain when preset topology changes
     * (connection, comb_type, region). NULL disables the full-rebuild
     * path and the code falls back to video_gpu_reinit_stages. */
    const char       *shader_dir;
    /* Optional render context — when set, preset_apply clears its
     * transient state (HV-sag smoother, etc.) on preset change so
     * time-integrators from the previous preset don't bleed through. */
    GPURenderCtx     *render_ctx;
    /* Set to true inside preset_apply_cpu_state whenever the region
     * flipped this apply. preset_apply_gpu_push reads it to decide
     * whether to do a full GPU rebuild; main.c reads it to rebuild
     * the debug tap manager (which caches per-stage metadata). */
    bool              region_switched;
    /* Proxy int the OSD region widget writes into. The callback
     * compares against ctx->region and, if different, routes through
     * preset_set_region() so the full teardown + signal-table
     * refresh happens atomically. */
    int               osd_region_sel;
    /* Raised by preset_set_region whenever it triggered a full GPU
     * rebuild; main.c consumes it to destroy + re-create the debug
     * tap manager (which caches per-stage metadata tied to the old
     * chain). Cleared by the consumer. */
    bool              chain_rebuilt_flag;
    bool              console_reset_requested; /* consumed by main at a frame boundary */
    Uint64            preset_notice_until;
} PresetCtx;

/* Set the global preset context (must be called before any callbacks fire). */
void preset_ctx_init(PresetCtx *ctx);
struct DebugServer;
void preset_register_debug_controls(PresetCtx *ctx, struct DebugServer *server);

/* Apply a physical preset by struct pointer (CPU state + GPU push in
 * one call). Runtime preset loads preserve the CURRENT live region so
 * cycling looks does not silently kick a PAL/NTSC session back to the
 * preset file's baked-in region. */
void preset_apply(PresetCtx *ctx, const PhysicalPreset *p);

/* Two-phase startup path. Call preset_apply_cpu_state BEFORE
 * video_gpu_init / audio_gpu_init so the GPU inits see the preset's
 * FIR tap counts, audio params, and color matrix. Then call
 * preset_apply_gpu_push AFTER the GPU chain is live to push everything
 * into GPU buffers + uniforms. Together they're equivalent to a single
 * preset_apply() call. */
void preset_apply_cpu_state(PresetCtx *ctx, const PhysicalPreset *p);
void preset_apply_gpu_push(PresetCtx *ctx);

/* Switch the pipeline region (NTSC ↔ PAL) without reloading a preset.
 * Called from the OSD region toggle and from the ROM-load path when the
 * iNES header advertises PAL. Regenerates both signal tables, rebuilds
 * the FIR taps at the new sample rate, updates VideoChain.signal_fmt,
 * and does a full GPU-chain rebuild so per-stage dispatch dimensions
 * match the new samples_per_line. Returns true if a rebuild actually
 * ran; the caller should rebuild any per-chain state it caches
 * (e.g. debug_tap_manager) on a true return. */
bool preset_set_region(PresetCtx *ctx, int new_region);

/* Preset registry (backed by JSON files scanned in preset_ctx_init).
 * preset_load_index() is the runtime path and preserves the live region;
 * preset_load_index_exact() is the startup/import path that honours the
 * region stored in the preset file. */
int         preset_total_count(void);
const char *preset_display_name(int idx);
int         preset_load_index(int idx);          /* returns idx on success, -1 otherwise */
int         preset_load_index_exact(int idx);    /* startup path: honour preset region */
int         preset_register_file(const char *path); /* read-only external preset, no copy/write */
int         preset_find_by_slug(const char *s);  /* substring match on path/name, -1 if none */

/* Editor catalog and commands. IDs are registry slots guarded by revision. */
uint32_t preset_catalog_revision(void);
int preset_active_index(void);
bool preset_is_user(int index);
bool preset_is_modified(void);
const char *preset_cycle_notice(void); /* NULL after the brief selection notice */
bool preset_manage(uint32_t operation, int index, uint32_t revision,
                   const char *name, char *error, size_t error_size);

/* Composite chain visualiser + OSD overlays onto the PPU framebuffer. */
void preset_composite_overlays(PresetCtx *ctx);

/* OSD menu root table and its size (populated by preset_ctx_init). */
extern OSDMenuItem preset_menu_root[];
extern int         preset_menu_root_count;

#endif /* PRESET_APPLY_H */
