# APU Authenticity Plan

Status: **not started.** Review and diagnosis complete; implementation
scheduled after NTSC is stable.

Philosophy: **match the composite video pipeline.** The NTSC work built a
physics-based signal generator + windowed-sinc filter chain + analog-path
model + tunable parameter struct + user presets. The APU gets the same
treatment — signal from first principles, proper band-limited resampling,
measured hardware filter coefficients, tunable analog knobs, and presets.

## Context

User-facing complaints:

1. **"Drifting"** — pitch wobbles audibly, especially on sustained notes
2. **"Some sounds are just bad"** — noise and high-pitch pulse channels
   sound harsh, squelchy, enharmonic
3. **"Doesn't feel analog at all"** — even when the notes are in tune and
   aliasing is acceptable, the output reads as "sterile digital NES" rather
   than "NES plugged into a CRT TV"

Each of these has a specific, identifiable root cause in the signal path,
and each has a known fix documented in the authoritative emulator literature
(nesdev wiki, Bisqwit articles, Shay Green's blip_buf work).

## What already works

The **cycle-accurate NES logic** is solid. Don't rewrite these:

| Component | File / line | Status |
|---|---|---|
| Pulse 1 / Pulse 2 (timer, duty, envelope, sweep, length) | `apu.h:71-92, 292-367, 499-506` | ✓ correct |
| Triangle (linear counter, 32-step sequencer, length) | `apu.h:94-106` | ✓ correct |
| Noise (LFSR both modes, envelope, period table) | `apu.h:108-122, 456-460, 514-519` | ✓ correct |
| DMC (sample buffer, delta decode, output level, loop, IRQ) | `apu.h:124-147, 464-492` | ✓ correct |
| DMC DMA (3-4 cycle CPU stall, address wrap, bus activity) | `nes.h:456-487` | ✓ correct |
| Frame counter modes 0 / 1 (cycle timing, IRQ, quarter/half) | `apu.h:149-158, 64-65, 426` | ✓ correct |
| $4017 write delay (3-4 CPU cycles) | `apu.h:828` | ✓ correct |
| Nonlinear mixer formula (exact NesDev) | `apu.h:547-568` | ✓ correct |
| Three-stage filter chain (90/440/14 kHz IIR) | `apu.h:530-545` | ✓ present, but textbook coefficients |
| Audio ring buffer, SDL callback, jitter absorption | `main.c:46-110` | ✓ works, but rate-adjust causes warble |
| NTSC / PAL region timing | `apu.h:883-885` | ✓ basic infrastructure |

**~900 lines of direct C in `src/nes/apu.h`.** The existing code is a good
foundation — we're going to replace the signal generation and output path
on top of it, not rewrite the channel state machines.

## Root-cause diagnosis

### 1. Drift — accumulator resampling + proportional rate-adjust

Current code (`apu.h:618-625`):
```c
apu->sample_accumulator += apu->sample_rate;   // +44100 per CPU cycle
if (apu->sample_accumulator >= apu->cpu_clock) {
    apu->sample_accumulator -= apu->cpu_clock;
    emit_one_sample();
}
```

At 1.79 MHz / 44.1 kHz, samples come out at ~40, ~41, ~40, ~41 CPU cycles
apart. Jitter ≈ 0.25 CPU cycles = **22 µs at 44.1 kHz**. Nearest-neighbor
point sampling of a step waveform with sub-sample jitter pitch-modulates
the tone.

Compounding it, the ring buffer fill controller (`main.c:1520-1530`)
adjusts `apu.sample_rate` ±2% proportionally to the fill error. Under
varying render load the rate swings back and forth — and since we're
controlling the **sample period**, not the playback rate, the swings
manifest as literal pitch warble over seconds-long time windows.

### 2. Bad sounds — no band-limited synthesis

The APU generates raw step waveforms: pulse duty transitions, LFSR noise
bit flips, triangle 32-step transitions, DMC delta steps. Every step is
a discontinuity in the time domain, which in the frequency domain means
spectral content up to Nyquist and beyond at the CPU clock rate.

Our resampler grabs point values with no anti-aliasing. Everything above
22 kHz in the input spectrum folds back into the audible band. You
cannot filter out aliasing after the fact — the 14 kHz LPF removes
high-frequency content *above* 14 kHz, but by then the aliased energy
below 14 kHz is already there, indistinguishable from intended harmonics.

Symptoms:
- **Pulse** at high frequencies: enharmonic whistles, not clean squares
- **Noise**: constant subcarrier-frequency buzz from folded LFSR harmonics
- **Triangle** at high frequencies: should be near-sine, sounds crunchy
- **DMC**: harsh on top of the real delta compression

Every serious emulator fixes this with **band-limited synthesis**: either
Shay Green's `blip_buf` library (per-transition band-limited impulses)
or with **oversampling + windowed-sinc downsample** (generate at high
rate, filter properly to target rate).

### 3. No analog feel — no analog-path model

Mathematically correct mixer and filters, but the pipeline stops there.
Missing real-hardware character:

- **Nonlinear DAC voltages**: the 2A03 pulse DAC outputs 16 discrete
  voltages that aren't evenly spaced (measured hardware data from NesDev).
  We treat them as linear 0-15 integers passed into the nonlinear mixer
  approximation. Closer to truth: use the measured voltage table directly.
- **Thermal noise floor**: real hardware has a measurable analog noise
  floor from the output amp. Our output is bit-exact silent between notes.
- **Output coupling capacitor drift**: NES has a DC-blocking cap that
  creates a sub-Hz pole and slight DC wandering.
- **Amplifier saturation**: when all 5 channels peak, real hardware
  compresses gently. Our mixer just sums per the nonlinear formula
  with no amp stage.
- **Cartridge / $4011 bus quirk**: Famicom DMC reads pulse residual
  levels into the audio bus (games used this for digitized samples).
  Missing entirely.

## Video → audio technique mapping

| NTSC composite | APU analog |
|---|---|
| Bisqwit 2C02 voltage-level signal table | 2A03 DAC nonlinear voltage curves (measured) |
| 8 samples per pixel internal buffer | Generate **one sample per CPU cycle** (1.79 MHz) |
| Hamming-windowed sinc Y/C FIR | Kaiser-windowed sinc 1.79 MHz → 44.1 kHz downsample |
| Adaptive comb filter | (not needed for audio) |
| Chroma bleeding (wide C FIR) | Gentle audio-band LPF matching measured hardware |
| CRT Gaussian beam profile | DAC quantization + nonlinear voltage steps |
| Afterglow / ringing / bloom | Output coupling cap + amp saturation |
| Vignette / corner darkening | Thermal noise floor |
| `NTSCComposite` tunable struct | `APUAnalog` tunable struct |
| Presets (Clean / PVM / Consumer / Bad / RF) | Presets (Famicom / NES / Mono TV / Broken speaker / Cartridge audio) |

## Implementation plan

Seven commits, each small and independently reversible. Each commit leaves
audio in a WORKING state — no "everything's broken until commit 7" pitfall.

### Commit 1: high-rate internal buffer + simple downsample (fixes drift)

**File**: `src/nes/apu.h`

Replace the accumulator resampler with an internal high-rate buffer.
Every CPU cycle, compute the mixer output once and store it in
`raw_stream[]` (sized ~2048 samples, roughly 1.1 ms at 1.79 MHz).

When `raw_stream[]` fills up, run a **proper decimator**: for this first
commit, a cheap block average with linear interpolation at the fractional
boundary — similar to the NTSC pipeline's `ntsc_downsample()`. Emit the
resulting 44.1 kHz samples via the existing callback.

This eliminates the accumulator jitter. Aliasing is still present (block
average has poor stopband) but drift goes away immediately.

**Verification**: Run a sustained SMB overworld theme. Drift should stop
being audible on long notes. No new artifacts introduced.

### Commit 2: windowed-sinc downsampler (fixes aliasing)

**File**: `src/nes/apu.h`

Replace the block-average decimator from Commit 1 with a proper
windowed-sinc polyphase resampler:

- Kaiser window, 65 taps, cutoff at 22 kHz (below Nyquist at 44.1 kHz)
- Ratio 40.585:1 is non-integer; use fractional phase tracking (similar
  to the NTSC `ntsc_downsample()` block average's fractional endpoints
  but with sinc weights instead of uniform weights)
- Precompute the window at init; reuse the tap-design helper from
  `ntsc_composite.h` if possible (move it to a shared header)

**Verification**: SMB1 title-screen pulse sweep should sound clean;
noise channel should stop buzzing on top of the intended hiss. Run
blargg `apu_test` ROMs to ensure accuracy regression is zero (the
per-channel state machines aren't touched).

### Commit 3: frame pacing cleanup (kills warble)

**File**: `frontends/sdl/main.c`

The proportional rate-adjust at line ~1520 causes audible pitch warble
from rapid sample-rate swings. Replace with:

- **Integral controller** with a long time constant (~10 seconds)
- **Dead band** around the target fill level so small errors don't
  trigger corrections
- **Smaller maximum adjust** (±0.5% instead of ±2%)
- **Optional: variable audio block push rate** — instead of adjusting
  sample rate, adjust how often we *call* the SDL callback

The NES's inherent 60.0988 Hz vs display 60 Hz mismatch is only 0.16%,
so ±0.5% leaves plenty of absorption headroom for normal operation.

**Verification**: Long sustained notes in Metroid or Castlevania 2
should have rock-solid pitch over minutes of play.

### Commit 4: 2A03 DAC voltage tables (replaces linear 0-15)

**File**: `src/nes/apu.h`

Add two precomputed lookup tables:
- `apu_pulse_dac[16]` — measured voltages for each pulse level
- `apu_tnd_dac[204]` — measured voltages for each valid
  triangle×3 + noise×2 + dmc combined level

Source: NesDev wiki "APU Mixer" page, "Linear approximation" section
lists exact-to-hardware values. Bisqwit has similar data.

The mixer then becomes a simple LOOKUP instead of the nonlinear formula:
```c
float out = apu_pulse_dac[p1 + p2] + apu_tnd_dac[3*tri + 2*noi + dmc];
```

This is strictly more accurate than the formula. The formula is an
analytic fit to the lookup, so output is very close but with subtle
character differences — especially on loud chords where the formula
over-smooths.

**Verification**: Spectrum analysis of a mixer test ROM should show
slightly different harmonic content matching hardware captures.

### Commit 5: measured analog filter chain (replaces textbook alphas)

**File**: `src/nes/apu.h`

Current filter alphas:
```c
#define HP_ALPHA_90HZ   0.996863
#define HP_ALPHA_440HZ  0.937419
#define LP_ALPHA_14KHZ  0.815687
```

These are textbook first-order IIR coefficients at 44.1 kHz for ideal
corner frequencies. Real NES hardware has been measured and has
different characteristics:

- The **14 kHz LPF** is actually closer to **8–10 kHz** in measured units
  (varies by NES model; Famicom is slightly different from front-loader
  NES from top-loader)
- The **440 Hz HPF** is real and measurable
- The **90 Hz HPF** is the output coupling cap; long time constant
  matters for DC drift modeling

Replace with multiple coefficient sets matching:
- **Famicom** (original Japanese 2A03)
- **NES front-loader** (RP2A03, US/EU)
- **NES top-loader** (later revisions)
- **Dendy / clones** (different filter networks, often harsher)

Each is ~5 coefficients in an `APUFilterConfig` struct.

**Verification**: Listen to dialog / sample-playback-heavy games
(Metal Storm, Vice: Project Doom, Battletoads) — should sound
noticeably less brittle with the lower LPF cutoff.

### Commit 6: `APUAnalog` tunable struct + analog character

**File**: `src/nes/apu.h`

Add the "analog personality" layer — the audio equivalent of NTSCComposite's
afterglow/ringing/bloom/vignette post-processing:

```c
typedef struct {
    APUFilterConfig filter;   /* from commit 5 */
    float dac_nonlinearity;   /* 0=ideal linear, 1=measured 2A03 */
    float saturation;         /* 0=none, 1=heavy soft-clip at peaks */
    float noise_floor_db;     /* -120 to -60 dB */
    float coupling_tau;       /* output cap time constant, seconds */
    float hum_50hz_db;        /* -120 to -40, simulates mains hum */
    float hum_60hz_db;
    float dmc_bus_crosstalk;  /* 0=none, 1=strong $4011 bus bleed */
    float output_gain;        /* final stage */
} APUAnalog;
```

Each field is a scalar applied in a fixed order in the post-filter
pipeline. Defaults produce current behavior (no personality), so this
commit is additive.

**Verification**: All defaults = current sound. Non-default values
produce audible character changes.

### Commit 7: presets + menu wiring

**Files**: `src/nes/apu.h`, `frontends/sdl/main.c`

Mirror the NTSC presets approach — a `APUPreset` struct holding a full
`APUAnalog` config, a static array of presets, and menu wiring in the
existing OSD under a new **Audio** submenu.

**Presets**:
- **Famicom (2A03)** — original Japanese filter, slight warmth, DMC bus
  crosstalk at ~0.3 (some games used this)
- **NES (RP2A03)** — front-loader US/EU, tighter filter, no bus crosstalk
- **Mono TV** — heavy LPF (3 kHz), compressed dynamics, hum at -60 dB,
  simulates the TV speaker the game was actually played through
- **Broken speaker** — extreme LPF (1.5 kHz), 6 dB of saturation, -40 dB
  noise, heavy coupling-cap drift
- **Cartridge audio** — DMC bus crosstalk at max, audible extra noise
  from Famicom cartridge slot

The menu item `Audio → Preset` → submenu with the 5 choices, just like
the video Presets submenu. Also expose individual knobs under
`Audio → Filter`, `Audio → Character`, etc.

**Verification**: Switching between Famicom ↔ NES ↔ Mono TV should be
audibly distinct. The video PVM/Consumer/Bad presets have a natural
pairing with audio presets, e.g., "PVM + NES" for pristine broadcast
reference, "Consumer TV + Mono TV" for 1987 living room feel.

## Verification / testing

**Automated**:
- **Blargg APU tests** (`apu_test.nes`, `apu_reset.nes`, `dmc_dma_during_read4.nes`):
  must pass or stay at the same pass state across all commits. These test
  cycle-accurate timing of the per-channel state machines — we're not
  touching that code, so they should not regress.
- **Frequency response test ROM**: play a 1 kHz square, FFT the output,
  check harmonic content matches real hardware within a few dB.
- **Drift test**: hold a sustained note for 60 seconds, record the output,
  look at the pitch over time. Should be within ±5 cents (inaudible drift)
  by commit 3.

**Listening tests** (do these per commit):
- **SMB1 overworld theme** — sustained pulse notes, high-frequency content
- **Castlevania 3 Beginning** — dense 4-channel interplay, subtle envelopes
- **Megaman 2 Dr. Wily** — fast arpeggios, triangle bass
- **Metal Storm title** — DMC-heavy, checks sample playback quality
- **Battletoads** — DMC bus crosstalk quirks (Famicom-specific bugs some
  games relied on)

**Reference recordings**: NESdev hardware captures on the wiki / Bisqwit's
analyses. Compare spectrograms side-by-side.

## Deferred / explicitly out of scope

- **PAL-specific audio timing**: the APU has basic NTSC/PAL CPU clock
  switching already; PAL frame counter timing details (different cycle
  counts per quarter/half frame) can be fixed in a follow-up if it
  matters for PAL games. Not in this plan.
- **Expansion audio** (MMC5 PCM, VRC6, VRC7, FDS, Namco 163, Sunsoft 5B):
  separate rabbit hole. Each expansion chip needs its own channel state
  machines. Completely independent from this plan.
- **Surround / stereo**: NES is mono. Famicom has L/R channel assignments
  for some MMC5 / VRC7 setups but the base 2A03 is mono only. Keep mono.
- **Frequency domain debugging UI**: a real-time FFT overlay in the OSD
  would be cool for tuning but is big scope. Later.
- **MIDI / piano-roll export**: nope.

## Critical files

- **`src/nes/apu.h`** — primary surface. Channel logic stays; signal
  generation, mixer, filter chain, and output path get replaced. New:
  high-rate buffer, windowed-sinc resampler, voltage tables, APUAnalog
  struct, preset data. Probably 400-600 lines of added/changed code.
- **`frontends/sdl/main.c`** — new `Audio` submenu under the existing
  OSD, rate-adjust controller rewrite. ~100 lines.
- **`docs/architecture/apu.md`** — update to reflect the new pipeline
  stages once the implementation lands.
- **`src/nes/nes.h`** — untouched. DMC DMA logic stays.

## References

- **NesDev wiki: APU Mixer** — nonlinear formula, voltage tables, filter
  description. Primary source for DAC data.
- **NesDev wiki: APU** — per-channel register semantics. Already followed.
- **Shay Green's `blip_buf`** — band-limited synthesis, alternative to
  oversampling. We're going oversampling per the video philosophy but
  blip_buf is worth reading for background.
- **Bisqwit's NES APU analysis** — audio signal chain deep-dive with
  hardware measurements.
- **Real hardware captures** — search NesDev forums for NES 2A03
  recording projects; useful for spectrum comparison.

## Estimated size

- `apu.h`: +400 to +600 LOC of new code (~200 replaced)
- `main.c`: +80 to +120 LOC for menu + rate controller rewrite
- **Total**: ~600-800 LOC across the two files
- **Estimated effort**: 2-3 focused days

## Execution order sanity check

Commit 1 → measurable drift fix (user-visible: sustained notes stop wobbling)
Commit 2 → measurable aliasing fix (user-visible: noise + high pulse sound clean)
Commit 3 → measurable warble fix (user-visible: rock-solid pitch over minutes)
Commit 4 → subtle tonal shift (user-visible: slightly more saturated mixer character)
Commit 5 → measurable tonal shift (user-visible: warmer, less brittle highs)
Commit 6 → additive infrastructure (no user-visible change at defaults)
Commit 7 → user-tunable presets (switch-and-hear comparison)

Each commit delivers its own audible improvement AND leaves the pipeline
working. Safe to stop after any commit; safe to revert any commit.
