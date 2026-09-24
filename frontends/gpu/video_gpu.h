/* GPU waveform generation, full-raster receiver and linear-light CRT pipeline.
 * VideoGPUChain owns the signal runner plus DAC, beam and phosphor resources.
 * Live input is PPU codes; CPU waveform upload remains a diagnostic path.
 * See docs/gpu-pipeline-reference.md for stages, units and model limitations. */

#ifndef VIDEO_GPU_H
#define VIDEO_GPU_H

#include "video_chain.h"
#include "decode_window.h"
#include "gpu_compute.h"
#include "signal_chain.h"
#include "vhs_gpu.h"
#include <stdbool.h>

/* Maximum FIR tap count for luma and chroma filters. */
#define VIDEO_GPU_MAX_FIR_TAPS 64

/* ============================================================================
 * GPU video chain state
 * ============================================================================
 *
 * Internally uses a SignalChain for the core signal processing stages
 * (FIR, demod). The chain runner handles ping-pong buffers, dispatch
 * boilerplate, and per-stage timing. This struct holds the chain plus
 * resources the chain runner doesn't own (DAC pipeline, index buffers,
 * signal table, and the CPU-side matrix decode state).
 *
 */

typedef struct {
    uint32_t count, full_width, active_width, samples_per_dot;
    float phase_base, line_phase;
    uint32_t region, lines, separate_yc;
    /* Sync tip and burst relative to blanking, in units of white. The 2C02
     * draws a -264/788 sync and a square burst; an encoder IC puts sync at
     * -40 IRE and a sine burst of 40 IRE peak to peak. */
    float sync_level, burst_amp;
    uint32_t burst_sine;
    float backdrop[12], gray_backdrop[12];
} GpuRasterParams;

/* One picture from a console whose video chip outputs RGB codes through a
 * DAC into an encoder IC (Mega Drive, Super Famicom). See
 * video_gpu_process_rgb. */
typedef struct {
    const uint32_t *pixels;   /* width × lines codes: bits 0-5 R, 6-11 G, 12-17 B,
                               * or with code_bits 10, bits 0-9 R, 10-19 G, 20-29 B */
    int width, lines;         /* console pixels per line and picture lines */
    int top_line;             /* raster picture line (0..239) of the first console line */
    int spp_num, spp_den;     /* samples per pixel = spp_num / spp_den */
    const float *ramp;        /* gun voltage per code, 0 black to 1 white */
    int ramp_n;               /* entries in ramp, at most 64 */
    int phase_base;           /* carrier phase at the first active sample, slots of 12 */
    int phase_line_adv;       /* carrier phase advance per raster line, slots of 12 */
    float chroma_bw_hz;       /* encoder chroma band (-3 dB, baseband); 0 = unfiltered */
    float luma_bw_hz;         /* encoder luma band (-3 dB); 0 = unfiltered */
    float luma_trap;          /* luma trap depth at the subcarrier, 0 to 1; 0 = none */
    float setup;              /* black pedestal as a fraction of white */
    int code_bits;            /* 0: codes index ramp; 10: linear 10-bit gun voltages
                               * (code / 1023), ramp unused, for a video or picture */
} VideoRGBSource;

