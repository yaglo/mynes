/*
 * NES APU (Audio Processing Unit)
 *
 * Full audio synthesis with pulse, triangle, noise, and DMC channels.
 * Implements frame counter, length counters, envelopes, sweep units,
 * and sample generation with filtering.
 */

#ifndef NES_APU_H
#define NES_APU_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* Strict C11 hides M_PI (it is a POSIX extension). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Hook macros are defined in nes/hooks.h (included before this file by nes.h).
 * Provide no-op fallbacks if used standalone. */
#ifndef HOOK_APU_REG
#define HOOK_APU_REG(addr, val) ((void)0)
#define HOOK_APU_FRAME(step, quarter, half, irq) ((void)0)
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define APU_CPU_CLOCK_NTSC  1789773
#define APU_CPU_CLOCK_PAL   1662607
#define APU_SAMPLE_RATE     44100
#define APU_SAMPLE_BUF_SIZE 2048

/* Resampling filter for CPU-rate → output-rate conversion.
 * Ring buffer holds the most recent raw DAC samples; a Kaiser-windowed
 * sinc FIR convolves them into a single output sample whenever one
 * is due. Ring size must be a power of 2 >= tap count. */
#define APU_RESAMPLE_RING 512
#define APU_RESAMPLE_TAPS 129

/* Analog output path — one-pole DF1 highpass + highpass + lowpass
 * cascade simulating the 2A03 amplifier stage and output coupling.
 *
 *   hp1:  DC-blocking output coupling cap. Very low corner, removes
 *         the ~0.5 of full scale DC bias from the nonlinear mixer.
 *   hp2:  amp feedback network. Low-bass shaping.
 *   lp:   RC rolloff at the output. Cuts the remaining high-frequency
 *         content that survived the resample FIR.
 *
 * Stored as runtime coefficients so commit-7 presets can swap them. */
typedef struct {
    double hp1_alpha;   /* highpass 1 (output coupling cap) */
    double hp2_alpha;   /* highpass 2 (amp feedback) */
    double lp_alpha;    /* lowpass   (RC output) */
} APUFilterConfig;

/* Analog character layer — applied AFTER the filter chain. Each field
 * defaults to neutral (zero or 1.0) so enabling the struct produces no
 * audible change until the user tunes it. These simulate the analog
 * output stage of real NES hardware: soft-clipping amp, cartridge
 * thermal/supply noise, mains hum bleeding through the output coupling,
 * the $4011 DMC bus pulse bleeding into the audio mix, and the
 * subtle nonlinearity of the 2A03 DAC ladder. */
typedef struct {
    APUFilterConfig filter;     /* the filter chain from commit 5 */
    float dac_nonlinearity;     /* 0=linear LUT, 1=cubic curve (drives DAC rebuild) */
    float saturation;           /* 0=none, 1=heavy tanh soft-clip */
    float noise_floor;          /* 0..0.02 peak amplitude of white noise */
    float hum_60hz;             /* 0..0.02 peak amplitude of 60 Hz mains */
    float dmc_bus_crosstalk;    /* 0..1 extra DMC bleed into output */
    float output_gain;          /* final stage multiply, default 1.0 */
} APUAnalog;

/* ============================================================================
 * Lookup Tables
 * ============================================================================ */

static const uint8_t apu_length_table[32] = {
    10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

static const uint8_t apu_duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},  /* 12.5% */
    {0, 1, 1, 0, 0, 0, 0, 0},  /* 25% */
    {0, 1, 1, 1, 1, 0, 0, 0},  /* 50% */
    {1, 0, 0, 1, 1, 1, 1, 1},  /* 75% (inverted 25%) */
};

static const uint8_t apu_triangle_table[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
     0,  1,  2,  3,  4,  5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint16_t apu_noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const uint16_t apu_noise_period_table_pal[16] = {
    4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778
};

static const uint16_t apu_dmc_rate_table_pal[16] = {
    398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50
};

static const uint16_t apu_dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106,  84,  72,  54
};

/* Frame counter step timings in CPU cycles */
static const int apu_frame_steps_mode0[6] = {7456, 14912, 22370, 29827, 29828, 29829};
static const int apu_frame_steps_mode1[5] = {7456, 14912, 22370, 37280, 37280};

static const int apu_frame_steps_mode0_pal[6] = {8312, 16626, 24938, 33251, 33252, 33253};
static const int apu_frame_steps_mode1_pal[5] = {8312, 16626, 24938, 41564, 41564};

/* ============================================================================
 * Channel Structures
 * ============================================================================ */

typedef struct {
    uint8_t reg[4];

    int timer;
    int timer_reload;
    int sequence_step;
    int length_counter;

    int envelope_divider;
    int envelope_decay;
    bool envelope_start;

    bool sweep_enable;
    bool sweep_negate;
    uint8_t sweep_period;
    uint8_t sweep_shift;
    int sweep_divider;
    bool sweep_reload;
    bool sweep_mute;

    bool enabled;
} APU_Pulse;

typedef struct {
    uint8_t reg0;
    uint8_t reg2;
    uint8_t reg3;

    int timer;
    int timer_reload;
    int sequence_step;
    int length_counter;
    int linear_counter;
    bool linear_reload;
    bool enabled;
} APU_Triangle;

typedef struct {
    uint8_t reg0;
    uint8_t reg2;
    uint8_t reg3;

    int timer;
    int length_counter;

    int envelope_divider;
    int envelope_decay;
    bool envelope_start;

    uint16_t lfsr;
    bool enabled;
} APU_Noise;

typedef struct {
    uint8_t reg[4];

    int timer;
    int timer_reload;

    uint16_t sample_address;
    uint16_t current_address;
    int sample_length;
    int bytes_remaining;

    uint8_t sample_buffer;
    bool sample_buffer_empty;

    uint8_t shift_register;
    int bits_remaining;
    uint8_t output_level;

    bool irq_flag;
    bool loop_flag;
    bool irq_enabled;
    bool silence_flag;
    bool enabled;
} APU_DMC;

typedef struct {
    int cycle;
    int step;
    bool irq_inhibit;
    bool five_step;
    bool irq_flag;
    int write_delay;
    uint8_t pending_value;
    bool pending_write;
} APU_FrameCounter;

/* Audio callback type: called with buffer of float samples */
typedef void (*apu_audio_callback_t)(void *user_data, float sample);

/* ============================================================================
 * APU State
 * ============================================================================ */

