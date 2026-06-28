# APU Architecture

## Overview

The APU (`src/nes/apu.h`) emulates the NES 2A03 audio processing unit. It
produces mixed audio from five channels: two pulse wave generators, a triangle
wave, a noise generator, and a delta modulation channel (DMC). Output passes
through a three-stage filter chain before reaching the audio callback.

## Channels

### Pulse 1 and Pulse 2 ($4000-$4007)

Square wave generators with configurable duty cycle, volume envelope,
sweep unit, and length counter.

- **Duty cycles**: 12.5%, 25%, 50%, 75% (inverted 25%)
- **Timer range**: 8-2047 (periods below 8 are muted)
- **Sweep**: Shifts the timer period up or down each half-frame. Pulse 1
  uses one's complement negation; Pulse 2 uses two's complement.

```c
// Sweep difference: Pulse 1 adds extra correction
apu_clock_sweep(&apu->pulse[0], true);   // negate_correction = true
apu_clock_sweep(&apu->pulse[1], false);  // negate_correction = false
```

### Triangle ($4008-$400B)

Produces a 4-bit triangle wave (values 0-15-0) using a 32-step sequence.
Controlled by a linear counter (quarter-frame) and length counter (half-frame).
Has no volume control -- output is either on or off.

### Noise ($400C-$400F)

Pseudo-random noise from a 15-bit linear feedback shift register (LFSR).
Two modes: normal (taps bits 0 and 1) and short loop (taps bits 0 and 6).
Has volume envelope and length counter.

### DMC ($4010-$4013)

Plays 1-bit delta-encoded samples from ROM. Reads sample bytes via DMA
(stealing 3-4 CPU cycles per fetch). Can trigger IRQ when the sample
finishes.

Key fields:
- `sample_address` / `sample_length`: configured by registers
- `sample_buffer`: byte fetched by DMA
- `shift_register`: 8-bit shift register output (delta-decoded)
- `output_level`: 7-bit DAC level (0-127)

## Frame Counter

The frame counter ($4017) clocks envelopes, length counters, sweep units,
and linear counters at specific cycle counts:

### 4-Step Mode (mode bit = 0)

| Step | CPU Cycle | Quarter | Half | IRQ |
|------|-----------|---------|------|-----|
| 1 | 7456 | Yes | No | No |
| 2 | 14912 | Yes | Yes | No |
| 3 | 22370 | Yes | No | No |
| 4 | 29827 | No | No | Yes |
| 5 | 29828 | Yes | Yes | Yes |
| 6 | 29829 | No | No | Yes (reset) |

### 5-Step Mode (mode bit = 1)

| Step | CPU Cycle | Quarter | Half | IRQ |
|------|-----------|---------|------|-----|
| 1 | 7456 | Yes | No | No |
| 2 | 14912 | Yes | Yes | No |
| 3 | 22370 | Yes | No | No |
| 4 | 37280 | Yes | Yes | No |

5-step mode never generates IRQ. Writing to $4017 with a pending write
delay resets the frame counter after 3-4 CPU cycles.

## Mixing Formula

The NES uses a nonlinear mixing formula (from the nesdev wiki):

```c
// Pulse channels (lookup approximation)
pulse_out = 95.88 / (8128.0 / (pulse1 + pulse2) + 100.0);

// Triangle, noise, and DMC
tnd_input = tri/8227.0 + noise/12241.0 + dmc/22638.0;
tnd_out = 159.79 / (1.0/tnd_input + 100.0);

raw_sample = pulse_out + tnd_out;
```

## Filter Chain

The raw mixed output passes through three filters simulating the NES
analog output path:

1. **High-pass 90 Hz** -- removes DC offset
2. **High-pass 440 Hz** -- shapes bass response
3. **Low-pass 14 kHz** -- removes aliasing/high-frequency noise

```c
#define HP_ALPHA_90HZ   0.996863
#define HP_ALPHA_440HZ  0.937419
#define LP_ALPHA_14KHZ  0.815687
```

These coefficients are pre-computed for a 44.1 kHz sample rate. The filters
are implemented as single-pole IIR filters:

```c
// Highpass: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
// Lowpass:  y[n] += alpha * (x[n] - y[n])
```

## Sample Generation

The APU generates one audio sample every `CPU_CLOCK / SAMPLE_RATE` CPU
cycles (~40.6 cycles at 44.1 kHz NTSC). When a sample is ready, it calls
the audio callback:

```c
if (apu->audio_callback) {
    float sample = apu_mix_sample(apu);
    apu->audio_callback(apu->audio_user_data, sample);
}
```

## DMC DMA Integration

When the DMC sample buffer empties and bytes remain, `apu_dmc_needs_sample()`
returns true. The NES system (`nes.h`) responds by halting the CPU and
performing a DMA read:

```c
if (apu_dmc_needs_sample(nes->apu)) {
    nes->cpu->rdy = false;
    // ... 3-4 cycle DMA sequence ...
    uint8_t sample = nes_cpu_read(nes->cpu, nes->apu->dmc_current_addr);
    apu_dmc_load_sample(nes->apu, sample);
}
```

The DMC address wraps within $8000-$FFFF (bit 15 always set).

## Clock Rates

| Parameter | NTSC | PAL |
|-----------|------|-----|
| CPU clock | 1,789,773 Hz | 1,662,607 Hz |
| Sample rate | 44,100 Hz | 44,100 Hz |
| Cycles per sample | ~40.6 | ~37.7 |

## Related Files

- `src/nes/apu.h` -- complete APU implementation
- `src/nes/nes.h` -- DMC DMA, APU IRQ propagation
- `src/nes/hooks.h` -- `on_apu_frame` and `on_apu_reg` hooks
- `src/nes/trace.c` -- APU trace handlers
