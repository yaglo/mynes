/*
 * Audio Signal Chain — Initialization and Coefficient Computation
 * ================================================================
 *
 * Sets up the audio chain with physically-accurate component values
 * based on hardware variant (Famicom, NES, Dendy) and speaker type.
 * Converts R/C values to IIR coefficients for the GPU kernels.
 */

#include "audio_chain.h"
#include "kernels/rc_filter_ref.h"
#include "kernels/fir_ref.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Hardware variant component values (from NES schematics)
 * ============================================================================
 * These are the actual resistor and capacitor values on the NES/Famicom
 * motherboard that shape the audio signal before it reaches the output jack.
 *
 * Sources: NES hardware schematics, NesDev Wiki APU Mixer docs.
 */

/* Coupling capacitor (DC blocking, between DAC and output stage).
 * R = output load impedance, C = the physical capacitor. */
static const struct { float R; float C; } coupling_cap_values[] = {
    [AUDIO_CONSOLE_FAMICOM]   = { 10000.0f, 100e-6f },  /* fc ≈ 0.16 Hz (huge cap, very low cutoff) */
    [AUDIO_CONSOLE_NES_FRONT] = { 10000.0f, 10e-6f  },  /* fc ≈ 1.6 Hz */
    [AUDIO_CONSOLE_NES_TOP]   = { 10000.0f, 10e-6f  },  /* fc ≈ 1.6 Hz */
    [AUDIO_CONSOLE_DENDY]     = { 10000.0f, 47e-6f  },  /* fc ≈ 0.34 Hz */
};

/* Feedback network high-pass (shapes bass response). */
static const struct { float R; float C; } feedback_values[] = {
    [AUDIO_CONSOLE_FAMICOM]   = { 10000.0f, 0.033e-6f }, /* fc ≈ 482 Hz */
    [AUDIO_CONSOLE_NES_FRONT] = { 10000.0f, 0.036e-6f }, /* fc ≈ 442 Hz */
    [AUDIO_CONSOLE_NES_TOP]   = { 10000.0f, 0.036e-6f }, /* fc ≈ 442 Hz */
    [AUDIO_CONSOLE_DENDY]     = { 10000.0f, 0.036e-6f }, /* fc ≈ 442 Hz */
};

/* Amplifier bandwidth low-pass (op-amp GBW limit). */
static const struct { float R; float C; } amp_bw_values[] = {
    [AUDIO_CONSOLE_FAMICOM]   = { 75.0f, 0.22e-9f },   /* fc ≈ 9.6 kHz */
    [AUDIO_CONSOLE_NES_FRONT] = { 75.0f, 0.15e-9f },   /* fc ≈ 14.1 kHz */
    [AUDIO_CONSOLE_NES_TOP]   = { 75.0f, 0.18e-9f },   /* fc ≈ 11.8 kHz */
    [AUDIO_CONSOLE_DENDY]     = { 75.0f, 0.27e-9f },   /* fc ≈ 7.9 kHz */
};

/* Speaker presets (from audio_format.h). */
static const SpeakerParams speaker_presets[] = {
    [AUDIO_SPEAKER_SMALL_TV]   = { 350.0f, 400.0f,  6000.0f, 2.0f, 5000.0f },
    [AUDIO_SPEAKER_CONSOLE_TV] = { 150.0f,  80.0f, 10000.0f, 1.5f, 6000.0f },
    [AUDIO_SPEAKER_PVM]        = { 100.0f,  60.0f, 15000.0f, 0.8f, 8000.0f },
    [AUDIO_SPEAKER_ARCADE]     = { 200.0f, 100.0f,  8000.0f, 3.0f, 4000.0f },
    [AUDIO_SPEAKER_HEADPHONES] = {  20.0f,  20.0f, 20000.0f, 0.7f, 15000.0f },
    [AUDIO_SPEAKER_FAMICOM_RF] = { 400.0f, 500.0f,  4000.0f, 2.5f, 3500.0f },
};