typedef struct APU {
    /* Channels */
    APU_Pulse pulse[2];
    APU_Triangle triangle;
    APU_Noise noise;
    APU_DMC dmc;

    /* Frame counter */
    APU_FrameCounter frame;
    /* CPU writes reach the length units after the next sequencer clock.
     * A reload coinciding with a length clock is ignored if already nonzero. */
    uint8_t length_halt;
    uint8_t length_writes; /* bits 0..3 reload, bit 4 refresh halt inputs */
    uint8_t length_reload[4], length_reload_old[4];
    bool length_clocked;


    /* Sample generation.
     * Philosophy: we compute ONE raw DAC sample per CPU cycle
     * (~1.79 MHz) and push it into a ring buffer. When the fractional
     * sample_accumulator crosses the CPU clock, we convolve the last
     * APU_RESAMPLE_TAPS samples with a Kaiser-windowed sinc FIR to
     * produce one output sample at ~44.1 kHz. The filter is designed
     * with cutoff just below the output Nyquist (22 kHz), so aliasing
     * from step waveforms (pulse duty edges, LFSR flips, triangle
     * steps, DMC delta) is rejected by the filter's stopband — not
     * folded back into the audible band like a point sampler would. */
    int    sample_accumulator;  /* fractional phase in input samples */
    int    sample_rate;         /* effective output rate (adjusted for sync) */
    bool   pal;                /* 2A07 frame sequencer and noise/DMC divisors */
    int    cpu_clock;           /* CPU clock rate (NTSC or PAL) */

    /* Change detection: true when any channel has reported a state
     * change since the last mixer evaluation. The mixer is only
     * recomputed when dirty is set, then the cached `last_raw_sample`
     * is pushed into the resample ring every CPU cycle. Saves ~95%
     * of mixer evaluations on typical content. */
    bool   dirty;
    float  last_raw_sample;

    /* Resample ring buffer (must be aligned for SIMD). */
    _Alignas(32) float resample_ring[APU_RESAMPLE_RING];
    int    resample_write;
    _Alignas(32) float resample_taps[APU_RESAMPLE_TAPS];

    /* 2A03 DAC voltage lookup tables.
     * pulse_dac[n]  for n = pulse1 + pulse2 (0..30)
     * tnd_dac[n]    for n = 3*tri + 2*noise + dmc (0..202)
     * Precomputed from the NesDev linear approximation of the
     * hardware nonlinear mixer at init. A single lookup per channel
     * per CPU cycle replaces the divisions in the old formula. */
    float  pulse_dac[31];
    float  tnd_dac[203];

    /* Analog filter chain state (highpass1 + highpass2 + lowpass).
     * Coefficients live in filter_config so they can be swapped at
     * runtime for different hardware models (Famicom / NES / Dendy). */
    APUFilterConfig filter_config;
    double hp_filter1;
    double hp_filter2;
    double lp_filter;
    double hp_prev_input1;
    double hp_prev_input2;

    /* Analog character layer (saturation, noise, hum, crosstalk).
     * Applied after the filter chain in the final output stage. */
    APUAnalog analog;
    uint32_t  noise_rng;        /* xorshift32 state for noise_floor */
    double    hum_phase;        /* radians, wraps at 2*pi */

    /* Audio callback */
    apu_audio_callback_t audio_callback;
    void *audio_user_data;

    /* Legacy compatibility fields (used by NES system for IRQ/DMA) */
    bool frame_irq_pending;
    bool dmc_irq_pending;
    uint16_t dmc_current_addr;
    uint16_t dmc_bytes_remaining;
    bool dmc_sample_buffer_empty;

    /* APU put/get cycle parity. Toggled at the start of each apu_step.
     * AccuracyCoin Frame Counter IRQ test 7 distinguishes whether a $4015
     * read happens on a "put" or "get" cycle:
     *   - read on get cycle → clear bit 6 immediately
     *   - read on put cycle → defer clear: the SAME cycle's read still sees
     *     bit 6 set, the NEXT cycle's read also still sees it set, and the
     *     clear lands one apu cycle after that.
     * Mirrors C# Emulator.cs APU_PutCycle. */
    bool put_cycle;
    bool frame_irq_clear_pending;
    uint8_t frame_irq_clear_delay;  /* apu_step decrements; clear when 0 */

    /* DMC DMA delay counter — set when $4015 enables the DMC and starts a
     * sample. The DMA can't fire immediately; it needs ~2 APU cycles
     * before the CPU is halted. Decrements per apu_step. While > 0,
     * apu_dmc_needs_sample() returns false (DMA gated). */
    uint8_t dmc_dma_delay;
    uint8_t dmc_abort_delay;
    bool dmc_abort_pending;
    bool dmc_late_request;

    /* Register storage */
    uint8_t regs[0x18];
} APU;

/* ============================================================================
 * Inline Helpers
 * ============================================================================ */

static inline bool apu_pulse_length_halt(const APU_Pulse *p) {
    return (p->reg[0] & 0x20) != 0;
}
static inline bool apu_pulse_constant_volume(const APU_Pulse *p) {
    return (p->reg[0] & 0x10) != 0;
}
static inline int apu_pulse_volume(const APU_Pulse *p) {
    return p->reg[0] & 0x0F;
}
static inline int apu_pulse_duty(const APU_Pulse *p) {
    return (p->reg[0] >> 6) & 0x03;
}
static inline bool apu_triangle_control_flag(const APU_Triangle *t) {
    return (t->reg0 & 0x80) != 0;
}
static inline int apu_triangle_linear_reload(const APU_Triangle *t) {
    return t->reg0 & 0x7F;
}
static inline bool apu_noise_length_halt(const APU_Noise *n) {
    return (n->reg0 & 0x20) != 0;
}
static inline bool apu_noise_constant_volume(const APU_Noise *n) {
    return (n->reg0 & 0x10) != 0;
}
static inline int apu_noise_volume(const APU_Noise *n) {
    return n->reg0 & 0x0F;
}

/* ============================================================================
 * Pulse Period Update
 * ============================================================================ */

/* The sweep unit computes its target continuously, so the channel mutes
 * whenever an add would overflow $7FF, even with sweep disabled or a zero
 * shift. Only the reload period changes here: a running timer divider keeps
 * counting and picks up the new period at its next reload. */
static inline void apu_update_pulse_period(APU_Pulse *p) {
    int period = p->timer_reload &= 0x7FF;
    p->sweep_mute = period < 8 ||
        (!p->sweep_negate && period + (period >> p->sweep_shift) > 0x7FF);
}

/* ============================================================================
 * Envelope Clocking
 * ============================================================================ */

static inline void apu_clock_pulse_envelope(APU_Pulse *p) {
    if (p->envelope_start) {
        p->envelope_start = false;
        p->envelope_decay = 15;
        p->envelope_divider = apu_pulse_volume(p);
    } else {
        if (p->envelope_divider == 0) {
            p->envelope_divider = apu_pulse_volume(p);
            if (p->envelope_decay > 0) {
                p->envelope_decay--;
            } else if (apu_pulse_length_halt(p)) {
                p->envelope_decay = 15;  /* Loop mode */
            }
        } else {
            p->envelope_divider--;
        }
    }
}

static inline void apu_clock_noise_envelope(APU_Noise *n) {
    if (n->envelope_start) {
        n->envelope_start = false;
        n->envelope_decay = 15;
        n->envelope_divider = apu_noise_volume(n);
    } else {
        if (n->envelope_divider == 0) {
            n->envelope_divider = apu_noise_volume(n);
            if (n->envelope_decay > 0) {
                n->envelope_decay--;
            } else if (apu_noise_length_halt(n)) {
                n->envelope_decay = 15;
            }
        } else {
            n->envelope_divider--;
        }
    }
}

/* ============================================================================
 * Linear Counter
 * ============================================================================ */

static inline void apu_clock_linear_counter(APU_Triangle *t) {
    if (t->linear_reload) {
        t->linear_counter = apu_triangle_linear_reload(t);
    } else if (t->linear_counter > 0) {
        t->linear_counter--;
    }
    if (!apu_triangle_control_flag(t)) {
        t->linear_reload = false;
    }
}

/* ============================================================================
 * Length Counter
 * ============================================================================ */

static inline void apu_clock_length(bool halt, int *length_counter) {
    if (!halt && *length_counter > 0) {
        (*length_counter)--;
    }
}

