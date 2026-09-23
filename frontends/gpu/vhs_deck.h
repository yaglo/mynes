/* NTSC SP VHS deck, host side of the two-kernel tape model.
 *
 * vhs_tape (V1) records the composite (keyed AGC, Y low-pass, IEC emphasis
 * and clip, colour-under band-pass), reads the recorded frequency back in
 * playback time through the transport timing, synthesises the FM carrier
 * with tape noise and dropouts, and demodulates it with a pulse-count
 * discriminator. vhs_playback (V2) runs the dropout compensator, de-emphasis,
 * noise canceller, sharpness, the Y delay line and the 1H chroma comb, then
 * up-converts the chroma onto the fixed carrier grid.
 *
 * This file designs the IIR filters as first-order partial-fraction
 * sections (bilinear transform, each factor prewarped at its own corner),
 * and builds the per-frame line table and dropout list. Both are pure
 * functions of (deck seed, emulated frame number), so skipped frames and
 * seeks need no state. Sources and measured values are in
 * docs/architecture/gpu-realism-validation.md.
 */
#ifndef VHS_DECK_H
#define VHS_DECK_H

#include <stdbool.h>
#include <stdint.h>
#include "gpu_compute.h"
#include "video_chain.h"

#define VHS_LINES 262
#define VHS_SPL 2728              /* 12 fsc samples per NES line */
#define VHS_SPL3 682              /* the same line at 3 fsc */
#define VHS_MASK_WORDS 171        /* dropout mask words per line, 16 samples each */
#define VHS_TABLE_LINES 264       /* previous frame's line 261, lines 0-261, next frame's line 0 */
#define VHS_MAX_DEFECTS 64
#define VHS_CLICK_TAPS 24

/* Filter slots in the coefficient buffer. Real-input filters store real
 * poles and one of each conjugate pair; complex-input filters store every
 * pole. The kernels know which kind each slot is. */
enum {
    VHS_F_YREC,    /* record Y: 3rd-order Bessel 3.4 MHz x fsc trap (Q 2) */
    VHS_F_PRE,     /* IEC pre-emphasis, tau 1.3 us, X 4 */
    VHS_F_CREC,    /* record chroma: 2nd-order Butterworth band-pass fsc +-0.5 MHz */
    VHS_F_MOD,     /* tape modulation noise: 2nd-order Butterworth 0.4 MHz */
    VHS_F_RF_REC,  /* record FM high-pass 1.6 MHz (3rd order) x head/tape pole */
    VHS_F_RF_PB,   /* playback RF: 2nd-order high-pass 1.4 MHz x low-pass 6 MHz */
    VHS_F_ENV,     /* dropout detector integrator, 0.35 us */
    VHS_F_YPB,     /* playback luma: 3rd-order Bessel 3.0 MHz */
    VHS_F_CPB,     /* playback colour-under band-pass: 2nd-order 0.5 MHz, 3 fsc */
    VHS_F_DE,      /* de-emphasis, exact inverse of VHS_F_PRE */
    VHS_F_CANC,    /* noise canceller split, first order */
    VHS_FILTERS
};
#define VHS_FILTER_SLOTS 16
#define VHS_MAX_SECTIONS 48
/* Buffer layout in vec4: VHS_FILTER_SLOTS headers (direct.re, direct.im,
 * first section, section count), VHS_MAX_SECTIONS sections (pole.re,
 * pole.im, residue.re, residue.im), then the DOC click taps. */
#define VHS_COEFF_VEC4 (VHS_FILTER_SLOTS + VHS_MAX_SECTIONS + VHS_CLICK_TAPS / 4)

/* One raster line of the transport table, in 12 fsc samples and dB.
 * tau is the playback delay at the line start; on the switch line the
 * old head ends at tau_old and the new head starts at tau_new at x_switch. */
typedef struct {
    float tau, tau_old, tau_new, x_switch;
    float rf_db, rf_db_old, noise, noise_new;
    float chroma_phase, rf_phase_jump, head, reserved;
} VHSLineEntry;

/* A tape defect in raster samples of the frame (tape position), with the
 * raster range in which its head scan is the one playing. */
typedef struct {
    float start, length, spacing_um, taper;
    float valid_from, valid_to, reserved0, reserved1;
} VHSDefect;

/* Field-coefficient cache for the transport model. */
#define VHS_HARMONICS 22
#define VHS_SLOW_TERMS 96
#define VHS_SLOW_RING 128

typedef struct {
    VHSParams p;
    double fs;
    float coeffs[VHS_COEFF_VEC4 * 4];
    GpuVHSParams gpu;
    double luma_delay_ns, chroma_delay_ns;   /* path delays found by the design */
    double path_samples;                     /* fixed input-to-output delay removed from tau */
    /* transport */
    double sig[VHS_HARMONICS + 1], rho, ramp, centre, offset[2], switch_line;
    int64_t slow_tag[VHS_SLOW_RING];
    double slow_g[VHS_SLOW_RING][2 * VHS_HARMONICS];
} VHSDeck;

/* Design every filter and the static GPU parameters for sample rate fs. */
void vhs_deck_configure(VHSDeck *d, const VHSParams *p, double fs);

/* Fill the line table and defect list for emulated frame number frame, and
 * the per-frame GPU parameters. Returns the defect count. */
int vhs_deck_frame(VHSDeck *d, uint32_t frame, VHSLineEntry table[VHS_TABLE_LINES],
                   VHSDefect defects[VHS_MAX_DEFECTS]);

/* Inspection helpers used by the unit tests. */
void vhs_deck_varying(VHSDeck *d, int64_t field, double coeff[2 * VHS_HARMONICS]);
typedef struct { int64_t field; double scan_start, length, depth_db; int track; } VHSTapeEvent;
int vhs_deck_field_events(const VHSDeck *d, int64_t field, VHSTapeEvent *out, int max);
/* Complex frequency response of one designed filter at f (Hz). */
void vhs_deck_response(const VHSDeck *d, int filter, double f, double *re, double *im);

#endif
