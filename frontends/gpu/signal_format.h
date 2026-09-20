/*
 * Signal Format — CPU→GPU Interface Contract for the Video Signal Path
 * ====================================================================
 *
 * This header defines the buffer format for the composite waveform that
 * the CPU produces and the GPU consumes. Every compute shader's workgroup
 * size, dispatch dimensions, and memory access pattern derives from this
 * format. Change it and everything downstream changes.
 *
 * Physical model:
 *   The NES 2C02 PPU generates a composite video signal — NOT RGB. Each
 *   "pixel" on screen is actually a voltage waveform that encodes both
 *   luminance and chrominance into a single analog signal. The waveform
 *   shape depends on the palette index (6 bits), emphasis bits (3 bits),
 *   and the phase of the 3.579545 MHz NTSC colorburst carrier at that
 *   dot position.
 *
 *   We sample this waveform at 8 samples per NES pixel (PPU dot), giving
 *   2048 samples per visible scanline. At 8 samples per pixel and 12
 *   samples per colorburst cycle, the ratio 8/12 = 2/3 cycles per pixel
 *   matches the real NTSC relationship. PAL uses 10 samples per pixel
 *   (10/12 = 5/6 cycles per pixel).
 *
 * Upload granularity:
 *   The CPU uploads the ENTIRE frame's waveform buffer at once (not
 *   per-scanline). The GPU processes all 240 scanlines as a single
 *   dispatch with workgroup-per-scanline parallelism.
 *
 * Buffer layout:
 *   1D array of float32, row-major. Each scanline is `samples_per_line`
 *   consecutive floats. Scanline N starts at offset N * samples_per_line.
 *
 *   Value range: [0.0, 1.0] normalized. 0.0 = NTSC black level (IRE 0),
 *   1.0 = NTSC white level (IRE 100). Sub-black (sync, color $0D) may
 *   produce small negative values which downstream stages handle via
 *   clamping.
 *
 * Alternative upload: palette index buffer
 *   For GPU-side waveform generation (optional, future), the CPU can
 *   upload a compact buffer of 9-bit palette+emphasis indices (uint16)
 *   and the GPU generates the waveform from the precomputed signal table.
 *   This reduces upload bandwidth from ~1.9 MB to ~120 KB per frame.
 */

#ifndef SIGNAL_FORMAT_H
#define SIGNAL_FORMAT_H

#include <stdint.h>
#include <math.h>

/* ============================================================================
 * Video waveform buffer format
 * ============================================================================ */

/* NES visible screen dimensions (constant, hardware-defined). */
#define SIGNAL_NES_WIDTH       256   /* visible PPU dots per scanline */
#define SIGNAL_NES_HEIGHT      240   /* visible scanlines per frame */

/* Samples per NES pixel (PPU dot). 8 for NTSC, 10 for PAL.
 * The colorburst subcarrier has 12 samples per cycle in both regions:
 *   NTSC: 8/12 = 2/3 subcarrier cycles per pixel
 *   PAL:  10/12 = 5/6 subcarrier cycles per pixel */
#define SIGNAL_NTSC_SAMPLES_PER_PIXEL  8
#define SIGNAL_PAL_SAMPLES_PER_PIXEL   10
#define SIGNAL_SAMPLES_PER_SUBCARRIER_CYCLE 12.0f

/* Absolute subcarrier frequencies. The signal chain keeps 12 samples per
 * chroma cycle in both regions, but the RC/FIR math still depends on the
 * real carrier frequency when converting Hz cutoffs to normalised units. */
#define SIGNAL_NTSC_SUBCARRIER_HZ      3.579545e6f
#define SIGNAL_PAL_SUBCARRIER_HZ       4.43361875e6f

/* Samples per visible scanline (256 pixels × samples_per_pixel). */
#define SIGNAL_NTSC_SAMPLES_PER_LINE   (SIGNAL_NES_WIDTH * SIGNAL_NTSC_SAMPLES_PER_PIXEL)  /* 2048 */
#define SIGNAL_PAL_SAMPLES_PER_LINE    (SIGNAL_NES_WIDTH * SIGNAL_PAL_SAMPLES_PER_PIXEL)   /* 2560 */