/* ============================================================================
 * Sweep Unit
 * ============================================================================ */

static inline void apu_clock_sweep(APU_Pulse *p, bool negate_correction) {
    if (!p->sweep_enable)
        return;

    if (p->sweep_divider == 0) {
        if (p->sweep_shift > 0 && !p->sweep_mute) {
            int change = p->timer_reload >> p->sweep_shift;
            if (p->sweep_negate) {
                change = -(change + (negate_correction ? 1 : 0));
            }
            int target = p->timer_reload + change;
            if (target >= 0 && target <= 0x7FF && p->timer_reload >= 8) {
                p->timer_reload = target;
                apu_update_pulse_period(p);
            }
        }
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = false;
    } else if (p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = false;
    } else {
        p->sweep_divider--;
    }
}

/* ============================================================================
 * Quarter Frame / Half Frame
 * ============================================================================ */

static inline void apu_clock_quarter_frame(APU *apu) {
    HOOK_APU_FRAME(0, true, false, apu->frame.irq_flag);
    apu_clock_pulse_envelope(&apu->pulse[0]);
    apu_clock_pulse_envelope(&apu->pulse[1]);
    apu_clock_noise_envelope(&apu->noise);
    apu_clock_linear_counter(&apu->triangle);
    apu->dirty = true;  /* envelopes and linear counter can change output */
}

static inline void apu_clock_half_frame(APU *apu) {
    apu->length_clocked = true;
    HOOK_APU_FRAME(0, false, true, apu->frame.irq_flag);
    apu_clock_length(apu->length_halt & 1,
                     &apu->pulse[0].length_counter);
    apu_clock_length(apu->length_halt & 2,
                     &apu->pulse[1].length_counter);
    apu_clock_length(apu->length_halt & 4,
                     &apu->triangle.length_counter);
    apu_clock_length(apu->length_halt & 8,
                     &apu->noise.length_counter);

    apu_clock_sweep(&apu->pulse[0], true);   /* Pulse 1: negate adds 1 */
    apu_clock_sweep(&apu->pulse[1], false);  /* Pulse 2: negate doesn't */
    apu->dirty = true;  /* length counters and sweep can change output */
}

static inline void apu_apply_length_writes(APU *apu) {
    if (!apu->length_writes) return;
    int *counters[] = {&apu->pulse[0].length_counter, &apu->pulse[1].length_counter,
                       &apu->triangle.length_counter, &apu->noise.length_counter};
    bool enabled[] = {apu->pulse[0].enabled, apu->pulse[1].enabled,
                      apu->triangle.enabled, apu->noise.enabled};
    for (int i=0; i<4; ++i) {
        if ((apu->length_writes & (1u<<i)) && enabled[i] &&
            (!apu->length_clocked || !apu->length_reload_old[i])) {
            *counters[i] = apu->length_reload[i];
            apu->dirty = true;
        }
    }
    apu->length_halt = (apu_pulse_length_halt(&apu->pulse[0]) ? 1 : 0) |
        (apu_pulse_length_halt(&apu->pulse[1]) ? 2 : 0) |
        (apu_triangle_control_flag(&apu->triangle) ? 4 : 0) |
        (apu_noise_length_halt(&apu->noise) ? 8 : 0);
    apu->length_writes = 0;
}

/* ============================================================================
 * Frame Counter
 * ============================================================================ */

static inline void apu_clock_frame_counter(APU *apu) {
    APU_FrameCounter *fc = &apu->frame;

    if (fc->pending_write) {
        fc->write_delay--;
        if (fc->write_delay > 0)
            return;
        fc->pending_write = false;
        fc->cycle = 0;
        fc->step = 0;
        return;
    }

    const int *steps = apu->pal
        ? (fc->five_step ? apu_frame_steps_mode1_pal : apu_frame_steps_mode0_pal)
        : (fc->five_step ? apu_frame_steps_mode1 : apu_frame_steps_mode0);

    if (fc->five_step) {
        if (fc->cycle == steps[0]) {
            apu_clock_quarter_frame(apu);
        } else if (fc->cycle == steps[1]) {
            apu_clock_quarter_frame(apu);
            apu_clock_half_frame(apu);
        } else if (fc->cycle == steps[2]) {
            apu_clock_quarter_frame(apu);
        } else if (fc->cycle == steps[4]) {
            apu_clock_quarter_frame(apu);
            apu_clock_half_frame(apu);
            fc->cycle = -2;
        }
    } else {
        if (fc->cycle == steps[0]) {
            apu_clock_quarter_frame(apu);
        } else if (fc->cycle == steps[1]) {
            apu_clock_quarter_frame(apu);
            apu_clock_half_frame(apu);
        } else if (fc->cycle == steps[2]) {
            apu_clock_quarter_frame(apu);
        } else if (fc->cycle == steps[3]) {
            /* AccuracyCoin Frame Counter IRQ tests J, K, L: the IRQ flag
             * IS set during these two cycles even when irq_inhibit is on.
             * The flag is just promptly cleared again on the third cycle
             * (steps[5]) if irq_inhibit is true. Mirrors C# Emulator.cs
             * APU_Status_FrameInterrupt = true on cycles 29828 and 29829
             * unconditionally, then = !inhibit on 29830. */
            fc->irq_flag = true;
        } else if (fc->cycle == steps[4]) {
            apu_clock_quarter_frame(apu);
            apu_clock_half_frame(apu);
            fc->irq_flag = true;
        } else if (fc->cycle == steps[5]) {
            /* On the third cycle, the inhibit flag finally takes effect:
             * the flag remains set if not inhibited, otherwise cleared. */
            fc->irq_flag = !fc->irq_inhibit;
            fc->cycle = -1;
        }
    }

    fc->cycle++;
}

/* ============================================================================
 * Channel Timer Clocking
 * ============================================================================
 *
 * Each channel's per-CPU-cycle timer clock returns true if the call
 * changed any state that would affect the mixer output (sequence step,
 * LFSR bit, DMC output level). The apu_step driver ORs these flags
 * together and only re-evaluates the nonlinear mixer when at least one
 * channel reports dirty — typical NES music produces ~50-100k state
 * changes per second, so ~95% of the 1.79M/sec CPU cycles can skip
 * the mixer entirely and reuse a cached sample. */

static inline bool apu_clock_pulse_timer(APU_Pulse *p) {
    if (!p->enabled || p->length_counter == 0 || p->timer_reload < 8)
        return false;
    if (--p->timer <= 0) {
        p->timer = (p->timer_reload + 1) * 2;
        p->sequence_step = (p->sequence_step + 1) & 7;
        return true;  /* duty waveform advanced → output may have flipped */
    }
    return false;
}

static inline bool apu_clock_triangle_timer(APU_Triangle *t) {
    if (--t->timer <= 0) {
        t->timer = t->timer_reload + 1;
        if (t->length_counter > 0 && t->linear_counter > 0) {
            t->sequence_step = (t->sequence_step + 1) & 31;
            return true;  /* Counters gate the sequencer, not its timer. */
        }
    }
    return false;
}

