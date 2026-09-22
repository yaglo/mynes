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
Has no volume control. Stopping the counters holds the last DAC value;
it does not force the output to zero.

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

### NTSC 4-Step Mode (mode bit = 0)

| Step | CPU Cycle | Quarter | Half | IRQ |
|------|-----------|---------|------|-----|
| 1 | 7456 | Yes | No | No |
| 2 | 14912 | Yes | Yes | No |
| 3 | 22370 | Yes | No | No |
| 4 | 29827 | No | No | Yes |
| 5 | 29828 | Yes | Yes | Yes |
| 6 | 29829 | No | No | Yes (reset) |

### NTSC 5-Step Mode (mode bit = 1)

| Step | CPU Cycle | Quarter | Half | IRQ |
|------|-----------|---------|------|-----|
| 1 | 7456 | Yes | No | No |
| 2 | 14912 | Yes | Yes | No |
| 3 | 22370 | Yes | No | No |
| 4 | 37280 | Yes | Yes | No |

5-step mode never generates IRQ. Writing to $4017 with a pending write
delay resets the frame counter after 3-4 CPU cycles.

PAL uses separate sequencer positions: 8312, 16626, 24938, 33251,
33252 and 33253 in four-step mode; the final quarter/half clock in five-step
mode is 41564. These are internal counter positions, not elapsed-cycle
counts from an arbitrary `$4017` write. PAL also selects regional noise
and DMC period tables.

Length halt/reload writes take effect after the coincident half-frame clock.
A reload coinciding with a length clock is accepted when the old length was
zero and suppressed when it was nonzero. Volume/loop writes do not restart
envelopes. Triangle high-period writes preserve timer and sequencer phase;
stopping its length/linear counter holds the DAC output instead of forcing
zero. Direct APU checks, AccuracyCoin and all ten bundled hardware-tested
PAL APU ROMs cover these paths.

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

The core has a configurable single-pole high-pass → high-pass → low-pass
cascade, followed by optional analog character. Its legacy defaults are
`hp1_alpha = 0.996863`, `hp2_alpha = 0.937419`, `lp_alpha = 0.815687` at
44.1 kHz. These constants are not the previously documented 90 Hz / 440 Hz /
14 kHz triplet. `apu_filter_config_from_corners()` computes coefficients
from explicit corner frequencies and sample rate when a profile supplies them.

```c
// High-pass: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
// Low-pass:  y[n] += alpha * (x[n] - y[n])
```

The GPU frontend sets this core cascade to passthrough and applies its separate
console/cable/amplifier/speaker chain to the resampled DAC output. Its CPU
fallback uses the same audio-chain parameters. This avoids applying the console
response twice.

## Sample Generation

The nonlinear mixer produces a raw DAC value at CPU rate. A 129-tap
Kaiser-windowed sinc FIR reads a 512-sample ring when the fractional accumulator
reaches the next output sample. The accumulator uses the regional CPU clock and
configured sample rate; at 44.1 kHz the average spacing is about 40.6 CPU cycles
for NTSC and 37.7 for PAL. This is filtered decimation, not a point sample of the
current channel levels. The filter/analog stages then feed `audio_callback`.

## DMC DMA Integration

`apu_dmc_needs_sample(&nes->apu)` requests a refill. `nes_dma_step()` arbitrates
it against CPU writes and OAM DMA, halts the CPU on a readable cycle, and
performs the DMC read on a get cycle after the halt/dummy phases. The fetched
byte is passed to `apu_dmc_load_sample(&nes->apu, sample)`.

The DMC address wraps within $8000–$FFFF. OAM overlap, aborted requests and
controller/PPU register read side effects are handled in the shared DMA path;
a bare read followed by a fixed CPU delay would not describe the implementation.

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
