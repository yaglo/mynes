#include "audio_chain.h"
#include "kernels/rc_filter_ref.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Filter corners per console (audio_format.h): the NES-001 pair comes from
 * its schematic through ngspice (tools/circuits/nes001_audio.cir), the
 * Famicom high-pass from its mixing network; the rest are equivalent
 * networks, not measurements. A corner of 0 leaves that stage out. */
static const AudioFilterCorners console_corners[] = {
    AUDIO_CORNERS_FAMICOM, AUDIO_CORNERS_NES_FRONT,
    AUDIO_CORNERS_NES_TOP, AUDIO_CORNERS_DENDY
};

static AudioRCStage rc_stage(float corner, bool highpass) {
    if (corner <= 0.0f) return (AudioRCStage){ .enabled = false, .is_highpass = highpass,
        .resistance = 10000.0f, .capacitance = 1e-6f };
    return (AudioRCStage){ .enabled = true, .is_highpass = highpass,
        .resistance = 10000.0f,
        .capacitance = 1.0f / (2.0f * 3.14159265f * 10000.0f * corner) };
}

/* Speaker profiles. The first six are generic. The two named sets take
 * their amplifier output network from the service manuals
 * (tools/circuits/README.md); their drivers are still class estimates:
 *   PVM-14L2: AN5278 (30 dB) through C3510 100 uF into the 7x5 cm speaker,
 *     199 Hz at 8 ohm; a small oval driver near 250 Hz, Q about 0.9.
 *   Toshiba 14AF43: AN5276 (34 dB) through 1000 uF into 8 ohm 5 W, 20 Hz;
 *     round drivers near 140 Hz, Q about 0.8. */
static const SpeakerParams speaker_presets[] = {
    [AUDIO_SPEAKER_SMALL_TV]   = { 350.0f, 400.0f,  6000.0f, 2.0f, 5000.0f, 0.0f },
    [AUDIO_SPEAKER_CONSOLE_TV] = { 150.0f,  80.0f, 10000.0f, 1.5f, 6000.0f, 0.0f },
    [AUDIO_SPEAKER_PVM]        = { 100.0f,  60.0f, 15000.0f, 0.8f, 8000.0f, 0.0f },
    [AUDIO_SPEAKER_ARCADE]     = { 200.0f, 100.0f,  8000.0f, 3.0f, 4000.0f, 0.0f },
    [AUDIO_SPEAKER_HEADPHONES] = {  20.0f,  20.0f, 20000.0f, 0.7f, 15000.0f, 0.0f },
    [AUDIO_SPEAKER_FAMICOM_RF] = { 400.0f, 500.0f,  4000.0f, 2.5f, 3500.0f, 0.0f },
    [AUDIO_SPEAKER_PVM_14L2]   = { 250.0f, 200.0f,  8000.0f, 0.9f, 6000.0f, 199.0f },
    /* Named sets from their service manuals, spec sheets and brochures
     * (tools/circuits/README.md, monitor speakers): the amplifier's output
     * capacitor into the driver is the sourced part; the drivers' resonance,
     * Q and top end are class values for their size and cabinet. */
    [AUDIO_SPEAKER_TOSHIBA_14AF43] = { 220.0f, 150.0f, 8000.0f, 1.3f, 5000.0f, 19.9f },  /* 4x7 cm ovals, 1000 uF into 8 ohm */
    [AUDIO_SPEAKER_JVC_AV27D]  = { 150.0f,  90.0f,  9000.0f, 1.3f, 5000.0f, 0.0f },     /* 5x12 cm ovals; amplifier unknown */
    [AUDIO_SPEAKER_WEGA_27FS]  = { 130.0f,  80.0f, 10000.0f, 1.2f, 5500.0f, 0.0f },     /* 6x12 cm on a bridge: no capacitor */
    [AUDIO_SPEAKER_COMMODORE_1702] = { 160.0f, 100.0f, 8000.0f, 1.2f, 5000.0f, 19.9f }, /* 10 cm, 1000 uF into 8 ohm */
    [AUDIO_SPEAKER_ZENITH_19]  = { 120.0f,  80.0f,  7000.0f, 1.5f, 4500.0f, 42.0f },    /* 4x6 in oval class, 470 uF into 8 ohm (estimates) */
    [AUDIO_SPEAKER_RCA_CONSOLE] = { 75.0f,  50.0f, 15000.0f, 1.0f, 7000.0f, 10.0f },    /* 5 in woofers and tweeters, 50 Hz to 15 kHz */
    [AUDIO_SPEAKER_PVM_20M4U]  = { 200.0f, 150.0f,  8000.0f, 1.0f, 5000.0f, 42.0f },    /* about 8 cm mono, 470 uF into 8 ohm (estimates) */
    [AUDIO_SPEAKER_NEC_XM29]   = { 180.0f, 120.0f,  9000.0f, 1.2f, 5000.0f, 10.0f },    /* 9x5.5 cm 16 ohm ovals, 1000 uF (estimate) */
};