/* ============================================================================
 * Initialization
 * ============================================================================ */

void audio_chain_init_preset(AudioChain *chain, int console_variant,
                              int speaker_type, int region) {
    memset(chain, 0, sizeof(AudioChain));
    chain->console_variant = console_variant;

    /* Set processing rate based on region. */
    chain->sample_rate = (region == 1) ? (float)AUDIO_PAL_CPU_CLOCK
                                        : (float)AUDIO_NTSC_CPU_CLOCK;

    /* Stage 1: Coupling capacitor (DC blocking high-pass). */
    chain->coupling_cap.enabled = true;
    chain->coupling_cap.is_highpass = true;
    chain->coupling_cap.resistance = coupling_cap_values[console_variant].R;
    chain->coupling_cap.capacitance = coupling_cap_values[console_variant].C;

    /* Stage 2: Feedback network (bass shaping high-pass). */
    chain->feedback_network.enabled = true;
    chain->feedback_network.is_highpass = true;
    chain->feedback_network.resistance = feedback_values[console_variant].R;
    chain->feedback_network.capacitance = feedback_values[console_variant].C;

    /* Stage 3: Amplifier bandwidth (low-pass). */
    chain->amp_bandwidth.enabled = true;
    chain->amp_bandwidth.is_highpass = false;
    chain->amp_bandwidth.resistance = amp_bw_values[console_variant].R;
    chain->amp_bandwidth.capacitance = amp_bw_values[console_variant].C;

    /* Stage 4: Amplifier saturation. */
    chain->amp_saturation.enabled = false;  /* off by default (linear) */
    chain->amp_saturation.drive = 1.0f;

    /* Stage 5: PSU hum. */
    chain->psu_hum.enabled = false;  /* off by default (clean PSU) */
    chain->psu_hum.frequency = (region == 1) ? 50.0f : 60.0f;
    chain->psu_hum.amplitude = 0.0f;
    chain->psu_hum.harmonic_2 = 0.0f;
    chain->psu_hum.harmonic_3 = 0.0f;
    chain->psu_hum.phase = 0.0f;

    /* Stage 6: Noise floor. */
    chain->noise_floor.enabled = false;
    chain->noise_floor.amplitude = 0.0f;
    chain->noise_floor.rng_state = 42;

    /* Stage 7: Cable capacitance (depends on cable length).
     * Default: 2m cable, 67 pF/m shunt capacitance, 75Ω impedance. */
    chain->cable.enabled = true;
    chain->cable.is_highpass = false;
    chain->cable.resistance = 75.0f;
    chain->cable.capacitance = 2.0f * 67e-12f;  /* 2m × 67 pF/m */

    /* Stage 8: TV input coupling cap. */
    chain->tv_input_coupling.enabled = true;
    chain->tv_input_coupling.is_highpass = true;
    chain->tv_input_coupling.resistance = 47000.0f;  /* TV input impedance */
    chain->tv_input_coupling.capacitance = 1e-6f;    /* 1 µF coupling cap */

    /* Stage 9: Speaker. */
    chain->speaker.enabled = (speaker_type != AUDIO_SPEAKER_HEADPHONES);
    chain->speaker.params = speaker_presets[speaker_type];

    /* Stage 10: Decimation (1.79 MHz → 48 kHz). */
    chain->decimation.enabled = true;
    chain->decimation.decimation_ratio =
        (int)(chain->sample_rate / (float)AUDIO_OUTPUT_SAMPLE_RATE);
    chain->decimation.tap_count = 65;  /* Kaiser-windowed sinc */
    chain->decimation.taps = NULL;     /* allocated in prepare() */

    /* Compute coefficients. */
    audio_chain_prepare(chain);
}

/* ============================================================================
 * Coefficient computation
 * ============================================================================ */