typedef struct {
    /* --- Generic signal chain runner (owns ping-pong + aux buffers) --- */
    SignalChain sig_chain;
    SignalFormat raster_fmt; /* full lines upstream; signal_fmt remains active-picture format */
    /* The rectangle of the raster decoded into RGB: buf_rgb, buf_rgb2 and
     * buf_gun_current are window.width x window.lines interleaved RGB. */
    DecodeWindow window;
    int stage_raster, stage_receiver, stage_receiver_pll;
    int stage_y_console, stage_y_cable, stage_y_ghost, stage_yc_route;
    bool source_separated;
    int signal_phase_base, signal_line_phase;
    SDL_GPUBuffer *buf_receiver, *buf_receiver_measurements;
    /* The border the PPU draws around the picture: the backdrop's waveform
     * and its hue-0 grey for the raster, and its 9-bit palette entry for an
     * RGB PPU. One snapshot per frame; $0F until the frontend sets one. */
    float backdrop[12], gray_backdrop[12];
    unsigned backdrop_entry;
    unsigned elapsed_frames;


    /* --- Stage indices into sig_chain (for runtime parameter updates) --- */
    int stage_console_hp;       /* Console Output HP (RC, disabled) */
    int stage_console_lp;       /* Console output impedance and bandwidth */
    int stage_cable_rc;         /* Cable equivalent shunt-capacitance pole */
    int stage_tv_input_hp;      /* TV Input HP (RC, disabled) */
    int stage_rf;               /* RF Modulator/Demodulator (noise + hum) */
    VHSGpu vhs;                 /* VHS deck: tape and playback stages */
    int stage_rf_if;            /* complex receiver IF and envelope detector */
    int stage_agc;              /* Automatic Gain Control: apply the per-line gains */
    int stage_agc_loop;         /* the keyed top-sync loop that writes them */
    SDL_GPUBuffer *buf_agc_gains; /* vec4 per line plus the loop state */
    int stage_ghosting;         /* Ghosting (cable impedance reflection) */
    int stage_comb_bandpass, stage_comb; /* Chroma band and line Y/C separator */
    int stage_luma_fir;         /* Luma FIR */
    int stage_luma_peaking;     /* Sharpness after Y separation */
    int stage_chroma_demod;     /* Chroma Demod (modulator, dual-output) */
    int stage_chroma_i_fir;     /* Chroma I FIR */
    int stage_chroma_q_fir;     /* Chroma Q FIR */
    int stage_pal_chroma;       /* PAL chroma correction (PAL only) */
    int stage_matrix;           /* Matrix decode (Y + region-specific chroma -> RGB). */
    int stage_osd;
    SDL_GPUBuffer *buf_osd;
    uint32_t *osd_cache;
    int stage_post_pipeline;    /* RGB Post (video amp + h_blur custom stage) */
    int stage_crt_load, stage_crt_supply;
    SDL_GPUBuffer *buf_crt_load;
    int stage_deflection;       /* Beam landing / dwell / focus map */
    int stage_beam_output;      /* Beam deposition + temporal blend custom stage */

    /* --- DAC pipeline (not part of signal chain -- different dispatch) --- */
    GpuPipeline pipe_dac;

    /* --- DAC-specific GPU buffers (not owned by signal chain) --- */
    SDL_GPUBuffer *buf_indices;
    Uint32 indices_size;

    /* --- RGB console encoder source (video_gpu_process_rgb) --- */
    GpuPipeline pipe_encoder;
    SDL_GPUBuffer *buf_pixels, *buf_ramp;
    SDL_GPUTransferBuffer *pixels_transfer;
    Uint32 pixels_size;
    float ramp_cache[64];
    bool ramp_uploaded;
    /* Raster sync and burst shape; the 2C02 values unless a source sets them. */
    float raster_sync_level, raster_burst_amp;
    bool raster_burst_sine;
    SDL_GPUBuffer *buf_signal_table;
    /* Alternate (odd-scanline) signal table for PAL. On PAL the
     * subcarrier V-component flips sign every line — each scanline reads
     * from the opposite table, picked by parity in the dispatch path.
     * NULL in NTSC mode (main table used for every line). */
    SDL_GPUBuffer *buf_signal_table_alt;

    /* --- RGB output buffer (Phase 2: GPU matrix shader writes here) --- */
    SDL_GPUBuffer *buf_rf_carrier; /* interleaved complex AM carrier */
    SDL_GPUBuffer *buf_rgb;
    SDL_GPUBuffer *buf_rgb2;            /* second RGB buffer for video amp ping-pong */
    SDL_GPUBuffer *buf_gun_current;     /* linear light before horizontal spot spread */
    SDL_GPUBuffer *buf_pal_v;           /* corrected PAL V after 1H averaging */
    SDL_GPUBuffer *buf_pal_u;           /* corrected PAL U after odd-line sign fix */
    bool deflection_cache_valid;
    SDL_GPUBuffer *buf_deflection_x;    /* r/g/b landed X + dwell (float4/pixel) */
    SDL_GPUBuffer *buf_deflection_y;    /* r/g/b landed Y + sigma scale (float4/pixel) */

    /* --- Beam profile output (float16x4, display resolution) --- */
    SDL_GPUBuffer *buf_beam_rgba;
    SDL_GPUBuffer *buf_beam_prev;   /* previous frame for temporal blend */
    SDL_GPUTexture *tex_beam;       /* storage texture for zero-copy display */
    Uint32 beam_rgba_size;
    Uint32 deflection_size;         /* one float4 landing buffer */
    int beam_out_w;                 /* display width */
    int beam_out_h;                 /* display height */
    int beam_rows_per_scanline;     /* output rows per NES scanline */
    float beam_sigma_narrow;        /* narrow beam sigma (dark pixels) */
    float beam_sigma_wide;          /* wide beam sigma (bright pixels) */
    float beam_h_blur_sigma;        /* horizontal blur sigma in signal samples */
    uint32_t beam_frame_counter;    /* incremented each beam dispatch (for noise) */
    SDL_GPUBuffer *buf_phosphor_history; /* accumulated linear-light decay */
    Uint32 phosphor_history_size;
    bool temporal_history_valid;
    bool smoothing_history_valid;
    uint32_t signal_frame_counter;
    float temporal_blend;           /* optional display smoothing, independent of decay */
    float tail_r, tail_g, tail_b, tail_weight;
    float blend_r, blend_g, blend_b; /* per-channel phosphor persistence weights */
    float motion_threshold;         /* 3D comb motion detector threshold */
    float edge_focus;               /* beam focus degradation at edges */
    float velocity_dim;             /* beam velocity dimming at edges */
    float frame_brightness;         /* smoothed scene brightness for HV breathing */
    float apl_smoothed;             /* scene estimate for generic DC-restoration drift */
    float audio_bass_rms;           /* per-frame audio RMS for microphonics */

    /* --- Buffer sizes (bytes) --- */
    Uint32 signal_size;             /* samples_per_line * 240 * sizeof(float) */
    Uint32 rgb_size;                /* window.width * window.lines * 3 * sizeof(float) */

    /* --- Signal format (copied from chain) --- */
    SignalFormat signal_fmt;

    /* --- Video chain configuration (pointer, not owned) --- */
    const VideoChain *chain;

    /* --- FIR filter parameters (mirrored from CPU Composite) --- */
    float fir_y_taps[VIDEO_GPU_MAX_FIR_TAPS];
    float fir_c_taps[VIDEO_GPU_MAX_FIR_TAPS];
    float fir_q_taps[VIDEO_GPU_MAX_FIR_TAPS];
    int   fir_y_n;                  /* number of Y taps (odd) */
    int   fir_c_n;                  /* number of I (wider chroma) taps (odd) */
    int   fir_q_n;                  /* number of Q (narrower chroma) taps (odd) */

    /* --- Video amplifier FIR (per-channel RGB bandwidth limit) --- */
    float vamp_taps[32][4];             /* independent R/G/B FIR coefficients, std140 vec4 */
    int   vamp_tap_count;               /* number of taps (odd) */
    bool  vamp_enabled;                 /* whether video amp stage is active */

    /* --- Chroma demod parameters --- */
    SDL_GPUTransferBuffer *indices_transfer;
    float demod_line_phase;
    float demod_phase;              /* starting phase per scanline (radians) */
    float demod_dp;                 /* phase increment per sample (radians) */

    /* --- Color matrix (3x3 + bias, from VideoChain / Composite) --- */
    float color_matrix[3][3];       /* m[row=R,G,B][col=Y,C1,C2] */
    float color_bias[3];            /* pre-multiplied [R, G, B] bias */

    /* --- Performance --- */
    bool timing_enabled;
} VideoGPUChain;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/* Initialize the GPU video chain: load compute pipelines from SPIR-V,
 * allocate GPU buffers, set up FIR taps and demod parameters.
 *
 * gpu:        SDL3 GPU device (must already be created)
 * chain:      video chain config (video_chain_init_preset must have been
 *             called). The pointer is stored, not copied.
 * shader_dir: directory containing .spv files (e.g., "shaders/compute")
 * fir_y_taps: luma FIR filter coefficients (from CPU CompositeSignal)
 * fir_y_n:    number of luma FIR taps
 * fir_c_taps: I-channel (wider chroma) FIR filter coefficients
 * fir_c_n:    number of I FIR taps
 * fir_q_taps: Q-channel (narrower chroma) FIR filter coefficients
 * fir_q_n:    number of Q FIR taps
 *
 * Returns true on success. */