/* Maximum samples per line (PAL is wider). Use for buffer allocation. */
#define SIGNAL_MAX_SAMPLES_PER_LINE    SIGNAL_PAL_SAMPLES_PER_LINE  /* 2560 */

/* Total buffer size for one frame (float32). */
#define SIGNAL_FRAME_FLOATS(spl)       ((spl) * SIGNAL_NES_HEIGHT)
#define SIGNAL_NTSC_FRAME_FLOATS       SIGNAL_FRAME_FLOATS(SIGNAL_NTSC_SAMPLES_PER_LINE)  /* 491,520 */
#define SIGNAL_PAL_FRAME_FLOATS        SIGNAL_FRAME_FLOATS(SIGNAL_PAL_SAMPLES_PER_LINE)   /* 614,400 */
#define SIGNAL_MAX_FRAME_FLOATS        SIGNAL_PAL_FRAME_FLOATS

/* Total buffer size in bytes. */
#define SIGNAL_NTSC_FRAME_BYTES        (SIGNAL_NTSC_FRAME_FLOATS * sizeof(float))  /* ~1.9 MB */
#define SIGNAL_PAL_FRAME_BYTES         (SIGNAL_PAL_FRAME_FLOATS * sizeof(float))   /* ~2.4 MB */
#define SIGNAL_MAX_FRAME_BYTES         (SIGNAL_MAX_FRAME_FLOATS * sizeof(float))

/* ============================================================================
 * Palette index buffer format (compact alternative upload)
 * ============================================================================
 * Instead of uploading the full float waveform, the CPU can upload the
 * raw NES palette+emphasis indices and let the GPU generate waveforms
 * from a precomputed signal table. Each pixel is one uint16_t:
 *
 *   bits 0..5: palette index (post-greyscale-mask)
 *   bits 6..8: PPUMASK emphasis (R=bit6 G=bit7 B=bit8)
 *
 * This matches ppu.index_framebuffer in our PPU implementation.
 */
#define SIGNAL_INDEX_PIXELS            (SIGNAL_NES_WIDTH * SIGNAL_NES_HEIGHT)  /* 61,440 */
#define SIGNAL_INDEX_BYTES             (SIGNAL_INDEX_PIXELS * sizeof(uint16_t))  /* ~120 KB */

/* Signal table dimensions (precomputed on GPU):
 *   512 entries (64 palette × 8 emphasis) × 12 phases per entry.
 *   Each entry is 12 floats (one full subcarrier cycle). */
#define SIGNAL_TABLE_ENTRIES           512
#define SIGNAL_TABLE_PHASES            12
#define SIGNAL_TABLE_FLOATS            (SIGNAL_TABLE_ENTRIES * SIGNAL_TABLE_PHASES)  /* 6,144 */

/* ============================================================================
 * GPU dispatch dimensions (compute shader workgroup sizing)
 * ============================================================================
 * The natural parallelism for 1D signal processing is one workgroup per
 * scanline, with each thread handling one sample. For prefix scan (RC
 * filter), workgroup size must be a power of 2.
 *
 *   Workgroup size: 1024 (fits NTSC's 2048 samples in 2 workgroups,
 *                         PAL's 2560 in 3 workgroups)
 *   Dispatches per scanline: ceil(samples_per_line / 1024)
 *   Total dispatches per frame: dispatches_per_scanline × 240
 *
 * For prefix scan with inter-block carry, the carry from block K feeds
 * into block K+1. Within a scanline, blocks execute left-to-right
 * (sequential dependency). Across scanlines, all 240 lines are
 * independent and can execute in parallel.
 */
