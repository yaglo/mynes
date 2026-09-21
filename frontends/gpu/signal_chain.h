/*
 * Signal Chain Runner — Data-Driven GPU Compute Dispatch
 * ========================================================
 *
 * Generic stage dispatch table shared by audio and video chains.
 * Instead of hardcoding dispatch sequences in C, each chain is an
 * array of ChainStage structs. The runner iterates the array,
 * dispatches each enabled stage, and ping-pongs between two buffers.
 *
 * Adding a new stage = appending to the array. No C code changes.
 * Hot-switching connection type = toggling stage enabled flags.
 *
 * Each stage maps to one GPU kernel dispatch:
 *   - kernel_type selects which pipeline to use
 *   - params holds the uniform data for that kernel
 *   - enabled flag allows runtime bypass
 *   - timing_us records wall-clock dispatch time
 *
 * The runner owns a ping-pong buffer pool (2 buffers, alternating)
 * plus optional auxiliary buffers for multi-output stages (e.g.,
 * modulator produces I and Q simultaneously).
 */

#ifndef SIGNAL_CHAIN_H
#define SIGNAL_CHAIN_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "gpu_compute.h"

/* ============================================================================
 * Kernel type enumeration (matches the compute shader library)
 * ============================================================================ */

typedef enum {
    CHAIN_KERNEL_POINTWISE = 0,   /* pointwise.comp — gain, clip, gamma, hum, DAC */
    CHAIN_KERNEL_RC_FILTER,       /* rc_filter.comp — every capacitor */
    CHAIN_KERNEL_FIR,             /* fir.comp — bandwidth limit, decimation */
    CHAIN_KERNEL_DELAY,           /* delay.comp — comb filter, ghosting */
    CHAIN_KERNEL_COMB,            /* comb_filter.comp — Y/C separator */
    CHAIN_KERNEL_MODULATOR,       /* modulator.comp — chroma mod/demod */
    CHAIN_KERNEL_DAC,             /* dac_2c02.comp — palette→waveform */
    CHAIN_KERNEL_MATRIX,          /* matrix_decode.comp — YIQ→RGB */
    CHAIN_KERNEL_PAL_CHROMA,      /* pal_chroma.comp — PAL U-flip + 1H V average */
    CHAIN_KERNEL_DEFLECTION,      /* deflection.comp — beam landing / dwell / focus map */
    CHAIN_KERNEL_BEAM,            /* beam_profile.comp — scanline structure */
    CHAIN_KERNEL_RF,              /* rf_mod_demod.comp — RF channel simulation */
    CHAIN_KERNEL_VIDEO_AMP,       /* video_amp.comp — per-channel RGB bandwidth */
    CHAIN_KERNEL_H_BLUR_RGB,      /* h_blur_rgb.comp — horizontal blur on RGB */
    CHAIN_KERNEL_TEMPORAL_BLIT,   /* temporal_blit.comp — blend + blit to texture */
    CHAIN_KERNEL_AGC,             /* agc.comp — automatic gain control */
    CHAIN_KERNEL_RASTER,          /* active waveform -> complete horizontal raster */
    CHAIN_KERNEL_RECEIVER,
    CHAIN_KERNEL_RECEIVER_DEMOD,        /* burst phase + back-porch clamp */
    CHAIN_KERNEL_YC_ROUTE,        /* separated source Y/C receiver routing */
    CHAIN_KERNEL_RECEIVER_PLL,   /* line oscillator, holdover and clamp */
    CHAIN_KERNEL_CRT_LOAD,       /* video rail streaking and shared supply */
    CHAIN_KERNEL_GUN_CURRENT,    /* gun voltage -> linear emitted current */
    CHAIN_KERNEL_RF_IF,          /* complex IF filter and envelope detector */
    CHAIN_KERNEL_OSD,            /* TV RGB menu, after receiver */
    CHAIN_KERNEL_VHS,            /* recovered tape Y/C and timebase */
    CHAIN_KERNEL_COUNT
} ChainKernelType;

/* ============================================================================
 * Stage definition
 * ============================================================================ */