bool video_gpu_init(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                    const VideoChain *chain, const char *shader_dir,
                    const float *fir_y_taps, int fir_y_n,
                    const float *fir_c_taps, int fir_c_n,
                    const float *fir_q_taps, int fir_q_n);

/* Upload the precomputed signal table to GPU. Call once at init, and
 * again if the signal table changes (e.g., PAL ↔ NTSC switch).
 * table: COMP_SIGNAL_ENTRIES × COMP_TABLE_STRIDE floats.
 * table_alt: alternate table for PAL (NULL for NTSC). */
bool video_gpu_upload_signal_table(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                                    const float *table, const float *table_alt,
                                    int entries, int stride);

/* Full GPU path: upload palette index buffer and run the ENTIRE chain
 * including Stage 1 (DAC) on GPU. The CPU does ZERO signal work.
 *
 * idx_fb: 256×240 uint16 palette+emphasis indices (from ppu.index_framebuffer)
 * phase_base, phase_line_adv, frame_field: dot crawl phase params
 * rgb_out: if non-NULL, downloads the decode window's RGB (rgb_size bytes,
 *          laid out as vgc->window says) to the CPU
 *
 * Returns true if the GPU chain produced output. */
bool video_gpu_process_full(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                             const uint16_t *idx_fb,
                             int phase_base, int phase_line_adv, int frame_field,
                             float *rgb_out);

