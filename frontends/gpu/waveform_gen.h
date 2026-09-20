/*
 * waveform_gen.h -- CPU waveform generation (Stage 1 DAC)
 *
 * Pure function: generates composite waveform from the palette index
 * framebuffer using the standalone SignalPrecompute voltage table.
 * Header-only -- no .c file needed.
 */
#ifndef WAVEFORM_GEN_H
#define WAVEFORM_GEN_H

#include <math.h>
#include "signal_precompute.h"
#include "video_chain.h"

static inline void waveform_generate(float *waveform, const uint16_t *idx_fb,
                                      const SignalPrecompute *sp, unsigned fc) {
    const int spp = sp->samples_per_pixel;
    const int spl = sp->samples_per_line;

    int field_ph = signal_frame_phase(sp, fc);
    int line_adv = sp->phase_line_adv;
    int base_ph  = sp->phase_base;

    for (int sy = 0; sy < 240; sy++) {
        int phase0 = ((base_ph + field_ph + sy * line_adv) % 12 + 120) % 12;

        const uint16_t *row = &idx_fb[sy * 256];
        const float *table_src = (sp->region == SIGNAL_REGION_PAL && (sy & 1))
                                 ? (const float *)sp->table_alt
                                 : (const float *)sp->table;
        const float (*tab)[SIG_TABLE_STRIDE] =
            (const float (*)[SIG_TABLE_STRIDE])table_src;

        int ph = phase0;
        float *dst = &waveform[sy * spl];

        if (spp == 8) {
            for (int px = 0; px < 256; px++) {
                uint16_t entry = row[px] & 0x1FF;
                const float *src = &tab[entry][ph];
                float *d = &dst[px * 8];
                d[0]=src[0]; d[1]=src[1]; d[2]=src[2]; d[3]=src[3];
                d[4]=src[4]; d[5]=src[5]; d[6]=src[6]; d[7]=src[7];
                ph += 8;
                if (ph >= 12) ph -= 12;
            }
        } else {
            for (int px = 0; px < 256; px++) {
                uint16_t entry = row[px] & 0x1FF;
                const float *src = &tab[entry][ph];
                float *d = &dst[px * spp];
                for (int s = 0; s < spp; s++) d[s] = src[s];
                ph += spp;
                while (ph >= 12) ph -= 12;
            }
        }
    }
}

/* ============================================================================
 * Beam-edge post-process
 * ============================================================================
 *
 * Applies the three tunable horizontal-edge behaviours to an already-
 * generated scanline waveform. Run AFTER waveform_generate. Harmless
 * when all four knobs are zero (fast-path early return).
 *
 *   tv->beam_edge_fade           — NES-pixel count zeroed per edge.
 *   tv->beam_edge_overshoot      — amplitude of transient ringing.
 *   tv->burst_lock_drift         — degrees of chroma-phase error applied
 *                                  to the leftmost pixels.
 *   tv->burst_lock_drift_width   — NES-pixel count receiving the drift.
 *
 * The burst-lock drift re-emits the affected pixels from the same
 * SignalPrecompute table at a rotated phase, preserving the authentic
 * voltage levels so downstream decoding stays consistent. */