/* ============================================================================
 * Initialization
 * ============================================================================ */

void audio_chain_init_preset(AudioChain *chain, int console_variant,
                              int speaker_type, int region) {
    memset(chain, 0, sizeof(AudioChain));
    if (console_variant < 0 || console_variant > AUDIO_CONSOLE_DENDY) console_variant = AUDIO_CONSOLE_NES_FRONT;
    if (speaker_type < 0 || speaker_type > AUDIO_SPEAKER_LAST) speaker_type = AUDIO_SPEAKER_SMALL_TV;
    chain->console_variant = console_variant;
    chain->sample_rate = AUDIO_STREAM_RATE;
    AudioFilterCorners corners = console_corners[console_variant];
    chain->coupling_cap = rc_stage(corners.hp1_hz, true);
    chain->feedback_network = rc_stage(corners.lp2_hz, false); /* output-pin pole; the slot's name is historical */
    chain->amp_bandwidth = rc_stage(corners.lp_hz, false);

    /* Stage 4: Amplifier saturation (legacy) and the gate's rail window. */
    chain->amp_saturation.enabled = false;  /* off by default (linear) */
    chain->amp_saturation.drive = 1.0f;
    chain->rail_clip.enabled = false;
    chain->rail_clip.window = 0.75f;        /* 3 V peak to peak at 2 V per unit */
    chain->rail_clip.knee = 0.15f;
    chain->rf_deemphasis_us = 0.0f;

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

    /* Stage 9: Speaker, with its amplifier's output capacitor (stage 10). */
    chain->speaker.enabled = (speaker_type != AUDIO_SPEAKER_HEADPHONES);
    chain->speaker.params = speaker_presets[speaker_type];
    chain->speaker_coupling = rc_stage(chain->speaker.enabled ? chain->speaker.params.coupling_hp_hz : 0.0f, true);

    /* Stage 11: RF sound buzz; the preset enables it on RF connections. */
    chain->rf_sound.enabled = false;
    chain->rf_sound.am_rejection = powf(10.0f, -45.0f / 20.0f);
    chain->rf_sound.icpm_rad = 0.0f;
    chain->rf_sound.full_deviation = 0.7f;
    chain->rf_sound.deviation_hz = (region == 1) ? 50000.0f : 25000.0f;
    chain->rf_sound.line_hz = (region == 1) ? 15625.0f : 15734.264f;
    chain->rf_sound.lines = (region == 1) ? 312 : 262;

    /* Compute coefficients. */
    audio_chain_prepare(chain);
}

/* The supply is run rather than fitted: the adaptor's sine through its
 * source resistance and two diode drops into the reservoir, the console
 * and the modulator drawing from it, and the 7805 as the deck has it,
 * 5 V plus the input ripple over its rejection until the input falls
 * within its dropout, where the output follows the input. Twelve mains
 * cycles settle the reservoir; the last two give the twice-mains component
 * and its harmonics by DFT. About 40k steps, once per preset apply. */