static inline bool apu_clock_noise_timer(APU_Noise *n, bool pal) {
    if (!n->enabled || n->length_counter == 0)
        return false;
    if (--n->timer <= 0) {
        n->timer = (pal ? apu_noise_period_table_pal : apu_noise_period_table)[n->reg2 & 0x0F];
        int feedback_bit = (n->reg2 & 0x80) ? ((n->lfsr >> 6) & 1)
                                              : ((n->lfsr >> 1) & 1);
        int bit0 = n->lfsr & 1;
        int feedback = (bit0 ^ feedback_bit) & 1;
        n->lfsr = (n->lfsr >> 1) | (feedback << 14);
        return true;  /* LFSR shifted */
    }
    return false;
}

static inline bool apu_clock_dmc_timer(APU *apu) {
    APU_DMC *d = &apu->dmc;
    bool dirty = false;

    if (--d->timer <= 0) {
        d->timer = d->timer_reload > 0 ? d->timer_reload : apu_dmc_rate_table[0];

        if (!d->silence_flag) {
            uint8_t old = d->output_level;
            if (d->shift_register & 1) {
                if (d->output_level <= 125)
                    d->output_level += 2;
            } else {
                if (d->output_level >= 2)
                    d->output_level -= 2;
            }
            if (d->output_level != old) dirty = true;
            d->shift_register >>= 1;
        }

        d->bits_remaining--;
        if (d->bits_remaining == 0) {
            d->bits_remaining = 8;
            if (d->sample_buffer_empty) {
                d->silence_flag = true;
            } else {
                d->silence_flag = false;
                d->shift_register = d->sample_buffer;
                d->sample_buffer_empty = true;
                if (apu->dmc_dma_delay < 1) apu->dmc_dma_delay = 1;
            }
        }
    }
    return dirty;
}

/* ============================================================================
 * Channel Output
 * ============================================================================ */

static inline int apu_pulse_output(const APU_Pulse *p) {
    if (!p->enabled || p->length_counter == 0 ||
        p->timer_reload < 8 || p->sweep_mute)
        return 0;
    int duty = apu_duty_table[apu_pulse_duty(p)][p->sequence_step];
    if (duty == 0) return 0;
    return apu_pulse_constant_volume(p) ? apu_pulse_volume(p) : p->envelope_decay;
}

static inline int apu_triangle_output(const APU_Triangle *t) {
    /* A stopped triangle holds its last DAC level; gating it to zero
     * adds a discontinuity every time the music engine silences it. */
    return apu_triangle_table[t->sequence_step];
}

static inline int apu_noise_output(const APU_Noise *n) {
    if (!n->enabled || n->length_counter == 0)
        return 0;
    int envelope = apu_noise_constant_volume(n) ? apu_noise_volume(n) : n->envelope_decay;
    return (n->lfsr & 1) ? 0 : envelope;
}

/* ============================================================================
 * Audio Filtering & Mixing
 * ============================================================================ */

/* Compute a highpass IIR alpha for a given corner frequency.
 * DF1 form: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
 * alpha = 1 / (1 + 2*pi*f_c/f_s)                                   */
static inline double apu_hp_alpha_from_corner(double corner_hz, double sample_rate) {
    return 1.0 / (1.0 + 2.0 * M_PI * corner_hz / sample_rate);
}

/* Compute a lowpass IIR alpha for a given corner frequency.
 * One-pole form: y[n] += alpha * (x[n] - y[n])
 * alpha = 1 - exp(-2*pi*f_c/f_s)                                   */
static inline double apu_lp_alpha_from_corner(double corner_hz, double sample_rate) {
    return 1.0 - exp(-2.0 * M_PI * corner_hz / sample_rate);
}

/* Fill an APUFilterConfig from three corner frequencies at the given
 * sample rate. Commit 7 presets use this to name filter profiles by
 * corner freqs ("90/440/14000") rather than raw alphas. */
static inline void apu_filter_config_from_corners(APUFilterConfig *fc,
                                                    double hp1_hz,
                                                    double hp2_hz,
                                                    double lp_hz,
                                                    double sample_rate) {
    fc->hp1_alpha = apu_hp_alpha_from_corner(hp1_hz, sample_rate);
    fc->hp2_alpha = apu_hp_alpha_from_corner(hp2_hz, sample_rate);
    fc->lp_alpha  = apu_lp_alpha_from_corner(lp_hz,  sample_rate);
}

static inline float apu_apply_filters(APU *apu, float sample) {
    const APUFilterConfig *fc = &apu->filter_config;

    /* Highpass 1: output coupling cap (very low corner, DC block) */
    double hp1_out = fc->hp1_alpha * (apu->hp_filter1 + sample - apu->hp_prev_input1);
    apu->hp_prev_input1 = sample;
    apu->hp_filter1 = hp1_out;

    /* Highpass 2: amp feedback network (bass shaping) */
    double hp2_out = fc->hp2_alpha * (apu->hp_filter2 + hp1_out - apu->hp_prev_input2);
    apu->hp_prev_input2 = hp1_out;
    apu->hp_filter2 = hp2_out;

    /* Lowpass: RC rolloff at the amp output */
    apu->lp_filter += fc->lp_alpha * (hp2_out - apu->lp_filter);

    return (float)apu->lp_filter;
}

/* ============================================================================
 * Analog character post-processing (commit 6)
 * ============================================================================
 * Applied AFTER the filter chain on every output sample. Simulates
 * the subtle analog artifacts of real NES hardware:
 *
 *   - soft-clip amp saturation (tanh drive)
 *   - 60 Hz mains hum bleeding through the PSU
 *   - white thermal noise floor from the amp
 *   - DMC bus level bleeding into the main mix ($4011 quirk)
 *   - output gain trim
 *
 * All fields default to neutral values in apu_init, so this stage
 * is a no-op until a preset or menu enables the character knobs. */
static inline float apu_apply_analog(APU *apu, float sample) {
    const APUAnalog *a = &apu->analog;
    float y = sample;

    /* DMC bus crosstalk: a small fraction of the current DMC output
     * level bleeds into the output as a slowly-varying offset, like
     * the Famicom $4011 bus where direct DMC writes pull the audio
     * bus rail. The DMC DAC is 7-bit (0..127); we map to ±0.01 at
     * max crosstalk. */
    if (a->dmc_bus_crosstalk > 0.0f) {
        float dmc_level = ((float)apu->dmc.output_level - 64.0f) * (1.0f / 64.0f);
        y += dmc_level * 0.01f * a->dmc_bus_crosstalk;
    }

    /* Saturation (tanh soft-clip). drive scales from 1.0 (neutral)
     * to 5.0 (heavy). Normalized by tanh(drive) so full-scale input
     * still maps to full-scale output — the curvature lives in the
     * mid-levels, giving a warm musical compression. */
    if (a->saturation > 0.0f) {
        float drive = 1.0f + a->saturation * 4.0f;
        float norm  = tanhf(drive);
        y = tanhf(y * drive) / norm;
    }

    /* 60 Hz mains hum. Phase accumulator advances by 2π·60/fs per
     * output sample, wrapping modulo 2π. */
    if (a->hum_60hz > 0.0f) {
        apu->hum_phase += 2.0 * M_PI * 60.0 / (double)APU_SAMPLE_RATE;
        if (apu->hum_phase >= 2.0 * M_PI) apu->hum_phase -= 2.0 * M_PI;
        y += (float)(a->hum_60hz * sin(apu->hum_phase));
    }

    /* White thermal noise from the amp stage. Xorshift32 is fast
     * and produces a sufficiently-random sequence for this purpose. */
    if (a->noise_floor > 0.0f) {
        uint32_t r = apu->noise_rng;
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        apu->noise_rng = r;
        /* Map to [-1, 1] */
        float n = ((float)(int32_t)r) * (1.0f / 2147483648.0f);
        y += n * a->noise_floor;
    }

    /* Final output gain trim. */
    if (a->output_gain != 1.0f) {
        y *= a->output_gain;
    }

    return y;
}

