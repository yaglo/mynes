# Shader Subsystems Proposal: Physically-Accurate Audio & Video

Status: **proposal / design document**

## Vision

Model the complete analog signal path — from silicon to screen, from DAC
to speaker — as chains of discrete physical components. Every capacitor,
resistor, cable, and amplifier is an individual compute kernel stage.
Audio and video share the same kernel primitives (RC filters, FIR
convolutions, nonlinear transfer functions) because physics doesn't care
what kind of signal is passing through.

The goal is not "looks/sounds approximately like a CRT" but "simulates
the actual circuit with real component values, and the CRT look/sound
emerges from that."

---

## Table of Contents

1. [Current Architecture](#1-current-architecture)
2. [The Shared Kernel Primitive Library](#2-the-shared-kernel-primitive-library)
3. [Video Signal Chain — Complete Physical Model](#3-video-signal-chain)
4. [Audio Signal Chain — Complete Physical Model](#4-audio-signal-chain)
5. [Transmission Medium Simulation](#5-transmission-medium-simulation)
6. [Metal Compute Dispatch Architecture](#6-metal-compute-dispatch-architecture)
7. [Buffer Layout & Memory Architecture](#7-buffer-layout--memory-architecture)
8. [Preset System — Component Chain Configurations](#8-preset-system)
9. [Parameter Binding & Swift Integration](#9-parameter-binding--swift-integration)
10. [Performance Budget](#10-performance-budget)
11. [Implementation Roadmap](#11-implementation-roadmap)
12. [References](#12-references)

---

## 1. Current Architecture

### What exists today

```
VIDEO (hybrid CPU + GPU):
  CPU:  2C02 waveform table → comb filter → FIR Y/I/Q demod → YIQ→RGB matrix
        → beam profile → persistence → bloom → barrel distortion
  GPU:  RF modulation (raw mode) → halation H/V blur → CRT phosphor render

AUDIO (CPU only):
  CPU:  Channel state machines → nonlinear DAC LUT → Kaiser-sinc resampler
        → HP1 (90 Hz) → HP2 (440 Hz) → LP (14 kHz) → analog character layer
        → float callback → AVAudioEngine ring buffer
```

### Key files

| Component | Path | Lines |
|---|---|---|
| NTSC composite pipeline | `src/nes/composite.h` | ~2574 |
| PAL-CRT (vendored LMP88959) | `src/nes/palcrt/` | ~1200 |
| APU core + analog | `src/nes/apu.h` | ~1376 |
| Metal shaders | `frontends/swiftui/.../Shaders.metal` | ~1232 |
| Metal renderer | `frontends/swiftui/.../MetalEmulatorView.swift` | ~1466 |
| C bridge | `frontends/swiftui/.../nes_bridge.c` | ~411 |
| Emulator params + presets | `frontends/swiftui/.../EmulatorCore.swift` | ~1100 |
| SDL frontend | `frontends/sdl/main.c` | ~2000 |

### What's missing

The current pipeline jumps from "signal generated" directly to "signal
decoded" with nothing in between. Real signals travel through:

- Cables with impedance, capacitance, and resistance
- Connectors with contact resistance and crosstalk
- RF modulators and demodulators with bandwidth limitations
- Antenna paths with multipath reflection and interference
- Power supplies that inject hum and noise into every stage
- Amplifiers that clip, ring, and have finite bandwidth

The space between **encode** and **decode** is where most of the
character of real hardware lives. Two identical NES consoles sound and
look different because of their cables, their TVs, and the 60 Hz hum
on their power rails.

---

## 2. The Shared Kernel Primitive Library

Every analog component in both the audio and video paths reduces to one
of six kernel types. These are the building blocks — implemented once in
Metal, reused everywhere.

### 2.1 Point-wise transfer function

```
y[n] = f(x[n])
```

Each sample is independent. Trivially parallel — one thread per sample.

**Used for:** DAC voltage curves, gamma/degamma, transistor
saturation (tanh soft-clip), diode clipping, color matrix multiply,
nonlinear mixer LUTs.

**Metal kernel pattern:**
```metal
kernel void pointwise_transfer(
    device float* input  [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant TransferParams& p [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid >= p.count) return;
    float x = input[tid];
    // Example: cubic DAC nonlinearity
    output[tid] = x + p.k * (x * x * x - x);
}
```

### 2.2 First-order IIR (RC filter) via parallel prefix scan

```
y[n] = a · y[n-1] + b · x[n]
```

This is **the** critical primitive. Every capacitor in the system is
this operation. It looks sequential, but it's a linear recurrence —
solvable with **parallel prefix scan** in O(log N) steps.

**The math:** expand the recurrence:
```
y[n] = b·x[n] + a·b·x[n-1] + a²·b·x[n-2] + a³·b·x[n-3] + ...
```

This is an associative reduction. The Blelloch scan algorithm computes
it in 2·log₂(N) passes over a workgroup, with each pass halving the
dependency distance.

For a 1024-sample block: ~20 steps instead of 1024 sequential steps.

**Mapping R and C values to coefficients:**
```
For RC low-pass:   a = exp(-dt / (R·C)),  b = 1 - a
For RC high-pass:  a = 1 / (1 + 2π·fc·dt),  b = a
    where fc = 1 / (2π·R·C),  dt = 1 / sample_rate
```

**Used for:** Every coupling capacitor (DC blocking), every RC low-pass
on the motherboard, every RC high-pass in the amplifier feedback network,
cable capacitance rolloff, speaker crossover filters.

**Metal kernel pattern (simplified):**
```metal
// Phase 1: Up-sweep (reduce)
// Phase 2: Down-sweep (propagate)
// Each thread handles one element; workgroup size = block size
// For details see Blelloch 1990 or GPU Gems 3 Ch. 39

kernel void rc_filter_prefix_scan(
    device float* data     [[buffer(0)]],
    device float* state    [[buffer(1)]],  // carry from previous block
    constant RCParams& p   [[buffer(2)]],
    uint tid  [[thread_position_in_grid]],
    uint lid  [[thread_position_in_threadgroup]],
    uint gid  [[threadgroup_position_in_grid]])
{
    threadgroup float shared[BLOCK_SIZE];
    // Load: transform input to (a, b·x) pairs
    // Scan: associative combine (a1·a2, a1·b2 + b1)
    // Store: extract y values from scanned pairs
    // Carry: last element's state feeds next block
}
```

### 2.3 FIR convolution (finite impulse response)

```
y[n] = Σ h[k] · x[n-k]  for k = 0..N-1
```

Each output sample is an independent dot product with the coefficient
array. Parallel across output samples; the dot product itself can be
partially parallelized within a threadgroup using shared memory.

**Used for:** Bandwidth limiting (Y channel, chroma channel), anti-alias
decimation (1.79 MHz → 48 kHz), demodulation carrier multiply + filter,
cable frequency response shaping.

**Metal kernel pattern:**
```metal
kernel void fir_convolve(
    device const float* input   [[buffer(0)]],
    device float*       output  [[buffer(1)]],
    constant float*     taps    [[buffer(2)]],
    constant FIRParams& p       [[buffer(3)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid >= p.output_count) return;
    float sum = 0.0;
    uint base = tid * p.decimation_ratio;
    for (uint k = 0; k < p.tap_count; k++) {
        sum += taps[k] * input[base + k];
    }
    output[tid] = sum;
}
```

### 2.4 Delay line

```
y[n] = x[n - D]
```

Pure offset read. Trivially parallel.

**Used for:** Comb filter (1H delay = 1 scanline), multipath ghosting
(delayed copy at reduced amplitude), echo/reflection in cables.

Can be combined with scaling for ghost signals:
```
y[n] = x[n] + ghost_level · x[n - D]
```

### 2.5 Modulator / demodulator (carrier multiply)

```
y[n] = x[n] · cos(2π · f_carrier · n / f_sample + φ)
```

Point-wise multiply with a generated carrier. Trivially parallel.

**Used for:** NTSC chroma modulation (3.58 MHz), NTSC chroma
demodulation (synchronous detection for I and Q), RF modulation
(AM envelope onto carrier), RF demodulation (envelope detection).

### 2.6 Two-dimensional convolution (separable)

```
Pass 1 (horizontal): temp[x,y] = Σ h[k] · src[x+k, y]
Pass 2 (vertical):   out[x,y]  = Σ h[k] · temp[x, y+k]
```

Two dispatch passes, each parallel across all pixels. A 2D Gaussian
with radius R costs 2R multiplies per pixel instead of R² — the
separability gives ~20× speedup at typical radii.

**Used for:** Halation (light scattering through CRT glass), bloom
(phosphor glow), defocus simulation.

### Primitive summary

| # | Kernel | Parallelism | GPU Strategy | Audio | Video |
|---|--------|-------------|-------------|-------|-------|
| 1 | Point-wise | Per-sample | 1 thread/sample | DAC, clip, gain | Gamma, matrix, LUT |
| 2 | RC (IIR-1) | Per-block | Prefix scan | Every cap | Every cap |
| 3 | FIR | Per-output | Dot product | Resample, BW limit | Demod, BW limit |
| 4 | Delay | Per-sample | Offset read | — | Comb, ghost |
| 5 | Mod/demod | Per-sample | Carrier × | — | Chroma, RF |
| 6 | 2D convolve | Per-pixel | Separable | — | Halation, bloom |

---

## 3. Video Signal Chain

The complete physical model from PPU output to photons hitting the
viewer's eye. Each numbered box is a separate compute dispatch.

### 3.1 Signal domain (encode → transmission → decode)

```
PPU OUTPUT (256×240 palette indices + emphasis, from CPU)
│
▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 1: 2C02 VIDEO DAC                                 │
│  ─────────────────────────                               │
│  Kernel type: point-wise (LUT)                           │
│                                                          │
│  The PPU doesn't output RGB. It generates a composite    │
│  waveform directly — a sequence of voltage levels that   │
│  encode luma and chroma into the NTSC baseband.          │
│                                                          │
│  Input:  palette index (6 bit) + emphasis (3 bit) = 9b   │
│          + dot phase within color burst cycle (0-11)     │
│  Output: one float per PPU dot (composite voltage)       │
│          ~2048 samples per scanline × 240 lines          │
│                                                          │
│  Implementation: precomputed 512×12 waveform table       │
│  (Bisqwit's 2C02 model). Each palette+emphasis combo     │
│  maps to 12 voltage samples spanning one chroma cycle.   │
│                                                          │
│  NOTE: This stage is currently CPU-side and may stay     │
│  there for cycle-accuracy reasons. The waveform buffer   │
│  is uploaded to GPU for all subsequent stages.           │
│                                                          │
│  Component values: N/A (digital-to-analog, discrete      │
│  voltage levels from silicon)                            │
└──────────────────────┬───────────────────────────────────┘
                       │
                       │  composite waveform buffer
                       │  (~2048 × 240 × float)
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 2: CONSOLE OUTPUT STAGE                           │
│  ─────────────────────────────                           │
│  Kernel types: RC low-pass (prefix scan) + point-wise    │
│                                                          │
│  The composite signal passes through the console's       │
│  internal circuitry before reaching the output jack.     │
│                                                          │
│  Components:                                             │
│  • Output coupling capacitor (AC coupling)               │
│    RC high-pass, fc ≈ 1-5 Hz                             │
│    Blocks DC offset from the DAC                         │
│    R = 75 Ω (source impedance), C = 220 µF typical      │
│                                                          │
│  • Output buffer amplifier                               │
│    Bandwidth limit ≈ 6 MHz (RC low-pass)                 │
│    Slew rate limiting on fast transitions                │
│    Slight nonlinearity at signal peaks (soft clip)       │
│                                                          │
│  • Power supply ripple injection                         │
│    60 Hz (NTSC) / 50 Hz (PAL) sinusoidal hum            │
│    Amplitude: 0-20 mV peak on 1V p-p signal             │
│    Modulates both luma and chroma equally                │
│                                                          │
│  Output: conditioned composite signal, AC-coupled        │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 3: CABLE / TRANSMISSION MEDIUM                    │
│  ────────────────────────────────────                    │
│  Kernel types: RC low-pass + delay + point-wise          │
│                                                          │
│  *** This is the critical missing stage. ***             │
│  Different cable types have dramatically different       │
│  characteristics. The cable IS a distributed RC          │
│  network — literally a chain of capacitors.              │
│                                                          │
│  SUB-STAGES (active set depends on connection type):     │
│                                                          │
│  3a. COMPOSITE CABLE (RCA)                               │
│      ┌─────────────────────────────────────────┐         │
│      │ • Series resistance: 0.1-2.0 Ω/meter   │         │
│      │   (connector contact adds 0.5-5 Ω)     │         │
│      │ • Shunt capacitance: 50-100 pF/meter    │         │
│      │   → RC low-pass, fc depends on length   │         │
│      │   6 ft cable: fc ≈ 20 MHz (minimal)     │         │
│      │   25 ft cable: fc ≈ 5 MHz (visible)     │         │
│      │ • Characteristic impedance: 75 Ω        │         │
│      │   Mismatch → reflections (ghosting)     │         │
│      │   Ghost delay: 2 × cable length / c     │         │
│      │   Ghost amplitude: reflection coeff     │         │
│      │ • Shield effectiveness: 60-90 dB        │         │
│      │   Imperfect → RF interference pickup    │         │
│      └─────────────────────────────────────────┘         │
│                                                          │
│  3b. RF COAXIAL (antenna/cable)                          │
│      ┌─────────────────────────────────────────┐         │
│      │ All of 3a, PLUS:                        │         │
│      │ • RF modulator (console side)           │         │
│      │   Carrier: Ch 3 = 61.25 MHz             │         │
│      │            Ch 4 = 67.25 MHz             │         │
│      │   AM modulation of composite onto       │         │
│      │   carrier with vestigial sideband       │         │
│      │   Bandwidth: ±3 MHz (lossy)             │         │
│      │ • Antenna/cable noise floor             │         │
│      │   Thermal noise: -70 to -50 dBm         │         │
│      │   Interference: other channels,         │         │
│      │   harmonics, ignition noise, etc.       │         │
│      │ • RF demodulator (TV tuner side)        │         │
│      │   Envelope detection or synchronous     │         │
│      │   AGC (automatic gain control) with     │         │
│      │   attack/release time constants         │         │
│      │   Additional bandwidth limiting         │         │
│      └─────────────────────────────────────────┘         │
│                                                          │
│  3c. S-VIDEO CABLE (Y/C separate)                        │
│      ┌─────────────────────────────────────────┐         │
│      │ • Two independent signal paths:         │         │
│      │   Y (luma): 75 Ω, same cable model      │         │
│      │   C (chroma): 75 Ω, same cable model    │         │
│      │ • NO composite encode/decode artifacts  │         │
│      │   (no dot crawl, no cross-color)        │         │
│      │ • Cable capacitance still affects both   │         │
│      │   channels independently                │         │
│      │ • Chroma bandwidth preserved (~1.5 MHz)  │         │
│      └─────────────────────────────────────────┘         │
│                                                          │
│  3d. COMPONENT VIDEO (Y/Pb/Pr separate)                  │
│      ┌─────────────────────────────────────────┐         │
│      │ • Three independent 75 Ω paths          │         │
│      │ • Full bandwidth on all channels        │         │
│      │ • Cable capacitance only degradation    │         │
│      │ • (NES doesn't natively output this,    │         │
│      │    but useful for modded consoles)       │         │
│      └─────────────────────────────────────────┘         │
│                                                          │
│  3e. RGB SCART                                           │
│      ┌─────────────────────────────────────────┐         │
│      │ • Four paths: R, G, B, sync             │         │
│      │ • No encode/decode, no chroma artifacts │         │
│      │ • Cable capacitance per channel         │         │
│      │ • (Modded consoles only)                │         │
│      └─────────────────────────────────────────┘         │
│                                                          │
│  Parameters exposed per cable type:                      │
│  • cable_length_meters (affects RC rolloff + ghost delay)│
│  • connector_quality (0=gold/clean, 1=corroded/loose)    │
│  • shield_quality (0=perfect, 1=unshielded)              │
│  • rf_interference_level (only if shield < 1)            │
│                                                          │
│  Output: degraded signal at TV input jack                │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 4: TV INPUT / TUNER STAGE                         │
│  ─────────────────────────────────                       │
│  Kernel types: RC low-pass + point-wise                  │
│                                                          │
│  The TV's input circuitry before video processing.       │
│                                                          │
│  Components:                                             │
│  • Input termination resistor (75 Ω)                     │
│    Mismatch with cable → standing wave artifacts         │
│  • Input coupling capacitor                              │
│    Another AC coupling stage, another RC high-pass       │
│  • AGC (automatic gain control)                          │
│    Slow-attack envelope follower                         │
│    Time constant: ~100 ms attack, ~1 s release           │
│    Normalizes signal level (dark scenes brighten         │
│    slightly, bright scenes compress slightly)            │
│  • Clamp circuit                                         │
│    Re-establishes black level reference                  │
│    (back porch of horizontal sync)                       │
│                                                          │
│  For RF path: add tuner stage between cable and here     │
│  • Channel selection (bandpass around carrier)           │
│  • Local oscillator + mixer (superheterodyne)            │
│  • IF amplifier + SAW filter (bandwidth shaping)         │
│  • Video detector (AM demodulation)                      │
│                                                          │
│  Output: baseband composite at TV's internal level       │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 5: COMB FILTER / Y-C SEPARATOR                    │
│  ────────────────────────────────────                    │
│  Kernel types: delay + FIR + point-wise                  │
│                                                          │
│  Separates luminance (Y) from chrominance (C) in the     │
│  composite signal. Quality varies enormously by TV.      │
│                                                          │
│  5a. NO COMB (cheap TV)                                  │
│      Notch filter at 3.58 MHz removes chroma from Y      │
│      Bandpass at 3.58 MHz extracts C from composite      │
│      Result: visible dot crawl, cross-color on edges     │
│                                                          │
│  5b. 2-LINE COMB (decent TV)                             │
│      Y = (line[n] + line[n-1]) / 2                       │
│         (chroma cancels due to 180° phase flip)          │
│      C = (line[n] - line[n-1]) / 2                       │
│         (luma cancels, chroma survives)                  │
│      1H glass delay line (older) or digital (newer)      │
│      Result: much less dot crawl, some vertical          │
│      smearing on horizontal color boundaries            │
│                                                          │
│  5c. 3-LINE COMB (high-end TV / PVM)                     │
│      Uses line[n-1], line[n], line[n+1]                  │
│      Adaptive: picks best pair based on correlation      │
│      Result: minimal artifacts, very clean separation    │
│                                                          │
│  5d. S-VIDEO INPUT (bypass)                              │
│      Y and C arrive on separate pins                     │
│      Skip comb filter entirely                           │
│                                                          │
│  Output: separated Y (luma) and C (modulated chroma)     │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 6: CHROMA DEMODULATOR                             │
│  ───────────────────────────                             │
│  Kernel types: modulator (carrier ×) + FIR              │
│                                                          │
│  Extracts I and Q (or U and V for PAL) color-difference  │
│  signals from the modulated chroma subcarrier.           │
│                                                          │
│  Components:                                             │
│  • Phase-locked loop (PLL)                               │
│    Locks to color burst (3.579545 MHz NTSC)              │
│    Generates local reference carriers for I and Q        │
│    PLL bandwidth affects hue stability                   │
│    Cheap TVs: wider PLL → more hue jitter                │
│                                                          │
│  • Synchronous demodulators (×2)                         │
│    I = C(t) × cos(ωt + φ)                               │
│    Q = C(t) × sin(ωt + φ)                               │
│    Phase offset φ = tint/hue control                     │
│                                                          │
│  • Chroma bandwidth filters                              │
│    I bandwidth: ~1.5 MHz (NTSC spec)                     │
│    Q bandwidth: ~0.5 MHz (NTSC spec)                     │
│    Cheap TVs: even narrower (0.5 / 0.3 MHz)             │
│    FIR or cascaded RC implementation                     │
│                                                          │
│  • Chroma gain (saturation/color control)                │
│    Amplifier with adjustable gain                        │
│    Often has slight nonlinearity at saturation           │
│                                                          │
│  Output: Y, I, Q as separate float buffers               │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 7: LUMA PROCESSING                                │
│  ────────────────────────                                │
│  Kernel types: FIR + RC + point-wise                     │
│                                                          │
│  The Y signal gets its own processing chain.             │
│                                                          │
│  Components:                                             │
│  • Luma bandwidth filter                                 │
│    FIR low-pass, cutoff 4-6 MHz                          │
│    Rejects residual chroma subcarrier from Y             │
│    Determines horizontal sharpness                       │
│                                                          │
│  • Sharpness / peaking circuit                           │
│    Resonant LC or active filter                          │
│    Adds overshoot on luma transitions (ringing)          │
│    The "sharpness" knob on the TV                        │
│    Too high → white halos around dark edges              │
│                                                          │
│  • Black level clamp                                     │
│    References to back porch level                        │
│    Establishes true black                                │
│                                                          │
│  • Brightness / contrast                                 │
│    Y' = contrast × (Y - black_level) + brightness        │
│    Standard TV user controls                             │
│                                                          │
│  Output: processed luma (Y')                             │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 8: MATRIX DECODE (YIQ → RGB)                      │
│  ──────────────────────────────────                      │
│  Kernel type: point-wise (3×3 matrix multiply)           │
│                                                          │
│  Standard NTSC decode matrix:                            │
│    R = Y + 0.956·I + 0.621·Q                            │
│    G = Y - 0.272·I - 0.647·Q                            │
│    B = Y - 1.106·I + 1.703·Q                            │
│                                                          │
│  Real TVs deviate from this:                             │
│  • Phosphor chromaticity shifts the effective matrix     │
│  • Color temperature (warm/cool) biases R/B gains       │
│  • Drive level differences per gun                       │
│  • Cutoff adjustments per gun (black level per channel)  │
│                                                          │
│  Parameters:                                             │
│  • color_temperature (3200K warm ... 9300K cool)         │
│  • r_drive, g_drive, b_drive (per-gun gain, 0.8-1.2)    │
│  • r_cutoff, g_cutoff, b_cutoff (per-gun black, ±0.05)  │
│                                                          │
│  Output: linear RGB per sample                           │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
          ┌────────────┴────────────┐
          │  RGB signal enters      │
          │  DISPLAY DOMAIN         │
          │  (Section 3.2)          │
          └─────────────────────────┘
```

### 3.2 Display domain (electron beam → photon)

```
RGB DRIVE SIGNALS (from matrix decode)
│
▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 9: VIDEO AMPLIFIER                                │
│  ────────────────────────                                │
│  Kernel types: RC low-pass + point-wise                  │
│                                                          │
│  Final amplification stage driving the electron guns.    │
│                                                          │
│  Components:                                             │
│  • Bandwidth-limited amplifier (per channel)             │
│    RC low-pass per gun, fc ≈ 5-8 MHz                     │
│    Creates the horizontal "softness" of CRT              │
│    R, G, B may have slightly different bandwidths        │
│    (chromatic aberration from electronics, not optics)   │
│                                                          │
│  • Gamma pre-distortion                                  │
│    CRT phosphors have inherent gamma ≈ 2.2-2.5          │
│    Some TVs partially compensate in the amplifier        │
│    Others don't (consumer vs broadcast monitor)          │
│                                                          │
│  • Clipping / saturation                                 │
│    Beam current can't go below zero (black) or above     │
│    the cathode emission limit (white clipping)           │
│    Soft knee at both ends                                │
│                                                          │
│  Output: RGB beam current drive signals                  │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 10: ELECTRON BEAM & DEFLECTION                    │
│  ────────────────────────────────────                    │
│  Kernel types: point-wise + FIR                          │
│                                                          │
│  Three electron beams (R, G, B) scan the phosphor screen │
│  left-to-right, top-to-bottom.                           │
│                                                          │
│  Physical phenomena:                                     │
│  • Beam spot profile                                     │
│    Gaussian in both X and Y                              │
│    Size varies with beam current (brightness)            │
│    Brighter → wider spot (bloom)                         │
│    beamWidth = min + (max - min) × luma^curve            │
│                                                          │
│  • Deflection geometry                                   │
│    Beam travels further to reach edges → larger spot     │
│    Pincushion/barrel distortion from flat-face tube      │
│    Trapezoidal distortion from yoke misalignment         │
│                                                          │
│  • Convergence                                           │
│    Three guns must hit same triad at every position      │
│    Perfect at center, drifts at edges                    │
│    Static convergence: fixed R/B offset                  │
│    Dynamic convergence: edge-dependent offset            │
│    Temporal wobble from magnetic field sensitivity       │
│                                                          │
│  • Horizontal scan jitter                                │
│    Timebase instability → sub-pixel horizontal wobble    │
│    Per-scanline random offset (h-sync noise)             │
│    Stronger on cheap TVs, nearly zero on PVMs            │
│                                                          │
│  • Vertical jitter                                       │
│    V-hold instability → whole-frame vertical bounce      │
│    Very slow oscillation (sub-Hz)                        │
│                                                          │
│  Output: beam position + intensity for each pixel        │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 11: PHOSPHOR SCREEN                               │
│  ─────────────────────────                               │
│  Kernel types: point-wise + 2D convolve (persistence)    │
│                                                          │
│  The electron beam strikes phosphor dots/stripes that    │
│  glow proportional to beam energy.                       │
│                                                          │
│  Physical phenomena:                                     │
│  • Phosphor geometry                                     │
│    Shadow mask: RGB dot triads in triangular pattern     │
│    Aperture grille: RGB vertical stripes (Trinitron)     │
│    Slot mask: rectangular RGB groups                     │
│    Pitch: 0.25-0.80 mm (determines visibility)          │
│                                                          │
│  • Phosphor response curve                               │
│    Gamma ≈ 2.2-2.5 (inherent to phosphor physics)       │
│    Not perfectly uniform across R, G, B phosphors        │
│    P22 phosphor set (standard TV):                       │
│      Red: Y₂O₂S:Eu (slightly orange-red)                │
│      Green: ZnS:Cu,Al (slightly yellow-green)            │
│      Blue: ZnS:Ag (deep blue)                           │
│                                                          │
│  • Persistence (afterglow)                               │
│    Phosphors continue glowing after beam passes          │
│    Decay time varies by phosphor type:                   │
│      P22 standard: ~1-3 ms to 10% (medium-short)        │
│      P7 long: ~100 ms (radar displays)                   │
│    Visible as motion blur / ghosting on fast movement    │
│    Model: exponential decay blended with previous frame  │
│                                                          │
│  • Scanline structure                                    │
│    Beam doesn't illuminate full vertical span            │
│    Gap between scanlines (especially visible at close    │
│    viewing distance or on larger screens)                │
│    Gap darkness depends on beam height vs scan pitch     │
│                                                          │
│  • Moire patterns                                        │
│    Interaction between phosphor mask pitch and           │
│    scanline pitch creates interference patterns          │
│    Especially visible on aperture grille displays        │
│                                                          │
│  Output: RGB phosphor emission intensities               │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 12: CRT GLASS                                     │
│  ───────────────────                                     │
│  Kernel types: 2D convolve (separable) + point-wise      │
│                                                          │
│  Light passes through the CRT faceplate glass before     │
│  reaching the viewer.                                    │
│                                                          │
│  Physical phenomena:                                     │
│  • Halation                                              │
│    Light scatters inside the thick glass faceplate       │
│    Bright areas create a soft glow halo                  │
│    Radius depends on glass thickness (~10-15 mm)         │
│    Modeled as large-radius Gaussian blur of bright       │
│    areas, added back at low intensity                    │
│                                                          │
│  • Glass tint                                            │
│    Most CRTs have slightly tinted glass (gray or         │
│    amber) to improve contrast in ambient light           │
│    Multiplies all RGB equally by 0.6-0.9                 │
│    Trinitron: slightly warmer tint than shadow mask      │
│                                                          │
│  • Curvature                                             │
│    Barrel distortion from curved faceplate               │
│    Flat-face tubes (late model): nearly zero             │
│    Classic curved: 2-5% barrel                           │
│    Bubble screen (very old): up to 10%                   │
│                                                          │
│  • Anti-glare coating                                    │
│    Etched glass → slight diffusion of phosphor image     │
│    Chemical coating → color shift at viewing angles      │
│    Trinitron: often no coating (very glossy)             │
│                                                          │
│  Output: final visible image through glass               │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 13: VIEWING ENVIRONMENT                           │
│  ─────────────────────────────                           │
│  Kernel type: point-wise                                 │
│                                                          │
│  The final perceptual adjustments.                       │
│                                                          │
│  • Vignette                                              │
│    Beam deflection limits → corners are dimmer           │
│    Follows cos⁴(θ) law from deflection angle             │
│                                                          │
│  • Ambient light reflection                              │
│    Room light reflects off CRT glass                     │
│    Raises black level, reduces perceived contrast        │
│    Depends on glass tint and coating                     │
│                                                          │
│  • Display gamma                                         │
│    Final gamma curve for the output device               │
│    Maps linear light to perceptual (sRGB)                │
│                                                          │
│  Output: final pixel values (sRGB for display)           │
└──────────────────────────────────────────────────────────┘
```

---

## 4. Audio Signal Chain

The complete physical model from APU digital output to acoustic pressure
at the listener's ear.

### 4.1 Signal generation (CPU, stays on CPU)

The APU channel state machines (pulse, triangle, noise, DMC) are
cycle-accurate and must remain on the CPU. They produce one raw digital
sample per CPU cycle at ~1.79 MHz.

The CPU emits a buffer of raw samples per frame (~29,780 samples at
NTSC rate) which is uploaded to the GPU for analog path simulation.

### 4.2 Analog path (GPU compute)

```
APU RAW DIGITAL OUTPUT (~29,780 samples/frame, from CPU)
│
▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 1: DAC (Digital-to-Analog Converter)              │
│  ──────────────────────────────────────────               │
│  Kernel type: point-wise (LUT)                           │
│                                                          │
│  The 2A03 has two separate DAC circuits:                 │
│  • Pulse DAC: maps pulse1 + pulse2 (0-30) to voltage    │
│    Formula: 95.88 / (8128/n + 100)                      │
│    Nonlinear — high values compress slightly             │
│  • TND DAC: maps 3×tri + 2×noise + dmc (0-202)          │
│    Formula: 163.67 / (24329/n + 100)                    │
│    Even more nonlinear — the resistor ladder has         │
│    measurable deviations from the ideal formula          │
│                                                          │
│  Additional DAC character:                               │
│  • Quantization noise (7-bit DMC, 4-bit others)         │
│  • Per-unit variation (component tolerance)              │
│  • Temperature dependence (slight drift over session)    │
│                                                          │
│  Parameter: dac_nonlinearity (0 = ideal formula,         │
│  1 = measured hardware curve with cubic warping)         │
│                                                          │
│  Output: analog voltage, one sample per CPU cycle        │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 2: MIXER / SUMMING NETWORK                        │
│  ────────────────────────────────                        │
│  Kernel type: point-wise                                 │
│                                                          │
│  The pulse and TND outputs are summed through a          │
│  resistor network on the 2A03 die.                       │
│                                                          │
│  • Resistor ladder nonlinearity                          │
│    Combined output is NOT pulse_dac + tnd_dac            │
│    Real hardware: slight interaction between channels    │
│    When both are loud, total is slightly less than sum   │
│                                                          │
│  • DMC bus crosstalk                                     │
│    $4011 (DMC direct load) bleeds into audio bus         │
│    Famicom-specific: some games exploit this for         │
│    crude PCM playback                                    │
│    Model: add fraction of DMC level to output            │
│                                                          │
│  Output: mixed analog signal                             │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 3: OUTPUT AMPLIFIER                               │
│  ─────────────────────────                               │
│  Kernel types: RC low-pass + RC high-pass + point-wise   │
│                                                          │
│  The 2A03's internal output stage and the motherboard    │
│  audio circuit.                                          │
│                                                          │
│  Components:                                             │
│  • DC blocking capacitor (coupling cap)                  │
│    RC high-pass, fc ≈ 16-90 Hz                           │
│    Removes ~0.5V DC bias from DAC output                │
│    Time constant τ = R×C determines bass rolloff         │
│    Famicom: different cap value than NES                 │
│    R ≈ 10 kΩ, C ≈ 10-100 µF                             │
│                                                          │
│  • Amplifier gain stage                                  │
│    Voltage gain ≈ 2-4×                                   │
│    Bandwidth limited by op-amp/transistor GBW            │
│    RC low-pass, fc ≈ 8-14 kHz                            │
│    Slew rate limit → soft clipping on transients         │
│                                                          │
│  • Feedback network                                      │
│    Sets gain and frequency response                      │
│    RC high-pass in feedback loop, fc ≈ 440 Hz            │
│    Shapes the bass response (not a simple shelf)         │
│                                                          │
│  • Amplifier saturation                                  │
│    Soft clipping when all channels peak simultaneously   │
│    tanh-like compression curve                           │
│    drive = 1.0 + saturation × 4.0                        │
│    y = tanh(y × drive) / tanh(drive)                     │
│                                                          │
│  • Power supply noise injection                          │
│    60 Hz hum from rectifier ripple (NTSC)                │
│    50 Hz hum (PAL/Dendy)                                 │
│    Amplitude depends on PSU quality and age              │
│    Modeled as sinusoidal additive signal                 │
│                                                          │
│  • Thermal noise floor                                   │
│    White noise from resistor Johnson noise               │
│    Amplitude: -60 to -120 dB below full scale            │
│    Audible in quiet passages                             │
│                                                          │
│  Filter coefficients by hardware variant:                │
│  ┌─────────────┬─────────┬─────────┬────────┐            │
│  │ Variant     │ HP1     │ HP2     │ LP     │            │
│  ├─────────────┼─────────┼─────────┼────────┤            │
│  │ Famicom     │ 16 Hz   │ 440 Hz  │ 10 kHz │            │
│  │ NES (front) │ 90 Hz   │ 440 Hz  │ 14 kHz │            │
│  │ NES (top)   │ 90 Hz   │ 440 Hz  │ 12 kHz │            │
│  │ Dendy       │ 37 Hz   │ 440 Hz  │ 8 kHz  │            │
│  └─────────────┴─────────┴─────────┴────────┘            │
│                                                          │
│  Output: amplified, filtered audio at console output     │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 4: AUDIO CABLE                                    │
│  ────────────────────                                    │
│  Kernel types: RC low-pass + point-wise                  │
│                                                          │
│  Audio travels alongside video in composite/RF cables,   │
│  or through a separate audio cable.                      │
│                                                          │
│  Components:                                             │
│  • Cable capacitance                                     │
│    Shunt capacitance: 30-100 pF/meter                    │
│    With 10 kΩ source impedance:                          │
│    6 ft cable (2m): fc ≈ 80 kHz (inaudible)              │
│    50 ft cable (15m): fc ≈ 10 kHz (audible!)             │
│    RC low-pass, parameters from cable_length             │
│                                                          │
│  • Cable resistance                                      │
│    Series: 0.05-0.5 Ω/meter (negligible for audio)      │
│    Connector contact: 0.1-10 Ω (can cause crackle       │
│    if modeled as intermittent — but static for now)      │
│                                                          │
│  • RF path: audio subcarrier                             │
│    Audio FM-modulated at 4.5 MHz offset from video       │
│    Limited to ~50 Hz - 15 kHz bandwidth                  │
│    Additional noise from FM demodulation                 │
│    Intercarrier buzz (video modulating audio carrier)    │
│                                                          │
│  • Crosstalk from video signal                           │
│    In shared composite cables, video signal can          │
│    couple into audio (especially at horizontal rate      │
│    = 15.734 kHz, audible as a faint whine)               │
│                                                          │
│  Output: audio signal at TV/monitor input                │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 5: TV/MONITOR AUDIO INPUT & AMPLIFIER             │
│  ───────────────────────────────────────────              │
│  Kernel types: RC high-pass + RC low-pass + point-wise   │
│                                                          │
│  The TV's audio processing chain.                        │
│                                                          │
│  Components:                                             │
│  • Input coupling capacitor                              │
│    Another DC blocking stage                             │
│    RC high-pass, fc ≈ 20-100 Hz                          │
│                                                          │
│  • Tone control circuit (bass/treble)                    │
│    Baxandall tone stack or simpler RC shelving           │
│    Bass: shelf at ~300 Hz, ±10 dB                        │
│    Treble: shelf at ~3 kHz, ±10 dB                       │
│                                                          │
│  • Volume control                                        │
│    Logarithmic potentiometer characteristic              │
│    Not linear! Half rotation ≠ half volume               │
│                                                          │
│  • Power amplifier                                       │
│    Class AB push-pull (most consumer TVs)                │
│    Crossover distortion at low signal levels             │
│    Power supply sag on loud passages                     │
│    Clipping at maximum output (hard clip, not soft)      │
│                                                          │
│  Output: amplified signal driving speaker                │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 6: SPEAKER / TRANSDUCER                           │
│  ─────────────────────────────                           │
│  Kernel types: FIR (impulse response) or biquad chain    │
│                                                          │
│  The speaker is the most colored component in the chain. │
│  A TV speaker sounds nothing like headphones.            │
│                                                          │
│  Physical model:                                         │
│  • Resonant frequency                                    │
│    Small TV speaker: 200-400 Hz (no real bass below)     │
│    Larger TV: 100-200 Hz                                 │
│    Modeled as 2nd-order high-pass (resonant)             │
│                                                          │
│  • Frequency response                                    │
│    TV speaker: huge mid-range peak, rolled off highs     │
│    Typical: +6 dB at 1-3 kHz, -12 dB above 8 kHz        │
│    Modeled as chain of biquad filters                    │
│                                                          │
│  • Cabinet resonance                                     │
│    TV enclosure has resonant modes                       │
│    Boomy/boxy coloration at 200-500 Hz                   │
│    Modeled as peaking EQ biquad                          │
│                                                          │
│  • Cone breakup                                          │
│    Above ~5 kHz, paper cone doesn't move as piston      │
│    Modes create peaks and notches                        │
│    Modeled as additional biquad peaks                    │
│                                                          │
│  Speaker profiles:                                       │
│  ┌─────────────────┬───────┬────────┬──────────┐         │
│  │ Type            │ f_res │ BW     │ Character│         │
│  ├─────────────────┼───────┼────────┼──────────┤         │
│  │ Small TV (13")  │ 350Hz │ 400-6k │ tinny    │         │
│  │ Console TV (27")│ 150Hz │ 80-10k │ warm     │         │
│  │ PVM monitor     │ 100Hz │ 60-15k │ flat     │         │
│  │ Arcade cabinet  │ 200Hz │ 100-8k │ mid-honk │         │
│  │ Headphones      │ 20Hz  │ 20-20k │ neutral  │         │
│  │ Famicom RF TV   │ 400Hz │ 500-4k │ muffled  │         │
│  └─────────────────┴───────┴────────┴──────────┘         │
│                                                          │
│  Output: speaker excursion signal (acoustic analog)      │
└──────────────────────┬───────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────┐
│  STAGE 7: DECIMATION / OUTPUT                            │
│  ────────────────────────────                            │
│  Kernel type: FIR convolution                            │
│                                                          │
│  Downsample from internal processing rate to output      │
│  sample rate (44.1 or 48 kHz).                           │
│                                                          │
│  • Kaiser-windowed sinc, 65-129 taps                     │
│  • Cutoff at Nyquist/2 of output rate                    │
│  • Polyphase implementation for non-integer ratios       │
│  • Currently exists on CPU (apu.h resampler)             │
│    Can move to GPU as final FIR dispatch                 │
│                                                          │
│  • Final output gain / limiter                           │
│    Prevents clipping from accumulated processing         │
│    Soft limiter with 1 dB headroom                       │
│                                                          │
│  Output: 48 kHz float samples → audio ring buffer        │
└──────────────────────────────────────────────────────────┘
```

---

## 5. Transmission Medium Simulation

This is the critical new subsystem that sits between encode and decode
for both video and audio. It models the physical cable as what it
actually is: a distributed RC transmission line.

### 5.1 Cable as distributed RC network

A real cable is not a single RC filter. It's an infinite series of
infinitesimal RC sections. For simulation, we approximate with N
discrete sections:

```
Signal in ──┬──[R/N]──┬──[R/N]──┬──[R/N]──┬── ... ──┬── Signal out
            │         │         │         │          │
           [C/N]     [C/N]     [C/N]     [C/N]      [C/N]
            │         │         │         │          │
           GND       GND       GND       GND        GND
```

Each section is one RC low-pass. N sections in series = N prefix scan
dispatches. For audio frequencies, N = 2-4 is sufficient. For video
bandwidth, N = 4-8 captures the distributed behavior.

**Key insight for GPU:** each section is an independent prefix scan over
the sample buffer. The sections must execute sequentially (output of
section K feeds input of section K+1), but within each section, all
samples are processed in parallel.

### 5.2 Cable parameters

```c
typedef struct {
    float length_meters;          // physical length
    float resistance_per_meter;   // series R (Ω/m)
    float capacitance_per_meter;  // shunt C (F/m)
    float inductance_per_meter;   // series L (H/m, usually negligible for audio)
    int   num_sections;           // RC ladder approximation order
    float connector_resistance;   // total contact R at both ends (Ω)
    float shield_effectiveness;   // 0-1 (1 = perfect shield)
    float impedance_mismatch;     // 0-1 (0 = matched, 1 = open)
} CableParams;
```

### 5.3 Impedance mismatch and reflections (ghosting)

When cable impedance doesn't match source/load impedance, signal
reflects back and forth. Each reflection is a delayed, attenuated copy:

```
y[n] = x[n] + Γ·x[n - D] + Γ²·x[n - 2D] + ...
```

Where:
- `Γ` = reflection coefficient = (Z_load - Z_cable) / (Z_load + Z_cable)
- `D` = round-trip delay = 2 × length / propagation_velocity

This is a series of delay-line kernels with exponentially decaying gain.
Usually 1-2 reflections are sufficient (Γ² < 0.1 for reasonable setups).

### 5.4 RF path (antenna connection)

The RF path adds two major subsystems around the cable model:

```
Console ──→ [RF Modulator] ──→ [Cable/Antenna] ──→ [RF Tuner] ──→ TV
```

**RF Modulator (console side):**
- AM modulation: `rf[n] = (1 + m·composite[n]) × cos(2π·f_carrier·n/fs)`
- Carrier frequency: 61.25 MHz (Ch 3) or 67.25 MHz (Ch 4)
- Vestigial sideband filter: asymmetric bandwidth (wider lower sideband)
- Audio subcarrier: FM at +4.5 MHz offset
- Implementation: modulator kernel + FIR bandwidth limit

**Antenna/cable noise:**
- Thermal noise floor (white noise, level depends on temperature)
- Interference from other sources (modeled as filtered noise bursts)
- Ignition noise (impulsive, random timing)
- Adjacent channel leakage (filtered noise at carrier ± 6 MHz)

**RF Tuner (TV side):**
- Bandpass filter around selected channel (SAW filter model)
- Envelope detection or synchronous demodulation
- AGC with attack/release time constants
- Additional bandwidth limiting from IF amplifier

### 5.5 Power supply noise model

Present in every active stage (console output amp, TV input amp, etc.):

```c
typedef struct {
    float mains_frequency;    // 60 Hz (NTSC) or 50 Hz (PAL)
    float fundamental_level;  // amplitude of fundamental
    float harmonic_2_level;   // 120/100 Hz component
    float harmonic_3_level;   // 180/150 Hz component
    float ripple_frequency;   // full-wave rectifier: 2× mains
    float random_variation;   // PSU regulation quality (0=perfect)
} PowerSupplyParams;
```

Injected as additive signal at each amplifier stage. The hum character
differs at each stage because the PSU noise is different at each
voltage rail.

### 5.6 Connection type dispatch

The transmission medium stage is configured by selecting a connection
type. Each type activates a different subset of the cable/RF subsystems:

| Connection | Video encode | Cable model | Video decode | Audio path |
|---|---|---|---|---|
| RF (antenna) | Composite → RF mod | Coax + noise + reflections | RF tuner → composite decode | FM subcarrier |
| Composite (RCA) | Native composite | Single 75Ω cable | Direct composite decode | Separate RCA cable |
| S-Video | Y/C separation at console | Dual 75Ω cables | Bypass comb filter | Separate RCA cable |
| Component | RGB/YPbPr at console | Triple 75Ω cables | Direct matrix | Separate RCA cable |
| RGB SCART | Direct RGB | Quad cables | No decode needed | Separate in SCART |
| Direct (no cable) | — | — | — | — |

---

## 6. Metal Compute Dispatch Architecture

### 6.1 Dispatch chain overview

All stages execute as Metal compute kernels within a single command
buffer per frame. Metal's implicit resource tracking ensures correct
ordering.

```
┌─────────────────────────────────────────────────────────────┐
│                    COMMAND BUFFER (per frame)                │
│                                                             │
│  ┌─── VIDEO SIGNAL DOMAIN ────────────────────────────┐     │
│  │                                                    │     │
│  │  Dispatch 1: Console output stage                  │     │
│  │    kernel: rc_highpass (coupling cap)               │     │
│  │    kernel: rc_lowpass (amp bandwidth)               │     │
│  │    kernel: pointwise (saturation + PSU hum)         │     │
│  │                                                    │     │
│  │  Dispatch 2: Cable transmission                    │     │
│  │    kernel: rc_lowpass × N (distributed RC ladder)   │     │
│  │    kernel: delay_add (impedance reflections)        │     │
│  │    kernel: pointwise (noise injection)              │     │
│  │                                                    │     │
│  │  Dispatch 3: RF modulator (if RF path)             │     │
│  │    kernel: modulator (AM encode)                    │     │
│  │    kernel: fir (vestigial sideband)                 │     │
│  │    kernel: pointwise (noise + interference)         │     │
│  │                                                    │     │
│  │  Dispatch 4: RF tuner (if RF path)                 │     │
│  │    kernel: fir (bandpass / SAW filter)              │     │
│  │    kernel: pointwise (AGC envelope)                 │     │
│  │    kernel: demodulator (envelope detect)            │     │
│  │                                                    │     │
│  │  Dispatch 5: TV input stage                        │     │
│  │    kernel: rc_highpass (input coupling)             │     │
│  │    kernel: pointwise (AGC + clamp)                  │     │
│  │                                                    │     │
│  │  Dispatch 6: Comb filter                           │     │
│  │    kernel: delay + add (Y/C separation)             │     │
│  │                                                    │     │
│  │  Dispatch 7: Chroma demodulator                    │     │
│  │    kernel: modulator × 2 (I, Q carrier multiply)    │     │
│  │    kernel: fir × 2 (I, Q bandwidth limit)           │     │
│  │                                                    │     │
│  │  Dispatch 8: Luma processing                       │     │
│  │    kernel: fir (bandwidth limit)                    │     │
│  │    kernel: pointwise (peaking / sharpness)          │     │
│  │    kernel: pointwise (brightness, contrast)         │     │
│  │                                                    │     │
│  │  Dispatch 9: Matrix decode                         │     │
│  │    kernel: pointwise (YIQ → RGB matrix)             │     │
│  │                                                    │     │
│  └────────────────────────────────────────────────────┘     │
│                                                             │
│  ┌─── VIDEO DISPLAY DOMAIN ───────────────────────────┐     │
│  │                                                    │     │
│  │  Dispatch 10: Video amplifier                      │     │
│  │    kernel: rc_lowpass × 3 (per-gun bandwidth)       │     │
│  │    kernel: pointwise (gamma, clipping)              │     │
│  │                                                    │     │
│  │  Dispatch 11: CRT phosphor render (existing)       │     │
│  │    kernel: crtPhosphorRender                        │     │
│  │    (beam profile, phosphor mask, scanlines,         │     │
│  │     convergence, bloom, tone mapping)               │     │
│  │                                                    │     │
│  │  Dispatch 12: Halation (existing, separable)       │     │
│  │    kernel: halationBlurH                            │     │
│  │    kernel: halationBlurV                            │     │
│  │                                                    │     │
│  │  Dispatch 13: Final composite                      │     │
│  │    kernel: pointwise (vignette, glass tint,         │     │
│  │            ambient, final gamma)                    │     │
│  │                                                    │     │
│  └────────────────────────────────────────────────────┘     │
│                                                             │
│  ┌─── AUDIO SIGNAL DOMAIN ────────────────────────────┐     │
│  │                                                    │     │
│  │  Dispatch A1: DAC                                  │     │
│  │    kernel: pointwise (voltage LUT)                  │     │
│  │                                                    │     │
│  │  Dispatch A2: Mixer                                │     │
│  │    kernel: pointwise (resistor network sum)         │     │
│  │                                                    │     │
│  │  Dispatch A3: Console output amp                   │     │
│  │    kernel: rc_highpass (DC blocking cap)             │     │
│  │    kernel: rc_highpass (feedback network)            │     │
│  │    kernel: rc_lowpass (amp bandwidth)                │     │
│  │    kernel: pointwise (saturation + hum + noise)     │     │
│  │                                                    │     │
│  │  Dispatch A4: Audio cable                          │     │
│  │    kernel: rc_lowpass (cable capacitance)            │     │
│  │                                                    │     │
│  │  Dispatch A5: TV audio amp                         │     │
│  │    kernel: rc_highpass (input coupling)              │     │
│  │    kernel: pointwise (tone controls)                │     │
│  │    kernel: pointwise (power amp + clipping)         │     │
│  │                                                    │     │
│  │  Dispatch A6: Speaker model                        │     │
│  │    kernel: biquad chain (resonance + response)      │     │
│  │    kernel: pointwise (cabinet coloration)           │     │
│  │                                                    │     │
│  │  Dispatch A7: Decimation                           │     │
│  │    kernel: fir (anti-alias downsample to 48 kHz)    │     │
│  │                                                    │     │
│  └────────────────────────────────────────────────────┘     │
│                                                             │
│  commandBuffer.commit()                                     │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 Generic stage dispatcher

Rather than hardcoding each dispatch, implement a generic chain runner:

```swift
struct AnalogStage {
    let kernelType: KernelType      // .pointwise, .rcLowpass, .rcHighpass, .fir, .delay, .mod
    let params: [Float]             // R, C values or FIR taps or LUT data
    let enabled: Bool               // skip if false (preset-dependent)
}

struct AnalogChain {
    let stages: [AnalogStage]
    let sampleRate: Float           // processing rate for this chain
    let bufferSize: Int             // samples per frame
}

func dispatchChain(_ chain: AnalogChain,
                   input: MTLBuffer,
                   output: MTLBuffer,
                   encoder: MTLComputeCommandEncoder)
{
    var current = input
    for stage in chain.stages where stage.enabled {
        let next = getIntermediateBuffer()
        dispatchKernel(stage.kernelType,
                      params: stage.params,
                      input: current,
                      output: next,
                      encoder: encoder)
        current = next
    }
    // Copy final result to output
    blit(current, to: output, encoder: encoder)
}
```

### 6.3 Thread group sizing

| Kernel type | Threadgroup | Rationale |
|---|---|---|
| Point-wise (1D audio) | 256×1 | Simple linear dispatch |
| Point-wise (2D video) | 16×16 | 2D spatial locality |
| RC prefix scan | 1024×1 | One block = one scan unit |
| FIR convolution | 256×1 | Shared memory for tap window |
| Delay + add | 256×1 | Simple with offset |
| 2D separable blur | 32×16 | Cache-friendly row/col access |

---

## 7. Buffer Layout & Memory Architecture

### 7.1 Video buffers

```
Upload (CPU → GPU, per frame):
  composite_waveform:  MTLBuffer, ~2048 × 240 × sizeof(float) ≈ 1.9 MB
  (or palette_indices: MTLBuffer, 256 × 240 × sizeof(uint16) ≈ 120 KB
   if composite encode moves to GPU)

Intermediate (GPU only, ping-pong):
  signal_buf_a:   MTLBuffer, 2048 × 240 × float ≈ 1.9 MB
  signal_buf_b:   MTLBuffer, 2048 × 240 × float ≈ 1.9 MB
  yiq_buf:        MTLBuffer, 2048 × 240 × float × 3 ≈ 5.6 MB (Y, I, Q)
  rgb_buf:        MTLBuffer, 2048 × 240 × float × 3 ≈ 5.6 MB

Display textures (GPU only, existing):
  inputTexture:        MTLTexture, bgra8Unorm
  rfOutputTexture:     MTLTexture, rgba16Float
  halationTempTexture: MTLTexture, rgba16Float
  halationTexture:     MTLTexture, rgba16Float
  drawable:            MTLTexture, bgra10_xr_srgb

Total GPU memory for video: ~20 MB (with intermediates)
```

### 7.2 Audio buffers

```
Upload (CPU → GPU, per frame):
  raw_apu_samples: MTLBuffer, ~29780 × sizeof(float) ≈ 116 KB
  (plus channel-separated data if mixer moves to GPU)

Intermediate (GPU only, ping-pong):
  audio_buf_a:  MTLBuffer, 29780 × float ≈ 116 KB
  audio_buf_b:  MTLBuffer, 29780 × float ≈ 116 KB

Readback (GPU → CPU, per frame):
  output_samples: MTLBuffer, 800 × float ≈ 3.2 KB
  (48000 / 60 ≈ 800 samples per frame)
  .storageModeShared for CPU readback

Total GPU memory for audio: ~350 KB
```

### 7.3 Shared resources

```
Kernel library (compiled once):
  MTLLibrary with all kernel functions
  ~6 pipeline state objects (one per kernel type)

Parameter buffers (per frame, small):
  rc_params:      MTLBuffer, sizeof(RCParams) per stage
  fir_taps:       MTLBuffer, max 129 × float per FIR stage
  transfer_lut:   MTLBuffer, 256-2048 × float per LUT stage
  chain_config:   MTLBuffer, stage descriptors for generic dispatcher
```

---

## 8. Preset System

Presets become component chain configurations. Each preset specifies
which stages are active and their component values.

### 8.1 Preset structure

```c
typedef struct {
    // Connection type (determines which stages are active)
    ConnectionType connection;   // RF, COMPOSITE, SVIDEO, COMPONENT, RGB, DIRECT

    // Console variant (affects internal filtering)
    ConsoleVariant console;      // FAMICOM, NES_FRONTLOADER, NES_TOPLOADER, DENDY

    // Cable
    CableParams video_cable;
    CableParams audio_cable;     // may differ from video cable

    // TV characteristics
    CombFilterType comb;         // NONE, TWO_LINE, THREE_LINE
    float chroma_bandwidth;      // Hz
    float luma_bandwidth;        // Hz
    float color_temperature;     // Kelvin
    float hue_offset;            // degrees
    float saturation;            // multiplier

    // CRT display
    PhosphorMaskType mask;       // SHADOW_MASK, APERTURE_GRILLE, SLOT_MASK
    float mask_pitch;            // mm
    float beam_sharpness;
    float persistence;           // seconds
    float halation;
    float barrel;

    // Speaker
    SpeakerType speaker;         // SMALL_TV, CONSOLE_TV, PVM, ARCADE, HEADPHONES
    float speaker_resonance;     // Hz
    float speaker_bandwidth_low; // Hz
    float speaker_bandwidth_high;// Hz

    // Power supply
    PowerSupplyParams psu;

    // Environment
    float ambient_light;
    float viewing_distance;      // affects perceived mask visibility
} PhysicalPreset;
```

### 8.2 Example presets

**"Living Room 1988" (composite on 19" Zenith)**
```
connection:     COMPOSITE
console:        NES_FRONTLOADER
video_cable:    { length: 2.0m, cap: 67pF/m, sections: 3 }
audio_cable:    { length: 2.0m, cap: 67pF/m, sections: 2 }
comb:           NONE (cheap TV)
chroma_bw:      0.8 MHz
luma_bw:        4.0 MHz
color_temp:     6500 K
mask:           SHADOW_MASK
mask_pitch:     0.60 mm
persistence:    2 ms
speaker:        SMALL_TV (f_res=350 Hz, BW=400-6k)
psu:            { hum_60hz: -50 dB, ripple: moderate }
```

**"Bedroom RF 1990" (antenna on 13" GE)**
```
connection:     RF
console:        NES_FRONTLOADER
video_cable:    { length: 1.5m, cap: 67pF/m + RF mod/demod }
comb:           NONE
chroma_bw:      0.5 MHz (RF bandwidth loss)
luma_bw:        3.0 MHz
color_temp:     7500 K (blueish)
mask:           SHADOW_MASK
mask_pitch:     0.80 mm (visible on 13")
speaker:        SMALL_TV (f_res=400 Hz, BW=500-4k, tinny)
psu:            { hum_60hz: -45 dB, ripple: heavy }
rf_noise:       -55 dBm (moderate snow)
```

**"Studio PVM" (S-Video on Sony PVM-20M4U)**
```
connection:     SVIDEO
console:        NES_FRONTLOADER
video_cable:    { length: 1.0m, cap: 50pF/m, quality: high }
comb:           bypassed (S-Video)
chroma_bw:      1.5 MHz (full NTSC spec)
luma_bw:        6.0 MHz
color_temp:     6500 K (D65 calibrated)
mask:           APERTURE_GRILLE (Trinitron)
mask_pitch:     0.31 mm (barely visible)
beam_sharpness: 0.95 (very tight)
persistence:    1.5 ms
speaker:        PVM (f_res=100 Hz, BW=60-15k, flat)
psu:            { hum_60hz: -80 dB, ripple: negligible }
```

**"Famicom on Kitchen TV" (RF, Japanese market)**
```
connection:     RF
console:        FAMICOM
video_cable:    { length: 3.0m, cap: 80pF/m, shield: 0.7 }
comb:           NONE
chroma_bw:      0.4 MHz
luma_bw:        2.5 MHz
color_temp:     7000 K
mask:           SHADOW_MASK
mask_pitch:     0.70 mm
speaker:        SMALL_TV (f_res=380 Hz, BW=400-5k)
psu:            { hum_50hz: 0, hum_60hz: -48 dB } // Japan is 50/60 split
audio:          { dmc_bus_crosstalk: 0.3 } // Famicom-specific
```

**"Arcade Cabinet" (RGB on Wells Gardner)**
```
connection:     RGB
console:        NES_FRONTLOADER  // PlayChoice-10 or VS System
video_cable:    { length: 0.5m, cap: 50pF/m }
comb:           bypassed (RGB)
luma_bw:        8.0 MHz (full RGB bandwidth)
color_temp:     5500 K (warm P22)
mask:           SHADOW_MASK
mask_pitch:     0.28 mm (high-res arcade tube)
speaker:        ARCADE (f_res=200 Hz, BW=100-8k, mid-forward)
psu:            { hum_60hz: -55 dB } // shared cab power
ambient_light:  0.15 (dim arcade)
```

---

## 9. Parameter Binding & Swift Integration

### 9.1 Swift parameter architecture

The existing `EmulatorCore.swift` has ~100 `@Published` properties with
`didSet` pushing values to the C bridge. The new architecture extends
this with structured parameter groups:

```swift
// Each physical stage has its own parameter group
class PhysicalVideoParams: ObservableObject {
    @Published var consoleOutputCouplingCap: Float = 220e-6  // Farads
    @Published var consoleOutputImpedance: Float = 75.0      // Ohms
    @Published var consoleAmpBandwidth: Float = 6e6          // Hz
    @Published var consoleAmpSaturation: Float = 0.0         // 0-1
    @Published var consolePSUHum: Float = 0.0                // amplitude
    // ...
}

class CableParams: ObservableObject {
    @Published var connectionType: ConnectionType = .composite
    @Published var cableLengthMeters: Float = 2.0
    @Published var cableCapacitancePerMeter: Float = 67e-12  // F/m
    @Published var cableResistancePerMeter: Float = 0.1      // Ω/m
    @Published var connectorQuality: Float = 0.9             // 0-1
    @Published var shieldEffectiveness: Float = 0.85         // 0-1
    // ...
}

class TVParams: ObservableObject {
    @Published var combFilterType: CombType = .twoLine
    @Published var chromaBandwidth: Float = 1.0e6            // Hz
    @Published var lumaBandwidth: Float = 4.5e6              // Hz
    @Published var colorTemperature: Float = 6500            // Kelvin
    // ...
}

class SpeakerParams: ObservableObject {
    @Published var speakerType: SpeakerType = .smallTV
    @Published var resonanceFrequency: Float = 350           // Hz
    @Published var bandwidthLow: Float = 400                 // Hz
    @Published var bandwidthHigh: Float = 6000               // Hz
    @Published var cabinetResonance: Float = 0.3             // 0-1
    // ...
}
```

### 9.2 Parameter flow

```
Swift UI slider → @Published property → didSet observer
    → compute Metal coefficients (a, b from R, C)
    → write to parameter MTLBuffer
    → next frame dispatch uses new coefficients
```

**Critical:** R and C values are user-facing (physically meaningful).
The `a` and `b` IIR coefficients are computed in Swift from R, C, and
sample rate. Users adjust "cable length" or "coupling capacitor value";
the conversion to filter math is hidden.

### 9.3 UI organization

```
Settings panel:
├── Preset selector (dropdown: "Living Room 1988", "Studio PVM", ...)
│
├── Console
│   ├── Variant: [Famicom | NES Front | NES Top | Dendy]
│   └── PSU quality: [slider]
│
├── Connection
│   ├── Type: [RF | Composite | S-Video | Component | RGB]
│   ├── Cable length: [slider, 0.5m - 10m]
│   ├── Cable quality: [slider, "new" to "corroded"]
│   └── RF noise: [slider, only visible for RF type]
│
├── Television
│   ├── Comb filter: [None | 2-Line | 3-Line]
│   ├── Luma bandwidth: [slider]
│   ├── Chroma bandwidth: [slider]
│   ├── Color temperature: [slider, warm-cool]
│   ├── Tint / Hue: [slider]
│   └── Saturation: [slider]
│
├── Display (CRT)
│   ├── Mask type: [Shadow Mask | Aperture Grille | Slot Mask]
│   ├── Mask pitch: [slider]
│   ├── Beam sharpness: [slider]
│   ├── Persistence: [slider]
│   ├── Halation: [slider]
│   ├── Barrel distortion: [slider]
│   └── Convergence: [slider]
│
├── Audio
│   ├── Speaker type: [Small TV | Console TV | PVM | Arcade | Headphones]
│   ├── Speaker resonance: [slider]
│   ├── Speaker bandwidth: [range slider]
│   └── Cabinet resonance: [slider]
│
└── Environment
    ├── Ambient light: [slider]
    └── Viewing distance: [slider]
```

---

## 10. Performance Budget

### 10.1 Per-frame timing targets

At 60 fps, each frame has 16.67 ms total. Target: all GPU processing
completes within 8 ms (leaving headroom for CPU work and display).

| Domain | Dispatches | Estimated time | Notes |
|---|---|---|---|
| Video signal (encode→decode) | 8-12 | 2-4 ms | Depends on RF/composite/S-Video path |
| Video display (beam→glass) | 3-4 | 2-3 ms | Existing CRT render + halation |
| Audio signal (DAC→speaker) | 6-7 | 0.2-0.5 ms | Small buffers (30k samples) |
| Audio decimation | 1 | 0.05 ms | FIR downsample |
| **Total GPU** | **18-24** | **4-8 ms** | **Well within budget** |

### 10.2 Optimization strategies

**RC prefix scan block size:** 1024 samples is the sweet spot. Smaller
wastes occupancy; larger exceeds shared memory. Audio frames (~30k
samples) need ~30 blocks. Video scanlines (~2048 samples) need 2 blocks.

**Kernel fusion:** Adjacent point-wise stages can fuse into one kernel
(e.g., DAC + mixer + hum injection = one dispatch instead of three).
Keep them logically separate in the chain definition but merge at
compile time.

**Conditional dispatch:** Skip entire stage groups based on connection
type. RGB connection skips all of: RF mod/demod, comb filter, chroma
demod. S-Video skips comb filter. Direct skips cable model entirely.

**Shared intermediate buffers:** Ping-pong between two buffers for
sequential stages. Never need more than 2 intermediates + input + output.

### 10.3 Latency analysis

**Video latency:** Zero added. All processing happens within the frame
that generated the data. The frame is displayed at the same vsync it
would have been without processing.

**Audio latency:** One frame of buffering (16.67 ms) for the GPU
round-trip. This adds to the existing audio buffer latency:

```
Current:  APU cycle → filter → callback → ring buffer → AVAudioEngine
          Latency: ~1 buffer = 512-2048 samples = 11-46 ms

Proposed: APU cycle → raw buffer → GPU upload → chain process →
          readback → ring buffer → AVAudioEngine
          Latency: +1 frame = +16.67 ms

Total: ~28-63 ms (acceptable for non-rhythm games)
```

For latency-sensitive applications, the audio chain could fall back to
CPU processing (the existing filter chain) while video uses GPU.

---

## 11. Implementation Roadmap

### Phase 1: Kernel primitive library

Build and test the six kernel types as standalone Metal compute
functions. Verify correctness against CPU reference implementations.

**Deliverables:**
- `kernels/pointwise.metal` — generic transfer function kernel
- `kernels/rc_filter.metal` — prefix scan IIR kernel (the hard one)
- `kernels/fir.metal` — FIR convolution kernel
- `kernels/delay.metal` — delay line with scaling
- `kernels/modulator.metal` — carrier multiply
- `kernels/blur2d.metal` — separable 2D convolution
- Unit tests comparing GPU output to CPU reference for each kernel
- Verification: prefix scan RC matches sequential RC within float
  precision (< 1e-5 relative error)

### Phase 2: Audio analog chain on GPU

Port the existing CPU audio analog processing to GPU using the kernel
primitives. This is the cleanest greenfield — small buffers, simple
chain, easy to A/B test against CPU output.

**Deliverables:**
- Metal buffer upload for raw APU samples (per frame)
- GPU dispatch chain: DAC → mixer → coupling cap → amp → filters
- GPU readback of decimated output to audio ring buffer
- Bit-exact (within float precision) match with CPU path
- Toggle: CPU path vs GPU path (for comparison and fallback)

### Phase 3: Cable / transmission medium

Implement the distributed RC cable model and impedance reflection
model. Integrate into the audio chain first (simpler), then video.

**Deliverables:**
- `CableParams` struct with physical parameters
- N-section RC ladder dispatch (N prefix scans in sequence)
- Impedance mismatch ghost generation (delay + scale)
- Audio cable between console output amp and TV input
- Video cable between console output and TV input
- Connection type selector in UI

### Phase 4: Video signal chain on GPU

Move the composite encode→decode pipeline from CPU to GPU (or keep
CPU encode, move decode to GPU). This is the largest phase.

**Deliverables:**
- Upload composite waveform buffer to GPU
- Comb filter as delay + add kernel
- Chroma demod as modulator + FIR kernels
- Luma processing as FIR + pointwise kernels
- Matrix decode as pointwise kernel
- Integration with existing CRT phosphor renderer
- Verify visual match with CPU composite pipeline

### Phase 5: RF path

Add the full RF modulator/demodulator path as optional stages inserted
into the video and audio signal chains.

**Deliverables:**
- RF video modulator (AM + vestigial sideband)
- RF video demodulator (tuner + envelope detect + AGC)
- RF audio subcarrier (FM mod/demod at +4.5 MHz)
- RF noise model (thermal + interference)
- Toggle between RF/composite/S-Video/component/RGB paths

### Phase 6: Speaker model & TV audio

Add the TV-side audio processing and speaker simulation.

**Deliverables:**
- TV audio input stage (coupling + tone controls)
- Power amplifier model (class AB + clipping)
- Speaker resonance model (biquad chain)
- Cabinet coloration model
- Speaker presets (small TV, console TV, PVM, arcade, headphones)

### Phase 7: Physical preset system

Replace the current aesthetic-driven presets with physically-grounded
presets that configure entire signal chains.

**Deliverables:**
- `PhysicalPreset` struct with all chain parameters
- 8-12 historically accurate presets
- Preset selector in UI
- Per-component override controls (advanced panel)
- Smooth interpolation between presets (morphing)

### Phase 8: PAL integration

Extend the physical model to PAL signal characteristics.

**Deliverables:**
- PAL comb filter model (1H delay + V-phase averaging)
- PAL chroma demod (U/V extraction with alternating V phase)
- PAL-specific cable/TV parameters
- PAL speaker profiles (European TV characteristics)
- Region-aware preset variants

---

## 12. References

### Hardware documentation
- NesDev Wiki: APU Mixer — nonlinear formula, voltage tables, filter chain
- NesDev Wiki: PPU — 2C02 video signal generation
- Bisqwit: NES composite video signal analysis — waveform model
- LMP88959: PAL-CRT implementation — PAL encoder/decoder reference

### Signal processing
- Blelloch 1990: "Prefix Sums and Their Applications" — parallel scan algorithm
- GPU Gems 3, Chapter 39: "Parallel Prefix Sum (Scan) with CUDA"
- Smith: "The Scientist and Engineer's Guide to DSP" — FIR/IIR filter design
- Oppenheim & Willsky: "Signals and Systems" — sampling, aliasing, modulation

### CRT physics
- Monitors & Display Devices (Sherr) — phosphor characteristics, beam physics
- Television Engineering Handbook (Benson) — NTSC signal chain, TV receiver design
- Sony PVM technical manuals — aperture grille specifications, convergence

### Audio electronics
- Self: "Audio Power Amplifier Design" — class AB amplifier modeling
- Small: "Direct Radiator Loudspeaker System Analysis" (Thiele-Small) — speaker parameters
- NES hardware schematics — component values for filter networks

### Existing emulator implementations
- Mesen: composite video + audio filtering reference
- bsnes/higan: signal-level video processing
- Shay Green's blip_buf: band-limited synthesis for NES audio