#define SIGNAL_WORKGROUP_SIZE          1024
#define SIGNAL_NTSC_BLOCKS_PER_LINE    ((SIGNAL_NTSC_SAMPLES_PER_LINE + SIGNAL_WORKGROUP_SIZE - 1) / SIGNAL_WORKGROUP_SIZE)  /* 2 */
#define SIGNAL_PAL_BLOCKS_PER_LINE     ((SIGNAL_PAL_SAMPLES_PER_LINE + SIGNAL_WORKGROUP_SIZE - 1) / SIGNAL_WORKGROUP_SIZE)   /* 3 */

/* ============================================================================
 * Region enumeration
 * ============================================================================ */
#define SIGNAL_REGION_NTSC  0
#define SIGNAL_REGION_PAL   1

/* Runtime signal format descriptor. Set once at init, used by all
 * stages to determine dispatch dimensions. */
typedef struct {
    int region;                 /* SIGNAL_REGION_NTSC or SIGNAL_REGION_PAL */
    int samples_per_pixel;      /* 8 (NTSC) or 10 (PAL) */
    int samples_per_line;       /* 2048 (NTSC) or 2560 (PAL) */
    int lines;                  /* 240 (both regions) */
    int blocks_per_line;        /* ceil(samples_per_line / WORKGROUP_SIZE) */
    int total_samples;          /* samples_per_line × lines */
} SignalFormat;

/* Initialize a SignalFormat for a given region. */
static inline void signal_format_init(SignalFormat *fmt, int region) {
    fmt->region = region;
    if (region == SIGNAL_REGION_PAL) {
        fmt->samples_per_pixel = SIGNAL_PAL_SAMPLES_PER_PIXEL;
        fmt->samples_per_line  = SIGNAL_PAL_SAMPLES_PER_LINE;
    } else {
        fmt->samples_per_pixel = SIGNAL_NTSC_SAMPLES_PER_PIXEL;
        fmt->samples_per_line  = SIGNAL_NTSC_SAMPLES_PER_LINE;
    }
    fmt->lines = SIGNAL_NES_HEIGHT;
    fmt->blocks_per_line = (fmt->samples_per_line + SIGNAL_WORKGROUP_SIZE - 1)
                            / SIGNAL_WORKGROUP_SIZE;
    fmt->total_samples = fmt->samples_per_line * fmt->lines;
}

static inline int signal_region_normalize(int region) {
    return (region == SIGNAL_REGION_PAL) ? SIGNAL_REGION_PAL
                                         : SIGNAL_REGION_NTSC;
}

static inline float signal_region_subcarrier_hz(int region) {
    return (signal_region_normalize(region) == SIGNAL_REGION_PAL)
           ? SIGNAL_PAL_SUBCARRIER_HZ
           : SIGNAL_NTSC_SUBCARRIER_HZ;
}

static inline float signal_region_sample_rate_hz(int region) {
    return signal_region_subcarrier_hz(region)
           * SIGNAL_SAMPLES_PER_SUBCARRIER_CYCLE;
}

static inline float signal_format_sample_rate_hz(const SignalFormat *fmt) {
    return signal_region_sample_rate_hz(fmt ? fmt->region
                                            : SIGNAL_REGION_NTSC);
}

/* Clock-derived phase in the 12-sample carrier grid, including blanking. */
static inline int signal_region_line_phase(int region) {
    return (341 * (region == SIGNAL_REGION_PAL ? 10 : 8)) % 12;
}

static inline float signal_region_frame_ms(int region) {
    double dots = region == SIGNAL_REGION_PAL ? 341.0 * 312.0 : 341.0 * 262.0 - 0.5;
    double spp = region == SIGNAL_REGION_PAL ? 10.0 : 8.0;
    return (float)(1000.0 * dots * spp / signal_region_sample_rate_hz(region));
}

/* Per-channel scale changes the decay time constant, not its amplitude.
 * Zero explicitly disables persistence. */
static inline float signal_persistence_weight(int region, float tau_ms, float scale) {
    if (!(tau_ms > 0.0f) || !(scale > 0.0f)) return 0.0f;
    return expf(-signal_region_frame_ms(region) / (tau_ms * scale));
}

#endif /* SIGNAL_FORMAT_H */