/* Maximum uniform data size per stage (bytes). */
#define CHAIN_MAX_UNIFORM_SIZE 256

/* Bindings and limits for explicit per-stage I/O. */
#define CHAIN_STAGE_MAX_RO   4     /* max readonly buffer inputs per stage */
#define CHAIN_STAGE_MAX_RW   3     /* max readwrite buffer outputs per stage */
#define CHAIN_STAGE_MAX_EXT  4     /* max external buffer pointers per stage */

/* Symbolic references to the buffers a stage may read or write.
 * chain_run_cmd resolves these to concrete SDL_GPUBuffer* at dispatch
 * time from the chain state. Replaces the hardcoded switch-on-kernel
 * binding logic with declarative, inspectable, testable I/O.
 *
 * Pass CBR_BUF_SRC as a readwrite binding for an in-place stage; it
 * resolves to the same buffer as a CBR_BUF_SRC readonly binding, and
 * the runner suppresses the usual ping-pong flip. CBR_BUF_DST signals
 * "write new output; consume buf[src]; flip current_buf". */
typedef enum {
    CBR_NONE = 0,
    CBR_BUF_SRC,         /* chain->buf[chain->current_buf]       */
    CBR_BUF_DST,         /* chain->buf[1 - chain->current_buf]   */
    CBR_AUX0,            /* chain->aux[0]                        */
    CBR_AUX1,            /* chain->aux[1]                        */
    CBR_AUX2,            /* chain->aux[2]                        */
    CBR_AUX3,            /* chain->aux[3]                        */
    CBR_TAPS,            /* chain->tap_bufs[stage->taps_index]   */
    CBR_CARRY,           /* chain->carry_buf                     */
    CBR_EXT0,            /* stage->external[0]  (RGB / user-owned)  */
    CBR_EXT1,            /* stage->external[1]                      */
    CBR_EXT2,            /* stage->external[2]                      */
    CBR_EXT3,            /* stage->external[3]                      */
} ChainBufRef;

/* Forward decls for the optional rebind hook. Used when a stage's
 * bindings vary at runtime — the chroma path swaps aux slots based
 * on whether the comb filter is producing separated chroma. */
struct SignalChainFwd;
struct ChainStageFwd;
typedef bool (*ChainStageRebind)(struct SignalChainFwd *chain,
                                 struct ChainStageFwd *stage,
                                 void *user);

/* LEGACY custom-dispatch hook (still used by chroma + post meta stages
 * during the migration). Declared before ChainStage so the field type
 * resolves; removed once every stage is declarative. */
typedef bool (*ChainStageCustomDispatch)(void *chain,
                                         void *stage,
                                         SDL_GPUCommandBuffer *cmd,
                                         void *user);