/* ============================================================================
 * Resampling filter design (Kaiser-windowed sinc)
 * ============================================================================ */

/* Modified Bessel function of the first kind, order 0.
 * Power series converges quickly for the beta values we use (< 12). */
static inline double apu_bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    double xsq_quarter = (x * x) * 0.25;
    for (int k = 1; k < 50; k++) {
        term *= xsq_quarter / ((double)k * (double)k);
        sum += term;
        if (term < 1e-14 * sum) break;
    }
    return sum;
}

/* Design a Kaiser-windowed sinc low-pass and store in `taps`.
 * `cutoff` is the normalized cutoff (cycles/sample, 0..0.5).
 * `beta` controls the Kaiser window shape; 8.5 gives ~80 dB stopband.
 * Taps are normalized to unit DC gain. */
static inline void apu_design_kaiser_sinc(float *taps, int n,
                                           double cutoff, double beta) {
    int half = n / 2;
    double i0_beta = apu_bessel_i0(beta);
    double sum = 0.0;
    for (int k = 0; k < n; k++) {
        int m = k - half;
        /* Ideal sinc lowpass (2 * cutoff at DC, falls off as sin(x)/x) */
        double sinc_val;
        if (m == 0) {
            sinc_val = 2.0 * cutoff;
        } else {
            double arg = 2.0 * M_PI * cutoff * (double)m;
            sinc_val = sin(arg) / (M_PI * (double)m);
        }
        /* Kaiser window */
        double t = (double)m / (double)half;  /* -1 .. +1 */
        double arg2 = 1.0 - t * t;
        if (arg2 < 0.0) arg2 = 0.0;
        double window = apu_bessel_i0(beta * sqrt(arg2)) / i0_beta;
        double tap = sinc_val * window;
        taps[k] = (float)tap;
        sum += tap;
    }
    /* Normalize to unit DC gain */
    float inv_sum = (float)(1.0 / sum);
    for (int k = 0; k < n; k++) taps[k] *= inv_sum;
}

/* Precompute the pulse and TND DAC voltage lookup tables from the
 * NesDev linear approximation, optionally warping each entry by a
 * cubic nonlinearity to approximate the real 2A03 DAC's voltage
 * curve (which isn't perfectly linear across the 16/128-level range).
 *
 *   pulse_dac[n]  = 95.88  / (8128  / n + 100)  for n = 1..30
 *   tnd_dac[n]    = 163.67 / (24329 / n + 100)  for n = 1..202
 *
 * With dac_nonlinearity = k, each entry becomes
 *     y' = y + k * (y^3 - y)
 * which is a mild S-curve: small values get slightly smaller and
 * large values get slightly larger, matching the measured hardware's
 * slightly expansive response at high levels. k=0 ⇒ linear (nesdev
 * baseline), k=1 ⇒ full cubic curvature (character preset).
 *
 * Zero-input entries are explicit zeros (formula divides by zero). */
static inline void apu_build_dac_tables(APU *apu) {
    float k = apu->analog.dac_nonlinearity;
    apu->pulse_dac[0] = 0.0f;
    for (int n = 1; n < 31; n++) {
        double y = 95.88 / (8128.0 / (double)n + 100.0);
        double curved = y + (double)k * (y * y * y - y);
        apu->pulse_dac[n] = (float)curved;
    }
    apu->tnd_dac[0] = 0.0f;
    for (int n = 1; n < 203; n++) {
        double y = 163.67 / (24329.0 / (double)n + 100.0);
        double curved = y + (double)k * (y * y * y - y);
        apu->tnd_dac[n] = (float)curved;
    }
}

/* Compute the raw DAC output (unfiltered) via lookup-table mixer.
 * Called once per CPU cycle as part of the resampling loop; NOT
 * followed by the IIR filter chain — that runs at the lower output
 * rate inside apu_apply_filters. Lookup-based mixer is strictly
 * faster than the division formula and mathematically equivalent
 * (both derived from the NesDev linear approximation). */
static inline double apu_mix_sample_raw(APU *apu) {
    int p1 = apu_pulse_output(&apu->pulse[0]);
    int p2 = apu_pulse_output(&apu->pulse[1]);
    int tri = apu_triangle_output(&apu->triangle);
    int noi = apu_noise_output(&apu->noise);
    int dmc_out = apu->dmc.output_level;

    int pulse_idx = p1 + p2;                  /* 0..30 */
    int tnd_idx = 3 * tri + 2 * noi + dmc_out; /* 0..202 */

    /* These should be in range by construction but clamp defensively
     * against DMC DAC overflow or channel-disable transitions. */
    if (pulse_idx < 0)   pulse_idx = 0;
    if (pulse_idx > 30)  pulse_idx = 30;
    if (tnd_idx   < 0)   tnd_idx   = 0;
    if (tnd_idx   > 202) tnd_idx   = 202;

    return (double)apu->pulse_dac[pulse_idx] + (double)apu->tnd_dac[tnd_idx];
}

/* Legacy wrapper: raw mix + IIR filter chain. Still exposed for any
 * non-step caller (e.g., unit tests) that wants a one-shot sample. */
static inline float apu_mix_sample(APU *apu) {
    return apu_apply_filters(apu, (float)apu_mix_sample_raw(apu));
}

/* ============================================================================
 * DMC Sample Fetching (for DMA integration)
 * ============================================================================ */

/* The stop signal can race the output-unit reload request. A stop in the
 * preceding APU cycle leaves a one-cycle RDY pulse; a request already
 * reaching the reader completes its fetch. */
static inline void apu_dmc_stop(APU *apu, bool explicit_stop) {
    APU_DMC *d = &apu->dmc;
    if (d->bits_remaining == 1 && d->timer >= 1 && d->timer <= 2)
        apu->dmc_abort_delay = (uint8_t)(d->timer + 1);
    if (explicit_stop && d->sample_buffer_empty && apu->dmc_dma_delay <= 1)
        apu->dmc_late_request = true;
}

static inline bool apu_dmc_needs_sample(APU *apu) {
    /* Gate by dmc_dma_delay so the DMA doesn't fire on the very next read
     * after $4015 enables the DMC. Real hardware: ~2 APU cycles delay
     * between the write that enables DMC and the actual halt. */
    return apu->dmc.sample_buffer_empty &&
           (apu->dmc.bytes_remaining > 0 || apu->dmc_late_request) &&
           apu->dmc_dma_delay == 0;
}

static inline void apu_dmc_load_sample(APU *apu, uint8_t sample) {
    bool had_remaining = apu->dmc.bytes_remaining > 0;
    apu->dmc.sample_buffer = sample;
    apu->dmc.sample_buffer_empty = false;
    apu->dmc.current_address = (apu->dmc.current_address + 1) | 0x8000;
    if (apu->dmc.current_address == 0)
        apu->dmc.current_address = 0x8000;
    apu->dmc_late_request = false;
    if (had_remaining) apu->dmc.bytes_remaining--;

    if (had_remaining && apu->dmc.bytes_remaining == 0) {
        if (apu->dmc.loop_flag) {
            apu->dmc.current_address = apu->dmc.sample_address;
            apu->dmc.bytes_remaining = apu->dmc.sample_length;
        } else {
            apu_dmc_stop(apu, false);
            if (apu->dmc.irq_enabled) apu->dmc.irq_flag = true;
        }
    }

    /* Sync legacy fields */
    apu->dmc_current_addr = apu->dmc.current_address;
    apu->dmc_bytes_remaining = apu->dmc.bytes_remaining;
    apu->dmc_sample_buffer_empty = apu->dmc.sample_buffer_empty;
}