AudioPsuDerived audio_psu_derive(float adaptor_vac, float reservoir_uf, float load_ma,
                                 float rejection_db, float mains_hz, int console_variant) {
    AudioPsuDerived d = {0};
    if (adaptor_vac <= 0) adaptor_vac = AUDIO_NES001_ADAPTOR_VAC;
    if (reservoir_uf <= 0) reservoir_uf = AUDIO_NES001_RESERVOIR_UF;
    if (load_ma <= 0) load_ma = AUDIO_NES001_LOAD_MA;
    if (rejection_db <= 0) rejection_db = AUDIO_NES001_REJECTION_DB;
    if (mains_hz <= 0) mains_hz = 60.0f;
    const float dt = 5e-6f, two_pi = 6.2831853f;
    const float i_load = load_ma * 1e-3f, c = reservoir_uf * 1e-6f, peak = adaptor_vac * 1.41421356f;
    const float rr = powf(10.0f, -rejection_db / 20.0f);
    const int per_cycle = (int)(1.0f / (mains_hz * dt) + 0.5f), settle = 10, measure = 2;
    const float n_vt = AUDIO_NES001_PSU_DIODE_N * 0.02585f;
    float vc = peak - 1.8f, vmin = 1e9f, vmax = -1e9f;
    double re[3] = {0, 0, 0}, im[3] = {0, 0, 0};
    long n_measure = 0;
    for (int cycle = 0; cycle < settle + measure; ++cycle) {
        for (int k = 0; k < per_cycle; ++k) {
            float t = (float)k / (float)per_cycle;
            float vac = fabsf(peak * sinf(two_pi * t));
            /* Two diodes of the bridge conduct in series with the source:
             * vac - vc = r_src i + 2 (n Vt ln(1 + i/Is) + Rs i), by Newton. */
            float i_in = 0.0f, excess = vac - vc - 1.6f;
            if (excess > 0) {
                i_in = excess / AUDIO_NES001_PSU_SOURCE_OHM;
                for (int it = 0; it < 4; ++it) {
                    float f = AUDIO_NES001_PSU_SOURCE_OHM * i_in + 2.0f * (n_vt * logf(1.0f + i_in / AUDIO_NES001_PSU_DIODE_IS)
                            + AUDIO_NES001_PSU_DIODE_RS * i_in) - (vac - vc);
                    float df = AUDIO_NES001_PSU_SOURCE_OHM + 2.0f * (n_vt / (AUDIO_NES001_PSU_DIODE_IS + i_in) + AUDIO_NES001_PSU_DIODE_RS);
                    i_in -= f / df;
                    if (i_in < 0) { i_in = 0; break; }
                }
            }
            vc += (i_in - i_load - AUDIO_NES001_PSU_MODULATOR_A) * dt / c;
            if (cycle >= settle) {
                float v5 = fminf(5.0f + (vc - 12.0f) * rr, vc - AUDIO_NES001_PSU_DROPOUT_V);
                if (vc < vmin) vmin = vc;
                if (vc > vmax) vmax = vc;
                for (int h = 0; h < 3; ++h) {
                    float a = two_pi * (float)(2 * (h + 1)) * t;
                    re[h] += v5 * cosf(a);
                    im[h] += v5 * sinf(a);
                }
                ++n_measure;
            }
        }
    }
    float amp[3];
    for (int h = 0; h < 3; ++h) amp[h] = 2.0f * (float)hypot(re[h], im[h]) / (float)n_measure;
    d.raw_min_v = vmin;
    d.raw_pp_v = vmax - vmin;
    d.rail_ripple_v = amp[0];
    d.harmonic_2 = amp[0] > 0 ? amp[1] / amp[0] : 0.0f;
    d.harmonic_3 = amp[0] > 0 ? amp[2] / amp[0] : 0.0f;
    d.dropout = vmin < 5.0f + AUDIO_NES001_PSU_DROPOUT_V;
    bool gate = console_variant == AUDIO_CONSOLE_NES_FRONT || console_variant == AUDIO_CONSOLE_NES_TOP;
    d.jack_v = gate ? d.rail_ripple_v * powf(10.0f, AUDIO_NES001_SUPPLY_GAIN_DB / 20.0f) : 0.0f;
    return d;
}