typedef struct {
    /* Identity. */
    const char     *name;            /* human-readable name (for visualiser) */
    ChainKernelType kernel_type;     /* which compute shader to dispatch */

    /* Runtime state. */
    bool            enabled;         /* false = skip this stage */
    bool            bypass;          /* user-toggled bypass (visualiser B key) */

    /* Uniform parameters (copied to GPU each dispatch). */
    uint8_t         params[CHAIN_MAX_UNIFORM_SIZE];
    uint32_t        params_size;     /* actual size in bytes */

    /* Dispatch dimensions. */
    uint32_t        dispatch_x;      /* workgroups in X */
    uint32_t        dispatch_y;      /* workgroups in Y (1 for 1D audio) */
    uint32_t        dispatch_z;      /* workgroups in Z (always 1) */

    /* Timing (filled by the runner after dispatch). */
    double          timing_us;       /* wall-clock microseconds */
    double          timing_avg_us;   /* exponential moving average */

    /* --- Typed I/O declaration --- */
    ChainBufRef    ro[CHAIN_STAGE_MAX_RO];
    int            ro_count;
    ChainBufRef    rw[CHAIN_STAGE_MAX_RW];
    int            rw_count;
    int            taps_index;      /* tap buffer index for CBR_TAPS */
    SDL_GPUBuffer *external[CHAIN_STAGE_MAX_EXT];
    /* Optional pre-dispatch rebind hook — fills in ro[] / rw[] /
     * external[] from runtime state (e.g. chroma aux layout flips
     * when the comb toggles). NULL if the static declaration is
     * enough. Returning false aborts the stage. */
    ChainStageRebind rebind;
    void            *rebind_user;

    /* When true, chain_run_cmd uses the ro/rw declaration above
     * (plus optional rebind) to bind the pass. Set by per-kernel
     * registration helpers as each kernel migrates. Stages with
     * io_typed=false still flow through the legacy switch dispatcher. */
    bool            io_typed;
    bool            reuse_output; /* rebind may reuse a valid immutable result */

    /* LEGACY custom-dispatch escape hatch. Remains in place while the
     * chroma + post pipelines are migrated to the typed bindings. */
    ChainStageCustomDispatch custom;
    void                    *custom_user;

    /* Debug snapshot source/size. Governs what `chain_set_stage_capture`
     * allocates and what chain_run_cmd copies when capture is on.
     *
     * snapshot_src:
     *   CBR_NONE  → default: snapshot rw[0] for typed stages.
     *               Custom stages with no override cannot be captured
     *               (chain_set_stage_capture returns false).
     *   anything else → resolved through resolve_buf_ref at copy time.
     *
     * snapshot_size:
     *   0  → default: chain->buf_size.
     *   >0 → exact byte count to allocate + copy (used by stages whose
     *        output is not the signal-sized float buffer — e.g. Matrix
     *        Decode writes an interleaved RGB buffer 3× the signal
     *        buffer, so snapshot_size = chain->buf_size * 3). */
    ChainBufRef     snapshot_src;
    uint32_t        snapshot_size;
} ChainStage;

/* ============================================================================
 * Chain runner state
 * ============================================================================ */

#define CHAIN_MAX_STAGES     32
#define CHAIN_MAX_TAP_BUFS    8
#define CHAIN_MAX_AUX_BUFS    4

typedef struct {
    /* Stage array (the chain definition). */
    ChainStage stages[CHAIN_MAX_STAGES];
    int        num_stages;
    int        first_stage;       /* component/RGB input enters after the receiver */

    /* GPU pipelines (one per kernel type, shared across stages). */
    GpuPipeline pipelines[CHAIN_KERNEL_COUNT];
    bool        pipeline_loaded[CHAIN_KERNEL_COUNT];

    /* Ping-pong signal buffers (2 buffers, alternating). */
    SDL_GPUBuffer *buf[2];
    int            current_buf;       /* 0 or 1: which buffer has current data */
    uint32_t       buf_size;          /* size in bytes (same for both) */

    /* Optional per-stage debug snapshots for the visualiser. When enabled
     * for a stage, chain_run_cmd copies that stage's output into the
     * corresponding buffer immediately after dispatch. */
    SDL_GPUBuffer *debug_stage_bufs[CHAIN_MAX_STAGES];
    bool           debug_stage_capture_enabled[CHAIN_MAX_STAGES];

    /* Auxiliary buffers for multi-output stages. */
    SDL_GPUBuffer *aux[CHAIN_MAX_AUX_BUFS];
    uint32_t       aux_size;

    /* Carry buffer for RC prefix scan inter-block state. */
    SDL_GPUBuffer *carry_buf;
    uint32_t       carry_size;

    /* FIR tap coefficient buffers (uploaded once, shared). */
    SDL_GPUBuffer *tap_bufs[CHAIN_MAX_TAP_BUFS];
    uint32_t       tap_sizes[CHAIN_MAX_TAP_BUFS];
    int            num_tap_bufs;

    /* Signal format info. */
    int            sample_count;     /* total samples in the chain buffer */
    int            sample_rate;      /* processing sample rate */
    int            samples_per_line; /* samples per scanline (0=no per-line dispatch) */

    /* Per-chain timing. */
    double         total_timing_us;
    bool           timing_enabled;
} SignalChain;

/* ============================================================================
 * API
 * ============================================================================ */

/* Initialize a chain: allocate ping-pong buffers + carry buffer.
 * sample_count: number of float samples per buffer.
 * shader_dir: path to SPIR-V/MSL shader files. */