/* RGB console path: upload one picture of gun codes and run the ENTIRE
 * chain with the encoder stage (encoder_rgb.comp) in place of the 2C02
 * DAC. The raster then carries a standard -40 IRE sync and a 40 IRE sine
 * burst, as an encoder IC produces from the console's CSYNC. The rest of
 * the chain (console output pole, cable, receiver, CRT) is unchanged.
 * rgb_out, if non-NULL, receives rgb_size bytes as video_gpu_process_full.
 *
 * Returns true if the GPU chain produced output. */
bool video_gpu_process_rgb(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                           const VideoRGBSource *src, float *rgb_out);

/* Update the color decode matrix. Call when connection type, hue, saturation,
 * or color temperature changes. The matrix maps Y,I,Q -> R,G,B as floats
 * in [0,1] (not 0-255 -- the GPU works in float throughout).
 *
 * matrix: 3x3 row-major [R,G,B][Y,I,Q], values for float->float decode
 * bias:   3-element [R,G,B] additive bias */
void video_gpu_set_color_matrix(VideoGPUChain *vgc,
                                const float matrix[3][3],
                                const float bias[3]);

/* Reinitialize stage enable/disable and parameters from the current
 * VideoChain preset. Call when switching presets at runtime. Updates RF,
 * comb filter, and video amplifier stages. */
void video_gpu_reinit_stages(VideoGPUChain *vgc, const VideoChain *chain);

/* Rebuild the entire GPU video chain with a new region/format. Destroys
 * and re-initializes every GPU resource held by vgc (buffers, pipelines,
 * stages). The VideoGPUChain struct itself retains its address — callers
 * holding &vgc->sig_chain remain valid — but any *cached metadata*
 * (stage count, stage name pointers) becomes stale and should be
 * re-sampled. FIR taps are re-uploaded from the arrays supplied by the
 * caller; a fresh signal table upload still needs to be driven by the
 * caller after this returns.
 *
 * Used by the OSD region toggle + ROM-load auto-detect paths, which
 * both ultimately need NTSC ↔ PAL switching without tearing down the
 * whole frontend.
 *
 * Returns true on success. On failure the vgc is left in a destroyed
 * (but zeroed) state — caller should treat gpu_video as unavailable. */
bool video_gpu_rebuild_for_region(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                                   const VideoChain *chain,
                                   const char *shader_dir,
                                   const float *fir_y_taps, int fir_y_n,
                                   const float *fir_c_taps, int fir_c_n,
                                   const float *fir_q_taps, int fir_q_n);