/* Terminated 2C02 levels (NESdev NTSC video, lidnariq), the same table as
 * signal_precompute.h: low and high of the four luma rows, then the same
 * with emphasis. A code's mean over its 12 phases is its luma. */
void audio_video_frame_from_codes(AudioVideoFrame *frame, const uint16_t *codes, int region) {
    static const float levels[16] = { 228, 312, 552, 880, 616, 840, 1100, 1100,
                                      192, 256, 448, 712, 500, 676, 896, 896 };
    static float mean_of_code[512];
    static bool built = false;
    if (!built) {
        for (int code = 0; code < 512; ++code) {
            int color = code & 0x0F, level = (code >> 4) & 3, emph = code >> 6;
            if (color > 13) level = 1;
            float sum = 0;
            for (int p = 0; p < 12; ++p) {
                int in_hi = color == 0 || (color < 13 && ((color + p) % 12) < 6);
                int octant = p >> 1, mask = (0264513 >> (3 * octant)) & 7;
                int attenuated = color < 14 && (emph & mask) ? 8 : 0;
                sum += levels[level + (in_hi ? 4 : 0) + attenuated];
            }
            mean_of_code[code] = (sum / 12.0f - 312.0f) / (1100.0f - 312.0f);
        }
        built = true;
    }
    int lines = region == 1 ? 312 : 262;
    frame->lines = lines;
    for (int l = 0; l < lines; ++l) {
        float sum = 0;
        if (l < 240) {
            const uint16_t *row = codes + l * 256;
            for (int x = 0; x < 256; ++x) sum += mean_of_code[row[x] & 511];
            sum /= 256.0f;
        }
        frame->level[l] = sum;
    }
}

void audio_chain_rf_buzz(const AudioChain *c, AudioVideoFrame *f, float *aux, int count) {
    const AudioRFSoundStage *r = &c->rf_sound;
    int lines = f->lines > 0 && f->lines <= AUDIO_FRAME_LINES ? f->lines : r->lines;
    /* Vision carrier envelope relative to sync tip: blanking 0.75, white
     * 0.125 (System M negative modulation). */
    float env[AUDIO_FRAME_LINES], mean = 0;
    for (int l = 0; l < lines; ++l) {
        env[l] = 0.75f - 0.625f * fminf(fmaxf(f->level[l], 0.0f), 1.2f);
        mean += env[l];
    }
    mean /= (float)lines;
    float k_am = r->am_rejection * r->full_deviation;
    float k_pm = r->full_deviation / (2.0f * 3.14159265f * r->deviation_hz);
    /* The sync pulses: 4.7 us of the line 25% under blanking, a line-rate
     * AM tone of that fundamental; content-free, so a plain oscillator. */
    float a_line = k_am * (2.0f * 0.25f * sinf(3.14159265f * 4.7e-6f * r->line_hz) / 3.14159265f) / mean;
    float step = 2.0f * 3.14159265f * r->line_hz / c->sample_rate;
    for (int n = 0; n < count; ++n) {
        int l = (int)((long long)n * lines / (count > 0 ? count : 1));
        if (l >= lines) l = lines - 1;
        int prev = l ? l - 1 : lines - 1;
        float m = env[l] / mean - 1.0f;
        float dphi = r->icpm_rad * (f->level[l] - f->level[prev]) * r->line_hz;
        aux[n] = k_am * m + k_pm * dphi + a_line * sinf(f->line_phase);
        f->line_phase += step;
        if (f->line_phase >= 2.0f * 3.14159265f) f->line_phase -= 2.0f * 3.14159265f;
    }
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

    /* RC stages. On RF the cable slot is the set's sound de-emphasis. */
    prepare_rc_stage(&chain->coupling_cap, sr);
    prepare_rc_stage(&chain->feedback_network, sr);
    prepare_rc_stage(&chain->amp_bandwidth, sr);
    if (chain->rf_deemphasis_us > 0.0f) {
        chain->cable.enabled = true;
        chain->cable.is_highpass = false;
        chain->cable.resistance = 1.0f;
        chain->cable.capacitance = chain->rf_deemphasis_us * 1e-6f;
    }
    prepare_rc_stage(&chain->cable, sr);
    prepare_rc_stage(&chain->tv_input_coupling, sr);
    prepare_rc_stage(&chain->speaker_coupling, sr);

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
               (c->speaker.enabled && !(c->bypass_mask & (1u << 8)) ? 8u : 0u) |
               (c->rail_clip.enabled && !(c->bypass_mask & (1u << 3)) ? 16u : 0u) |
               (c->rf_sound.enabled && !(c->bypass_mask & (1u << 10)) ? 32u : 0u);
    const AudioRCStage *stages[] = { &c->coupling_cap, &c->feedback_network,
        &c->amp_bandwidth, &c->cable, &c->tv_input_coupling, &c->speaker_coupling };
    for (int i = 0; i < 6; ++i) {
        p->rc[i][0] = stages[i]->a;
        p->rc[i][1] = stages[i]->b;
        p->rc[i][2] = stages[i]->is_highpass;
        const int stage_bits[] = {0, 1, 2, 6, 7, 9};
        p->rc[i][3] = stages[i]->enabled && !(c->bypass_mask & (1u << stage_bits[i]));
    }
    p->clip[0] = fmaxf(c->rail_clip.window, 1e-3f);
    p->clip[1] = fminf(fmaxf(c->rail_clip.knee, 0.01f), 0.5f);
    memcpy(p->speaker[0], c->speaker.biquad_resonance, 5 * sizeof(float));
    memcpy(p->speaker[1], c->speaker.biquad_rolloff, 5 * sizeof(float));
    p->effects[0] = fmaxf(c->amp_saturation.drive, 0.01f);
    p->effects[1] = c->psu_hum.amplitude;
    p->effects[2] = 2.0f * 3.14159265f * c->psu_hum.frequency / c->sample_rate;
    p->effects[3] = c->noise_floor.amplitude;
    p->harmonics[0] = c->psu_hum.harmonic_2;
    p->harmonics[1] = c->psu_hum.harmonic_3;
}