/* ============================================================================
 * APU Step (one CPU cycle)
 * ============================================================================ */

static inline void apu_step(APU *apu) {
    /* Toggle put/get cycle parity. Each apu_step is one CPU cycle, and
     * the APU alternates between "put" and "get" cycles. Mirrors C#
     * Emulator.cs APU_PutCycle = !APU_PutCycle pattern. */
    apu->put_cycle = !apu->put_cycle;

    /* Apply pending frame IRQ clear (deferred from a $4015 read on a
     * put cycle). The clear is delayed by frame_irq_clear_delay apu cycles
     * so that the apu_step which would otherwise read the still-set flag
     * (via cpu_step right after this apu_step) finds the flag still set. */
    if (apu->frame_irq_clear_pending) {
        if (apu->frame_irq_clear_delay > 0) {
            apu->frame_irq_clear_delay--;
        } else {
            apu->frame.irq_flag = false;
            apu->frame_irq_pending = false;
            apu->frame_irq_clear_pending = false;
        }
    }

    /* Decrement DMC DMA delay so the DMA can fire after the gate elapses.
     * Without this delay the DMA would halt on the very next CPU read
     * (e.g. the LDA opcode fetch instead of the actual $2002 read). */
    if (apu->dmc_dma_delay > 0) {
        apu->dmc_dma_delay--;
    }

    if (apu->dmc_abort_delay && --apu->dmc_abort_delay == 0)
        apu->dmc_abort_pending = true;

    apu->length_clocked = false;
    apu_clock_frame_counter(apu);  /* may set apu->dirty via quarter/half frame */
    apu_apply_length_writes(apu);

    /* Clock all channels. Each timer returns true if its state change
     * would affect the mixer output (sequence step, LFSR shift, DMC
     * output level change). */
    bool step_dirty = apu->dirty;
    step_dirty |= apu_clock_pulse_timer(&apu->pulse[0]);
    step_dirty |= apu_clock_pulse_timer(&apu->pulse[1]);
    step_dirty |= apu_clock_triangle_timer(&apu->triangle);
    step_dirty |= apu_clock_noise_timer(&apu->noise, apu->pal);

    step_dirty |= apu_clock_dmc_timer(apu);

    /* Only re-evaluate the mixer when at least one channel has
     * reported a state change. Otherwise reuse the cached value —
     * on typical NES content 90-95% of CPU cycles see no change. */
    if (step_dirty) {
        apu->last_raw_sample = (float)apu_mix_sample_raw(apu);
        apu->dirty = false;
    }

    /* Push the current (possibly cached) raw sample into the resample
     * ring every CPU cycle. Writing the cached value is still needed
     * so the FIR has a full history even across cached-output runs. */
    apu->resample_ring[apu->resample_write & (APU_RESAMPLE_RING - 1)] =
        apu->last_raw_sample;
    apu->resample_write++;

    /* When the fractional accumulator crosses the CPU clock, emit one
     * output sample by convolving the Kaiser-windowed sinc FIR over
     * the most recent APU_RESAMPLE_TAPS samples in the ring. Proper
     * band-limited resampling — no aliasing fold-down from step-wave
     * harmonics, no pitch jitter from point sampling. Then run the
     * analog filter chain and the analog character post-processing.
     *
     * The FIR is symmetric so we pair-sum (ring[base-k] + ring[base-(N-1-k)])
     * and multiply by taps[k], halving the multiplies from 129 to ~65. */
    apu->sample_accumulator += apu->sample_rate;
    if (apu->sample_accumulator >= apu->cpu_clock) {
        apu->sample_accumulator -= apu->cpu_clock;
        if (apu->audio_callback) {
            const int    N    = APU_RESAMPLE_TAPS;
            const int    half = N / 2;
            const unsigned mask = APU_RESAMPLE_RING - 1;
            const int base = apu->resample_write - 1;
            const float * __restrict taps = apu->resample_taps;
            const float * __restrict ring = apu->resample_ring;

            /* Center tap stands alone for an odd-length symmetric FIR. */
            double sum = (double)taps[half] *
                         (double)ring[(base - half) & mask];
            /* Pair-sum the remaining taps around the center. */
            for (int k = 0; k < half; k++) {
                double pair = (double)ring[(base - k) & mask] +
                              (double)ring[(base - (N - 1 - k)) & mask];
                sum += (double)taps[k] * pair;
            }

            float filtered = apu_apply_filters(apu, (float)sum);
            float final    = apu_apply_analog(apu, filtered);
            apu->audio_callback(apu->audio_user_data, final);
        }
    }

    /* Sync IRQ flags for NES system integration. The frame IRQ flag is
     * briefly set even when inhibited (AccuracyCoin Frame Counter IRQ
     * tests J, K, L), so the irq_flag itself can be set with inhibit on.
     * But the IRQ line should NOT assert in that case — gate by inhibit.
     * Mirrors C# Emulator.cs IRQ_LevelDetector |= !APU_FrameCounterInhibitIRQ. */
    apu->frame_irq_pending = apu->frame.irq_flag && !apu->frame.irq_inhibit;
    apu->dmc_irq_pending = apu->dmc.irq_flag;
    apu->dmc_current_addr = apu->dmc.current_address;
    apu->dmc_bytes_remaining = apu->dmc.bytes_remaining;
    apu->dmc_sample_buffer_empty = apu->dmc.sample_buffer_empty;
}

/* ============================================================================
 * APU Status Read ($4015)
 * ============================================================================ */

static inline uint8_t apu_read_status(APU *apu) {
    uint8_t status = 0;

    if (apu->pulse[0].length_counter > 0) status |= 0x01;
    if (apu->pulse[1].length_counter > 0) status |= 0x02;
    if (apu->triangle.length_counter > 0) status |= 0x04;
    if (apu->noise.length_counter > 0)    status |= 0x08;
    if (apu->dmc.bytes_remaining > 0)     status |= 0x10;
    if (apu->frame.irq_flag)              status |= 0x40;
    if (apu->dmc.irq_flag)                status |= 0x80;

    /* Reading $4015 schedules the frame IRQ flag clear on the NEXT APU
     * "get" cycle (when put_cycle would be false). The cycle the read
     * happens on still observes the flag set; the clear lands at the
     * start of the next get cycle so the read after that sees zero.
     *
     * AccuracyCoin Frame Counter IRQ tests 6 and 7:
     *   - Read on put cycle (put_cycle == true): next get cycle is 1
     *     CPU cycle later. The read returns the still-set flag; the
     *     deferred clear runs at the very next apu_step. delay = 0 so
     *     the next apu_step sees pending && delay==0 and clears.
     *   - Read on get cycle (put_cycle == false): next get cycle is 2
     *     CPU cycles later (skip the intervening put cycle). delay = 1
     *     so the next apu_step decrements to 0 without clearing, and
     *     the apu_step after that runs the clear.
     *
     * This matches the rule from AccuracyCoin: "you probably want to
     * clear bit 6 inside the APU cycle code of your emulator... a flag
     * for 'we are clearing bit 6 on the next APU get cycle' to be set
     * inside the 'read $4015' code."
     */
    if (!apu->frame_irq_clear_pending) {
        apu->frame_irq_clear_pending = true;
        apu->frame_irq_clear_delay = apu->put_cycle ? 0 : 1;
    }

    return status;
}