/* Recalculate RC filter stage params from the current VideoChain values
 * (console_amp_bw, cable params). Call when those OSD parameters change.
 * The RC stages may be disabled (prefix-scan bug) but params are kept
 * up-to-date for when they're enabled. */
void video_gpu_update_rc_params(VideoGPUChain *vgc);

/* Update FIR filter taps at runtime (e.g., when bandwidth controls change).
 * Re-uploads the tap buffers to GPU. */
bool video_gpu_update_fir_taps(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                               const float *fir_y_taps, int fir_y_n,
                               const float *fir_c_taps, int fir_c_n,
                               const float *fir_q_taps, int fir_q_n);

/* Update chroma demodulator parameters.
 * phase: starting phase for this frame (radians)
 * dp:    phase increment per sample = 2*pi*Fsc/Fsample */
void video_gpu_set_demod(VideoGPUChain *vgc, float phase, float dp);

/* Update per-frame dynamic state that belongs to the beam/raster field
 * rather than the display optics. Call before video_gpu_process so the
 * deflection and gun stages see the current scene, DC-restoration and
 * microphonic inputs. */
void video_gpu_set_dynamic_state(VideoGPUChain *vgc,
                                 float frame_brightness,
                                 float apl_smoothed,
                                 float audio_bass_rms);

/* Process one frame of video through the GPU signal chain.
 *
 * waveform: CPU-generated composite waveform (float32, spl * 240)
 *           where spl = signal_fmt.samples_per_line
 * rgb_out:  if non-NULL, the RGB result is downloaded here (blocking).
 *           Must hold rgb_size bytes, laid out as vgc->window says.
 *           If NULL, the result stays on GPU (for display pipeline use).
 *
 * Returns true on success. */
bool video_gpu_process(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                       const float *waveform, float *rgb_out);

/* Get the GPU buffer containing the RGB output (for direct texture upload
 * without CPU readback). Returns NULL if not initialized. */
SDL_GPUBuffer *video_gpu_get_rgb_buffer(const VideoGPUChain *vgc);

/* Configure beam profile (display resolution + scanline structure).
 * Must be called before video_gpu_process to enable the beam shader.
 * out_w, out_h: display resolution (e.g., 1170 x 960)
 * rows_per_scanline: output rows per NES scanline (e.g., 4)
 * sigma_narrow: narrow beam sigma for dark pixels (e.g., 0.22)
 * sigma_wide: wide beam sigma for bright pixels (e.g., 0.55) */
/* Get the beam storage texture (zero-copy, written by temporal_blit shader). */
SDL_GPUTexture *video_gpu_get_beam_texture(const VideoGPUChain *vgc);

bool video_gpu_set_beam_params(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                               int out_w, int out_h,
                               int rows_per_scanline,
                               float sigma_narrow, float sigma_wide);

/* Get the beam profile output dimensions. Returns false if beam not configured. */
bool video_gpu_get_beam_size(const VideoGPUChain *vgc, int *out_w, int *out_h);

/* Download beam profile RGBA8 output to CPU.
 * rgba_out must hold out_w * out_h * 4 bytes.
 * Returns true on success. */
bool video_gpu_download_beam(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                             uint8_t *rgba_out);

/* Reset per-preset temporal state:
 *   - beam_frame_counter (used for noise phasing + dot-crawl phase index)
 *   - previous-frame beam buffer (temporal-blit history)
 * Call from preset_apply so the next frame after a preset switch starts
 * from a clean history instead of blending the old preset's output
 * through the temporal blit + persistence weights. Safe no-op if the
 * GPU chain isn't fully initialised. */
void video_gpu_reset_temporal_state(VideoGPUChain *vgc, SDL_GPUDevice *gpu);

/* Release all GPU resources. Safe to call on a zeroed struct. */
void video_gpu_destroy(VideoGPUChain *vgc, SDL_GPUDevice *gpu);

/* NULL disables composition; repeated identical overlays do not upload again. */
bool video_gpu_set_osd(VideoGPUChain *vgc,SDL_GPUDevice *gpu,const uint32_t *rgba);

#endif /* VIDEO_GPU_H */