bool chain_init(SignalChain *chain, SDL_GPUDevice *gpu,
                int sample_count, const char *shader_dir);

/* Add a stage to the chain. Returns the stage index, or -1 on failure. */
int chain_add_stage(SignalChain *chain, const char *name,
                    ChainKernelType kernel_type,
                    const void *params, uint32_t params_size,
                    uint32_t dispatch_x, uint32_t dispatch_y);

/* Populate a stage's ro[] / rw[] bindings with the canonical pattern
 * for its kernel_type — mirrors what the legacy chain_run_cmd switch
 * did per kernel, but as declarative bindings. Sets io_typed=true so
 * chain_run_cmd uses the typed dispatcher for this stage. Call after
 * chain_add_stage (and after setting taps_index / external[] if the
 * stage uses them). */
void chain_stage_set_default_io(ChainStage *s);

/* Upload tap coefficients for a FIR stage. Returns tap buffer index. */
int chain_upload_taps(SignalChain *chain, SDL_GPUDevice *gpu,
                      const float *taps, int num_taps);

/* Upload input data to the chain's current buffer. */
bool chain_upload_input(SignalChain *chain, SDL_GPUDevice *gpu,
                        const void *data, uint32_t size);

/* Run the entire chain: dispatches all enabled stages in sequence.
 * Ping-pongs between the two buffers. Records per-stage timing
 * when chain->timing_enabled is true.
 *
 * After completion, the result is in buf[chain->current_buf]. */
bool chain_run(SignalChain *chain, SDL_GPUDevice *gpu);

/* Record chain stages into an external command buffer (caller manages fence). */
bool chain_run_cmd(SignalChain *chain, SDL_GPUCommandBuffer *cmd);

/* Upload input via copy pass in an external command buffer. */
bool chain_upload_input_cmd(SignalChain *chain, SDL_GPUDevice *gpu,
                             SDL_GPUCommandBuffer *cmd,
                             const void *data, uint32_t size);

/* Download the chain output to CPU memory.
 * Blocks until the GPU finishes. */
bool chain_download_output(SignalChain *chain, SDL_GPUDevice *gpu,
                            void *out, uint32_t size);

/* Enable/disable a stage by index. */
void chain_set_stage_enabled(SignalChain *chain, int index, bool enabled);

/* Toggle bypass on a stage (visualiser). */
void chain_set_stage_bypass(SignalChain *chain, int index, bool bypass);

/* Update a stage's uniform parameters. */
void chain_update_params(SignalChain *chain, int index,
                          const void *params, uint32_t size);

/* Get per-stage timing (filled after chain_run). */
double chain_get_stage_timing(const SignalChain *chain, int index);
double chain_get_total_timing(const SignalChain *chain);

/* Get stage info for the visualiser. */
const char *chain_get_stage_name(const SignalChain *chain, int index);
bool chain_get_stage_enabled(const SignalChain *chain, int index);
bool chain_get_stage_bypassed(const SignalChain *chain, int index);
int chain_get_num_stages(const SignalChain *chain);

/* Enable/disable per-stage GPU snapshots for the debug visualiser.
 * Returns false (and leaves capture disabled) if the stage has no
 * meaningful snapshot source — currently: custom-dispatch stages that
 * have not declared stage->snapshot_src. */
bool chain_set_stage_capture(SignalChain *chain, SDL_GPUDevice *gpu,
                             int index, bool enabled);
SDL_GPUBuffer *chain_get_stage_capture_buffer(const SignalChain *chain, int index);

/* Byte size of the snapshot buffer for a stage. Returns
 * stage->snapshot_size if non-zero, else chain->buf_size. Visualiser
 * clients need this to download the right number of bytes for stages
 * whose output is wider than the main signal buffer (e.g. Matrix
 * Decode writes RGB, 3× signal size). */
uint32_t chain_get_stage_snapshot_size(const SignalChain *chain, int index);

/* Destroy the chain: free all GPU resources. */
void chain_destroy(SignalChain *chain, SDL_GPUDevice *gpu);

#endif /* SIGNAL_CHAIN_H */