/* ============================================================================
 * APU Register Writes
 * ============================================================================ */

static inline void apu_write(APU *apu, uint16_t addr, uint8_t val) {
    HOOK_APU_REG(addr, val);
    uint16_t reg = addr & 0x1F;
    if (reg < 0x18)
        apu->regs[reg] = val;

    /* Any register write can change mixer output (enable flags,
     * duty cycles, volume, length load, DMC direct level, etc.).
     * Mark dirty so the next apu_step re-evaluates the mixer. */
    apu->dirty = true;

    switch (addr) {
    /* Pulse 1: $4000-$4003 */
    case 0x4000:
        apu->pulse[0].reg[0] = val;
        apu->length_writes |= 16;
        break;
    case 0x4001:
        apu->pulse[0].reg[1] = val;
        apu->pulse[0].sweep_enable = (val & 0x80) != 0;
        apu->pulse[0].sweep_period = (val >> 4) & 0x07;
        apu->pulse[0].sweep_negate = (val & 0x08) != 0;
        apu->pulse[0].sweep_shift = val & 0x07;
        apu->pulse[0].sweep_reload = true;
        apu_update_pulse_period(&apu->pulse[0]);
        break;
    case 0x4002:
        apu->pulse[0].reg[2] = val;
        apu->pulse[0].timer_reload = (apu->pulse[0].timer_reload & 0x700) | val;
        apu_update_pulse_period(&apu->pulse[0]);
        break;
    case 0x4003:
        apu->pulse[0].reg[3] = val;
        apu->pulse[0].timer_reload = ((val & 0x07) << 8) | apu->pulse[0].reg[2];
        apu_update_pulse_period(&apu->pulse[0]);
        apu->pulse[0].sequence_step = 0;
        apu->pulse[0].envelope_start = true;
        if (apu->pulse[0].enabled) {
            apu->length_reload[0] = apu_length_table[val >> 3];
            apu->length_reload_old[0] = apu->pulse[0].length_counter;
            apu->length_writes |= 1;
        }
        break;

    /* Pulse 2: $4004-$4007 */
    case 0x4004:
        apu->pulse[1].reg[0] = val;
        apu->length_writes |= 16;
        break;
    case 0x4005:
        apu->pulse[1].reg[1] = val;
        apu->pulse[1].sweep_enable = (val & 0x80) != 0;
        apu->pulse[1].sweep_period = (val >> 4) & 0x07;
        apu->pulse[1].sweep_negate = (val & 0x08) != 0;
        apu->pulse[1].sweep_shift = val & 0x07;
        apu->pulse[1].sweep_reload = true;
        apu_update_pulse_period(&apu->pulse[1]);
        break;
    case 0x4006:
        apu->pulse[1].reg[2] = val;
        apu->pulse[1].timer_reload = (apu->pulse[1].timer_reload & 0x700) | val;
        apu_update_pulse_period(&apu->pulse[1]);
        break;
    case 0x4007:
        apu->pulse[1].reg[3] = val;
        apu->pulse[1].timer_reload = ((val & 0x07) << 8) | apu->pulse[1].reg[2];
        apu_update_pulse_period(&apu->pulse[1]);
        apu->pulse[1].sequence_step = 0;
        apu->pulse[1].envelope_start = true;
        if (apu->pulse[1].enabled) {
            apu->length_reload[1] = apu_length_table[val >> 3];
            apu->length_reload_old[1] = apu->pulse[1].length_counter;
            apu->length_writes |= 2;
        }
        break;

    /* Triangle: $4008-$400B */
    case 0x4008:
        apu->triangle.reg0 = val;
        apu->length_writes |= 16;
        break;
    case 0x4009:
        /* Unused */
        break;
    case 0x400A:
        apu->triangle.reg2 = val;
        apu->triangle.timer_reload = (apu->triangle.timer_reload & 0x700) | val;
        break;
    case 0x400B:
        apu->triangle.reg3 = val;
        apu->triangle.timer_reload = ((val & 0x07) << 8) | apu->triangle.reg2;
        if (apu->triangle.enabled) {
            apu->length_reload[2] = apu_length_table[val >> 3];
            apu->length_reload_old[2] = apu->triangle.length_counter;
            apu->length_writes |= 4;
        }
        /* $400B reloads length/linear counters, not waveform phase or timer. */
        apu->triangle.linear_reload = true;
        break;

    /* Noise: $400C-$400F */
    case 0x400C:
        apu->noise.reg0 = val;
        apu->length_writes |= 16;
        break;
    case 0x400D:
        /* Unused */
        break;
    case 0x400E:
        /* The new period takes effect at the timer's next reload. */
        apu->noise.reg2 = val;
        break;
    case 0x400F:
        apu->noise.reg3 = val;
        apu->noise.envelope_start = true;
        if (apu->noise.enabled) {
            apu->length_reload[3] = apu_length_table[val >> 3];
            apu->length_reload_old[3] = apu->noise.length_counter;
            apu->length_writes |= 8;
        }
        break;

    /* DMC: $4010-$4013 */
    case 0x4010:
        apu->dmc.reg[0] = val;
        apu->dmc.irq_enabled = (val & 0x80) != 0;
        apu->dmc.loop_flag = (val & 0x40) != 0;
        apu->dmc.timer_reload = (apu->pal ? apu_dmc_rate_table_pal : apu_dmc_rate_table)[val & 0x0F];
        if (!apu->dmc.irq_enabled)
            apu->dmc.irq_flag = false;
        break;
    case 0x4011:
        apu->dmc.reg[1] = val;
        apu->dmc.output_level = val & 0x7F;
        break;
    case 0x4012:
        apu->dmc.reg[2] = val;
        apu->dmc.sample_address = 0xC000 + ((uint16_t)val << 6);
        break;
    case 0x4013:
        apu->dmc.reg[3] = val;
        apu->dmc.sample_length = ((uint16_t)val << 4) + 1;
        break;

    /* Status: $4015 */
    case 0x4015:
        apu->pulse[0].enabled = (val & 0x01) != 0;
        if (!apu->pulse[0].enabled)
            apu->pulse[0].length_counter = 0;

        apu->pulse[1].enabled = (val & 0x02) != 0;
        if (!apu->pulse[1].enabled)
            apu->pulse[1].length_counter = 0;

        apu->triangle.enabled = (val & 0x04) != 0;
        if (!apu->triangle.enabled)
            apu->triangle.length_counter = 0;

        apu->noise.enabled = (val & 0x08) != 0;
        if (!apu->noise.enabled)
            apu->noise.length_counter = 0;

        {
            apu->dmc.enabled = (val & 0x10) != 0;
            if (!apu->dmc.enabled) {
                if (apu->dmc.bytes_remaining > 0) apu_dmc_stop(apu, true);
                apu->dmc.bytes_remaining = 0;
            } else {
                if (apu->dmc.bytes_remaining == 0) {
                    apu->dmc.current_address = apu->dmc.sample_address;
                    apu->dmc.bytes_remaining = apu->dmc.sample_length;
                    /* Real hardware: writing to $4015 to enable the DMC
                     * with no current sample takes ~2 APU cycles before
                     * the DMA actually fires. Without this delay the DMA
                     * would halt on the next CPU read instead of after
                     * a couple of cycles, breaking DMC DMA + $2002/$4015
                     * tests where the dummy reads must land on the
                     * register-access cycle. */
                    /* Enable reaches the reader on the second following get. */
                    apu->dmc_dma_delay = apu->put_cycle ? 3 : 4;
                }
            }
            apu->dmc.irq_flag = false;
        }
        break;

    /* Frame counter: $4017 */
    case 0x4017:
        apu->frame.five_step = (val & 0x80) != 0;
        apu->frame.irq_inhibit = (val & 0x40) != 0;
        if (apu->frame.irq_inhibit)
            apu->frame.irq_flag = false;
        /* Mode 1: immediately clock quarter/half frame */
        if (apu->frame.five_step) {
            apu_clock_quarter_frame(apu);
            apu_clock_half_frame(apu);
        }
        apu->frame.pending_write = true;
        /* AccuracyCoin Frame Counter IRQ tests B and C: the frame counter
         * reset delay depends on which APU cycle the $4017 write lands on.
         * Write on a put cycle (put_cycle == true at time of write) → 3
         * cycle delay. Write on a get cycle → 4 cycle delay. The cycle
         * here is "after this apu_step's toggle but before the cpu_step
         * runs the write", so put_cycle reflects this CPU cycle. */
        apu->frame.write_delay = apu->put_cycle ? 3 : 4;
        break;
    }
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

static inline void apu_init(APU *apu) {
    memset(apu, 0, sizeof(APU));
    apu->sample_rate = APU_SAMPLE_RATE;
    apu->cpu_clock = APU_CPU_CLOCK_NTSC;
    apu->noise.lfsr = 1;
    apu->dmc.sample_buffer_empty = true;
    apu->dmc.silence_flag = true;
    apu->dmc.bits_remaining = 8;
    apu->dmc.timer = apu_dmc_rate_table[0];
    apu->dmc.timer_reload = apu_dmc_rate_table[0];
    apu->frame.five_step = true;  /* 5-step mode (no IRQ) at power-on */
    apu->frame.irq_inhibit = true;
    apu->put_cycle = true;  /* So first apu_step toggles it false (= get cycle) */

    /* Design the resampling FIR: Kaiser-windowed sinc low-pass.
     * Cutoff is set just below 22 kHz in cycles/input-sample units
     * (22000/1789773 ≈ 0.0123). Beta 8.5 gives ~80 dB stopband with
     * a transition band narrow enough to preserve audible content
     * while aggressively rejecting the aliasing that step waveforms
     * (pulse, noise, triangle, DMC) would otherwise fold into the
     * audible band. */
    apu_design_kaiser_sinc(apu->resample_taps, APU_RESAMPLE_TAPS,
                            0.0120, 8.5);

    /* Build the pulse and TND DAC voltage lookup tables. */
    apu_build_dac_tables(apu);

    /* Default analog filter coefficients. These match the hardcoded
     * values the code used before commit 5 so existing sound is
     * preserved. Presets (commit 7) will offer alternatives matching
     * different hardware revisions (Famicom / front/top-loader NES / Dendy). */
    apu->filter_config.hp1_alpha = 0.996863;  /* ~22 Hz HP (output coupling) */
    apu->filter_config.hp2_alpha = 0.937419;  /* ~440 Hz HP (amp feedback) */
    apu->filter_config.lp_alpha  = 0.815687;  /* ~12 kHz LP (RC rolloff) */

    /* Default analog character: all neutral (no audible change). */
    apu->analog.filter            = apu->filter_config;
    apu->analog.dac_nonlinearity  = 0.0f;
    apu->analog.saturation        = 0.0f;
    apu->analog.noise_floor       = 0.0f;
    apu->analog.hum_60hz          = 0.0f;
    apu->analog.dmc_bus_crosstalk = 0.0f;
    apu->analog.output_gain       = 1.0f;
    apu->noise_rng                = 0x12345678u;   /* non-zero xorshift seed */
    apu->hum_phase                = 0.0;

    /* Change-detection state: force first mixer call to compute
     * a fresh value rather than using the all-zero cache. */
    apu->dirty           = true;
    apu->last_raw_sample = 0.0f;
}

static inline void apu_reset(APU *apu) {
    uint8_t saved_tri_reg0 = apu->triangle.reg0;
    apu_audio_callback_t saved_cb = apu->audio_callback;
    void *saved_ud = apu->audio_user_data;
    int saved_rate = apu->sample_rate;
    int saved_clock = apu->cpu_clock;
    bool saved_pal = apu->pal;
    /* Filter and analog character are user settings, not console state. */
    APUFilterConfig saved_filter = apu->filter_config;
    APUAnalog saved_analog = apu->analog;

    memset(apu, 0, sizeof(APU));
    apu->sample_rate = saved_rate ? saved_rate : APU_SAMPLE_RATE;
    apu->cpu_clock = saved_clock ? saved_clock : APU_CPU_CLOCK_NTSC;
    apu->pal = saved_pal;
    apu->noise.lfsr = 1;
    apu->dmc.sample_buffer_empty = true;
    apu->dmc.silence_flag = true;
    apu->dmc.bits_remaining = 8;
    apu->dmc.timer = apu->pal ? apu_dmc_rate_table_pal[0] : apu_dmc_rate_table[0];
    apu->dmc.timer_reload = apu->dmc.timer;

    /* Re-design the resampling FIR and DAC tables (memset wiped them). */
    apu_design_kaiser_sinc(apu->resample_taps, APU_RESAMPLE_TAPS,
                            0.0120, 8.5);
    apu->filter_config = saved_filter;
    apu->analog = saved_analog;
    apu->noise_rng                = 0x12345678u;
    apu->hum_phase                = 0.0;
    apu->dirty                    = true;
    apu->last_raw_sample          = 0.0f;
    apu_build_dac_tables(apu);   /* needs analog.dac_nonlinearity */

    /* Restore triangle control flag (unaffected by reset) */
    apu->triangle.reg0 = saved_tri_reg0;
    apu->length_halt = (saved_tri_reg0 & 0x80) ? 4 : 0;

    /* Restore audio callback */
    apu->audio_callback = saved_cb;
    apu->audio_user_data = saved_ud;
}

/* Set audio callback */
static inline void apu_set_audio_callback(APU *apu, apu_audio_callback_t cb, void *user_data) {
    apu->audio_callback = cb;
    apu->audio_user_data = user_data;
}

/* Set 2A03/2A07 timing; preserve the fractional output sample position. */
static inline void apu_set_region(APU *apu, int region) {
    int clock = region == 1 ? APU_CPU_CLOCK_PAL : APU_CPU_CLOCK_NTSC;
    if (apu->cpu_clock > 0)
        apu->sample_accumulator = (int)((int64_t)apu->sample_accumulator * clock / apu->cpu_clock);
    apu->cpu_clock = clock;
    apu->pal = region == 1;
    apu->dmc.timer_reload = (apu->pal ? apu_dmc_rate_table_pal : apu_dmc_rate_table)[apu->dmc.reg[0] & 15];
}

#endif /* NES_APU_H */
