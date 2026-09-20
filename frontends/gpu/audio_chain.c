#include "audio_chain.h"
#include "kernels/rc_filter_ref.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Nominal filter corners; these are equivalent RC networks, not claimed
 * component measurements of individual consoles. NES: 90/440/14000 Hz. */
static const AudioFilterCorners console_corners[] = {
    AUDIO_CORNERS_FAMICOM, AUDIO_CORNERS_NES_FRONT,
    AUDIO_CORNERS_NES_TOP, AUDIO_CORNERS_DENDY
};

static AudioRCStage rc_stage(float corner, bool highpass) {
    return (AudioRCStage){ .enabled = true, .is_highpass = highpass,
        .resistance = 10000.0f,
        .capacitance = 1.0f / (2.0f * 3.14159265f * 10000.0f * corner) };
}

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
    if (console_variant < 0 || console_variant > AUDIO_CONSOLE_DENDY) console_variant = AUDIO_CONSOLE_NES_FRONT;
    if (speaker_type < 0 || speaker_type > AUDIO_SPEAKER_FAMICOM_RF) speaker_type = AUDIO_SPEAKER_SMALL_TV;
    chain->console_variant = console_variant;
    chain->sample_rate = AUDIO_STREAM_RATE;
    AudioFilterCorners corners = console_corners[console_variant];
    chain->coupling_cap = rc_stage(corners.hp1_hz, true);
    chain->feedback_network = rc_stage(corners.hp2_hz, true);
    chain->amp_bandwidth = rc_stage(corners.lp_hz, false);

    /* Stage 4: Amplifier saturation. */
    chain->amp_saturation.enabled = false;  /* off by default (linear) */
    chain->amp_saturation.drive = 1.0f;

    /* Stage 5: PSU hum. */
    chain->psu_hum.enabled = false;  /* off by default (clean PSU) */
    chain->psu_hum.frequency = (region == 1) ? 50.0f : 60.0f;
    chain->psu_hum.amplitude = 0.0f;
    chain->psu_hum.harmonic_2 = 0.0f;
    chain->psu_hum.harmonic_3 = 0.0f;

    /* Stage 6: Noise floor. */
    chain->noise_floor.enabled = false;
    chain->noise_floor.amplitude = 0.0f;

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

}

void audio_chain_params(const AudioChain *c, int count, AudioParams *p) {
    memset(p, 0, sizeof(*p));
    p->count = count;
    p->flags = (c->amp_saturation.enabled && !(c->bypass_mask & (1u << 3)) ? 1u : 0u) |
               (c->psu_hum.enabled && !(c->bypass_mask & (1u << 4)) ? 2u : 0u) |
               (c->noise_floor.enabled && !(c->bypass_mask & (1u << 5)) ? 4u : 0u) |
               (c->speaker.enabled && !(c->bypass_mask & (1u << 8)) ? 8u : 0u);
    const AudioRCStage *stages[] = { &c->coupling_cap, &c->feedback_network,
        &c->amp_bandwidth, &c->cable, &c->tv_input_coupling };
    for (int i = 0; i < 5; ++i) {
        p->rc[i][0] = stages[i]->a;
        p->rc[i][1] = stages[i]->b;
        p->rc[i][2] = stages[i]->is_highpass;
        const int stage_bits[] = {0, 1, 2, 6, 7};
        p->rc[i][3] = stages[i]->enabled && !(c->bypass_mask & (1u << stage_bits[i]));
    }
    memcpy(p->speaker[0], c->speaker.biquad_resonance, 5 * sizeof(float));
    memcpy(p->speaker[1], c->speaker.biquad_rolloff, 5 * sizeof(float));
    p->effects[0] = fmaxf(c->amp_saturation.drive, 0.01f);
    p->effects[1] = c->psu_hum.amplitude;
    p->effects[2] = 2.0f * 3.14159265f * c->psu_hum.frequency / c->sample_rate;
    p->effects[3] = c->noise_floor.amplitude;
    p->harmonics[0] = c->psu_hum.harmonic_2;
    p->harmonics[1] = c->psu_hum.harmonic_3;
}

static float process_rc(const float *p, float *state, float x) {
    float y = p[2] != 0 ? p[0] * (state[1] + x - state[0])
                        : p[0] * state[1] + p[1] * x;
    state[0] = x;
    state[1] = y;
    return p[3] != 0 ? y : x;
}

void audio_chain_process(const AudioChain *c, AudioState *s,
                         const float *input, float *output, int count) {
    AudioParams p;
    audio_chain_params(c, count, &p);
    if (!s->rng) s->rng = 42;
    for (int n = 0; n < count; ++n) {
        float y = input[n];
        for (int i = 0; i < 3; ++i) y = process_rc(p.rc[i], s->rc[i], y);
        if (p.flags & 1) y = tanhf(y * p.effects[0]) / p.effects[0];
        if (p.flags & 2) y += p.effects[1] * (sinf(s->hum_phase) +
            p.harmonics[0] * sinf(2 * s->hum_phase) + p.harmonics[1] * sinf(3 * s->hum_phase));
        s->hum_phase += p.effects[2];
        if (s->hum_phase >= 2 * 3.14159265f) s->hum_phase -= 2 * 3.14159265f;
        s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
        if (p.flags & 4) y += ((float)(s->rng >> 8) / 8388608.0f - 1.0f) * p.effects[3];
        for (int i = 3; i < 5; ++i) y = process_rc(p.rc[i], s->rc[i], y);
        if (p.flags & 8) for (int i = 0; i < 2; ++i) {
            const float *b = p.speaker[i];
            float z = b[0] * y + s->speaker[i][0];
            s->speaker[i][0] = b[1] * y - b[3] * z + s->speaker[i][1];
            s->speaker[i][1] = b[2] * y - b[4] * z;
            y = z;
        }
        output[n] = y;
    }
}