static inline void waveform_apply_beam_edges(float *waveform,
                                              const uint16_t *idx_fb,
                                              const SignalPrecompute *sp,
                                              const TVDisplayParams *tv,
                                              unsigned fc) {
    if (!tv) return;
    const int spp = sp->samples_per_pixel;
    const int spl = sp->samples_per_line;

    float fade_px  = tv->beam_edge_fade;
    float osh_amp  = tv->beam_edge_overshoot;
    float drift_deg = tv->burst_lock_drift;
    float drift_wpx = tv->burst_lock_drift_width;

    if (fade_px < 0.0f) fade_px = 0.0f;
    if (osh_amp < 0.0f) osh_amp = 0.0f;
    if (drift_wpx < 0.0f) drift_wpx = 0.0f;

    /* Fast path: everything off. */
    if (fade_px < 1e-4f && osh_amp < 1e-4f &&
        (drift_wpx < 1e-4f || fabsf(drift_deg) < 1e-4f) && tv->beam_current_load < 1e-4f) {
        return;
    }

    int fade_samples = (int)(fade_px * (float)spp + 0.5f);
    if (fade_samples > spl / 2) fade_samples = spl / 2;
    /* Overshoot half-period: one subcarrier cycle = 12 samples at stock
     * 12-phase rate. Scale by samples-per-pixel so non-8x spp also fits. */
    int osh_span = spp + spp / 2;  /* ~1.5 NES pixels of ringing */

    int drift_samples = (int)(drift_wpx * (float)spp + 0.5f);
    if (drift_samples > spl) drift_samples = spl;

    /* Re-emit the leftmost `drift_samples` chroma samples with a rotated
     * phase. SignalPrecompute's 12-slot table is indexed by `ph` in
     * {0..11}; one subcarrier cycle = 360°, so slot offset = deg/30. */
    if (drift_samples > 0 && fabsf(drift_deg) > 1e-4f) {
        int field_ph = signal_frame_phase(sp, fc);
        int line_adv = sp->phase_line_adv;
        int base_ph  = sp->phase_base;
        float slot_shift = drift_deg / 30.0f;  /* 12 slots = 360° */
        for (int sy = 0; sy < 240; sy++) {
            int phase0 = ((base_ph + field_ph + sy * line_adv) % 12 + 120) % 12;
            const uint16_t *row = &idx_fb[sy * 256];
            float *dst = &waveform[sy * spl];
            const float *table_src =
                (sp->region == SIGNAL_REGION_PAL && (sy & 1))
                ? (const float *)sp->table_alt
                : (const float *)sp->table;
            const float (*tab)[SIG_TABLE_STRIDE] =
                (const float (*)[SIG_TABLE_STRIDE])table_src;
            int ph_int = phase0;
            for (int s = 0; s < drift_samples; s++) {
                int px = s / spp;
                int sub = s % spp;
                if (sub == 0) {
                    ph_int = (phase0 + (px * spp)) % 12;
                }
                /* Mix adjacent table slots proportional to the drift
                 * to get a fractional phase rotation. */
                float ph_f = (float)ph_int + (float)sub + slot_shift;
                int pA = (int)floorf(ph_f);
                float frac = ph_f - (float)pA;
                int pA_mod = ((pA % 12) + 12) % 12;
                int pB_mod = (pA_mod + 1) % 12;
                uint16_t entry = row[px] & 0x1FF;
                float a = tab[entry][pA_mod];
                float b = tab[entry][pB_mod];
                dst[s] = a + (b - a) * frac;
            }
        }
    }

    /* Per-scanline beam current loading: bright features along a line
     * drain the EHT supply progressively, so samples AFTER a bright
     * patch get dimmed by the accumulated sag; the left side of the
     * line (before any bright content) stays at full amplitude. Each
     * scanline begins with the HV recovered from the previous line's
     * horizontal retrace. We integrate luma as we sweep left-to-right
     * and apply an exponential-decay recovery so a narrow bright spike
     * sags the screen briefly rather than indefinitely.
     *
     *   drain coeff  = strength (per-sample drop per unit of bright signal)
     *   recover coeff = drain/8 (HV recharges slower than it drains)
     *
     * Saturates at sag=1.0 so amplitude can't flip sign.  */
    if (tv->beam_current_load > 1e-4f) {
        float strength = tv->beam_current_load;
        if (strength > 1.0f) strength = 1.0f;
        float drain_per_sample = strength / (float)spl;
        float recover_per_sample = drain_per_sample * 0.125f;
        for (int sy = 0; sy < 240; sy++) {
            float *line = &waveform[sy * spl];
            float sag = 0.0f;
            for (int s = 0; s < spl; s++) {
                float v = line[s];
                /* Drain scales with how bright THIS sample is;
                 * recovery is constant. */
                if (v > 0.0f) sag += v * drain_per_sample;
                sag -= recover_per_sample;
                if (sag < 0.0f) sag = 0.0f;
                if (sag > 0.9f) sag = 0.9f;
                line[s] = v * (1.0f - sag);
            }
        }
    }

    /* Edge overshoot + forced blanking fade. Applied per scanline. */
    if (fade_samples > 0 || osh_amp > 1e-4f) {
        for (int sy = 0; sy < 240; sy++) {
            float *line = &waveform[sy * spl];

            /* Left edge: zero the fade region, then ring on the next
             * `osh_span` samples with a cosine-modulated overshoot. */
            for (int s = 0; s < fade_samples; s++) line[s] = 0.0f;
            if (osh_amp > 1e-4f) {
                int s0 = fade_samples;
                int s1 = s0 + osh_span;
                if (s1 > spl) s1 = spl;
                for (int s = s0; s < s1; s++) {
                    float t = (float)(s - s0) / (float)osh_span;
                    float env = (1.0f - t) * cosf(t * 6.28318f);
                    line[s] *= 1.0f + osh_amp * env;
                }
            }

            /* Right edge: mirror behaviour. */
            for (int s = spl - fade_samples; s < spl; s++) line[s] = 0.0f;
            if (osh_amp > 1e-4f) {
                int s1 = spl - fade_samples;
                int s0 = s1 - osh_span;
                if (s0 < 0) s0 = 0;
                for (int s = s0; s < s1; s++) {
                    float t = (float)(s1 - s) / (float)osh_span;
                    float env = (1.0f - t) * cosf(t * 6.28318f);
                    line[s] *= 1.0f + osh_amp * env;
                }
            }
        }
    }
}

#endif /* WAVEFORM_GEN_H */