static void prepare_rc_stage(AudioRCStage *stage, float sample_rate) {
    if (!stage->enabled) return;
    if (stage->is_highpass) {
        rc_highpass_coeffs_from_fc(&stage->a, &stage->b,
            1.0f / (2.0f * 3.14159265f * stage->resistance * stage->capacitance),
            sample_rate);
    } else {
        rc_lowpass_coeffs(&stage->a, &stage->b,
            stage->resistance, stage->capacitance,
            sample_rate);
    }
}

/* Compute biquad coefficients for a 2nd-order resonant high-pass
 * (speaker resonance model). */
static void compute_speaker_biquad(float *coeffs, float f0, float Q,
                                    float sample_rate) {
    float w0 = 2.0f * 3.14159265f * f0 / sample_rate;
    float alpha = sinf(w0) / (2.0f * Q);
    float cos_w0 = cosf(w0);
    float a0 = 1.0f + alpha;
    /* High-pass biquad: */
    coeffs[0] = ((1.0f + cos_w0) / 2.0f) / a0;  /* b0/a0 */
    coeffs[1] = (-(1.0f + cos_w0)) / a0;          /* b1/a0 */
    coeffs[2] = ((1.0f + cos_w0) / 2.0f) / a0;   /* b2/a0 */
    coeffs[3] = (-2.0f * cos_w0) / a0;             /* a1/a0 */
    coeffs[4] = (1.0f - alpha) / a0;               /* a2/a0 */
}

/* Compute biquad coefficients for a 2nd-order low-pass
 * (speaker high-frequency rolloff). */
static void compute_rolloff_biquad(float *coeffs, float f0, float Q,
                                    float sample_rate) {
    float w0 = 2.0f * 3.14159265f * f0 / sample_rate;
    float alpha = sinf(w0) / (2.0f * Q);
    float cos_w0 = cosf(w0);
    float a0 = 1.0f + alpha;
    /* Low-pass biquad: */
    coeffs[0] = ((1.0f - cos_w0) / 2.0f) / a0;   /* b0/a0 */
    coeffs[1] = (1.0f - cos_w0) / a0;              /* b1/a0 */
    coeffs[2] = ((1.0f - cos_w0) / 2.0f) / a0;    /* b2/a0 */
    coeffs[3] = (-2.0f * cos_w0) / a0;              /* a1/a0 */
    coeffs[4] = (1.0f - alpha) / a0;                /* a2/a0 */
}

void audio_chain_prepare(AudioChain *chain) {
    float sr = chain->sample_rate;

    /* RC stages. */
    prepare_rc_stage(&chain->coupling_cap, sr);
    prepare_rc_stage(&chain->feedback_network, sr);
    prepare_rc_stage(&chain->amp_bandwidth, sr);
    prepare_rc_stage(&chain->cable, sr);
    prepare_rc_stage(&chain->tv_input_coupling, sr);

    /* Speaker biquads. */
    if (chain->speaker.enabled) {
        compute_speaker_biquad(chain->speaker.biquad_resonance,
            chain->speaker.params.resonance_hz,
            chain->speaker.params.cabinet_q,
            sr);
        compute_rolloff_biquad(chain->speaker.biquad_rolloff,
            chain->speaker.params.bandwidth_high,
            0.707f,  /* Butterworth Q */
            sr);
    }

    /* Decimation FIR. */
    if (chain->decimation.enabled) {
        if (chain->decimation.taps) free(chain->decimation.taps);
        chain->decimation.taps = (float *)malloc(
            chain->decimation.tap_count * sizeof(float));
        /* Cutoff at Nyquist of output rate (normalized to input rate). */
        float cutoff = 0.5f / (float)chain->decimation.decimation_ratio;
        fir_design_lowpass(chain->decimation.taps,
                           chain->decimation.tap_count,
                           cutoff);
    }
}

void audio_chain_destroy(AudioChain *chain) {
    if (chain->decimation.taps) {
        free(chain->decimation.taps);
        chain->decimation.taps = NULL;
    }
}