/* The gate's rail limit: linear to (1 - knee) of the window, then the last
 * knee share rounds into the rail. Same expression in audio_stream.comp. */
static float rail_clip(float y, float window, float knee) {
    float lin = window * (1.0f - knee), a = fabsf(y);
    if (a <= lin) return y;
    float out = lin + window * knee * tanhf((a - lin) / (window * knee));
    return y < 0.0f ? -out : out;
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
    audio_chain_process_aux(c, s, input, NULL, output, count);
}

void audio_chain_process_aux(const AudioChain *c, AudioState *s,
                             const float *input, const float *aux, float *output, int count) {
    AudioParams p;
    audio_chain_params(c, count, &p);
    if (!aux) p.flags &= ~32u;
    if (!s->rng) s->rng = 42;
    for (int n = 0; n < count; ++n) {
        float y = input[n];
        for (int i = 0; i < 3; ++i) y = process_rc(p.rc[i], s->rc[i], y);
        if (p.flags & 1) y = tanhf(y * p.effects[0]) / p.effects[0];
        if (p.flags & 16) y = rail_clip(y, p.clip[0], p.clip[1]);
        if (p.flags & 2) y += p.effects[1] * (sinf(s->hum_phase) +
            p.harmonics[0] * sinf(2 * s->hum_phase) + p.harmonics[1] * sinf(3 * s->hum_phase));
        s->hum_phase += p.effects[2];
        if (s->hum_phase >= 2 * 3.14159265f) s->hum_phase -= 2 * 3.14159265f;
        s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
        if (p.flags & 4) y += ((float)(s->rng >> 8) / 8388608.0f - 1.0f) * p.effects[3];
        if (p.flags & 32) y += aux[n];
        for (int i = 3; i < 6; ++i) y = process_rc(p.rc[i], s->rc[i], y);
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
