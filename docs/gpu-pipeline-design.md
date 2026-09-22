# From Palette Index to Phosphor Glow

**Historical design proposal.** This describes early intended stages and presets,
not the current implementation. In particular, its distributed cable ladder,
coupling-capacitor path, preset identities and claims of complete physical
modeling are superseded. Use the [current pipeline reference](gpu-pipeline-reference.md),
[model validation](architecture/gpu-realism-validation.md) and
[23-preset audit](https://yaglo.github.io/mynes-web/gallery/presets/) for implemented behavior and evidence.

## The Problem

Most CRT emulation in emulators is cosmetic. A fragment shader darkens scanlines, adds barrel distortion, maybe applies a phosphor mask texture. The result looks vaguely CRT-like but is immediately recognizable as "shader filter over a clean digital image" rather than a real CRT.

The difference: a real CRT processes an analog signal through 14 stages of electronics, each introducing characteristic artifacts. Chroma bleed, dot crawl, convergence error, rainbow shimmer -- these are not post-effects. They are consequences of the signal path. You cannot paint them on after the fact and have them look right, because they interact: the comb filter's imperfect Y/C separation produces cross-color that the chroma demodulator then phase-shifts, which the matrix decode maps to specific wrong colors that depend on the subcarrier phase at that pixel. No post-processing filter can reproduce this because it never had the composite signal to begin with.

## The Approach

Model the complete physical signal path from the 2C02 composite video pin to the CRT phosphor screen as a chain of GPU compute shaders. Each shader corresponds to a real electronic stage. The connection type (RF, Composite, S-Video, Component, RGB, Direct) selects which stages are active -- not as a quality slider, but because different cables physically bypass different electronics.

An S-Video cable carries luma and chroma on separate wires. That means no comb filter is needed to separate them -- the cable already did it. This is not "S-Video mode skips the comb filter for better quality." It is "the comb filter stage is physically absent from the signal path because there is no composite signal to separate."

## The 14-Stage Pipeline

### Stage 1: 2C02 DAC

The NES PPU outputs a 9-bit value on its composite video pin: 6-bit palette index + 3-bit emphasis. This shader converts that into the analog waveform the NES actually generates.

The implementation uses Bisqwit's voltage model. The PPU's output is not RGB -- it is a composite waveform whose shape encodes both luminance and chrominance simultaneously. Each NES pixel produces 8 waveform samples (NTSC) at 12-phase subcarrier resolution. The ratio 8/12 = 2/3 subcarrier cycles per pixel matches the real NTSC relationship.

The signal table is precomputed on CPU: 512 entries (64 palette values x 8 emphasis combinations) x 24 floats per entry (12 phases duplicated for safe boundary reads). The GPU shader looks up each pixel's entry and writes `samples_per_pixel` consecutive floats. Dispatch: 256 threads per workgroup (one per NES pixel), 240 workgroups (one per scanline).

### Stage 2: Console Output

The NES's own output circuitry. A 10 uF coupling capacitor (NES front-loader) or 100 uF (Famicom) blocks DC, implemented as a first-order RC highpass. An amplifier bandwidth limit at ~6 MHz rolls off high frequencies. Optional PSU hum injection adds mains interference from the console's power supply.

The coupling cap value matters. The Famicom's 100 uF cap has a corner frequency of ~0.16 Hz -- essentially transparent to audio. The NES front-loader's 10 uF cap cuts at ~1.6 Hz, which is inaudible for audio but measurable on test signals. These are the actual values from the NES motherboard schematic.

### Stage 3: Cable

A distributed RC transmission line modeled as a multi-section ladder network (2-8 sections depending on cable quality). The cable's parasitic capacitance forms a lowpass filter with the source impedance, softening high frequencies. Longer cables with higher capacitance produce more rolloff.

Parameters come from real cable datasheets:

| Cable Type | Capacitance | Typical Length | Sections |
|------------|-------------|----------------|----------|
| RG-59 coax (RF) | 67 pF/m | 1.5 m | 4 |
| Cheap RCA (Composite) | 80 pF/m | 2.0 m | 3 |
| Quality S-Video | 50-55 pF/m | 1.0 m | 2 |
| RGB SCART/BNC | 50 pF/m | 0.5 m | 2 |

Ghost reflections from impedance mismatches at connectors are modeled by the delay shader: a delayed, attenuated copy of the signal added back. Corroded connectors (connector resistance 3 ohms vs 0.05 for gold-plated) produce visible ghosts.

### Stage 4: RF Modulator (RF path only)

The NES RF output AM-modulates the composite signal onto a carrier -- channel 3 at 61.25 MHz or channel 4 at 67.25 MHz (Japanese Famicom uses channel 1 at 95.75 MHz). The demodulator recovers the composite signal, but the round-trip adds:

- White noise (RF snow) from the thermal noise floor
- 60 Hz hum from CRT oscillator coupling
- Bandwidth limiting from the RF channel (~4 MHz vs composite's ~4.2 MHz)

The noise floor ranges from -60 dBm (decent reception) to -44 dBm (the corroded-cable basement scenario). AGC with asymmetric attack/release (fast attack prevents signal overload, slow release prevents gain pumping) normalizes the amplitude per scanline.

### Stage 5: TV Input

The TV's own coupling capacitor and automatic gain control. AGC measures peak amplitude per scanline and applies a smoothed gain correction. The carry buffer persists the smoothed gain across frames so the envelope never resets -- just like a real TV's AGC circuit maintains state across fields.

### Stage 6: Comb Filter

Y/C separation exploiting NTSC subcarrier phase inversion. The 3.579545 MHz color subcarrier inverts phase every scanline. Subtracting adjacent scanlines cancels the (same-phase) luma and isolates the (opposite-phase) chroma.

Four modes model different TV hardware:

- **Bypass**: S-Video input, Y/C already separated at source
- **1-line**: Cheap TV. `Y = (signal[n] + signal[n-1H]) / 2`, `C = (signal[n] - signal[n-1H]) / 2`
- **2-line**: Decent TV. Uses current + 2-lines-ago (same phase) for cleaner luma
- **3-line**: PVM-grade. Averages 4 scanlines, chroma cancels over 2 complete subcarrier cycles

The comb filter is the primary source of composite video's characteristic artifacts. Without it (Direct/RGB modes), the signal is clean. With a 1-line comb, vertical detail creates cross-color (rainbow shimmer on horizontal stripes). The 3-line comb reduces this but introduces slight vertical softening.

### Stage 7: Chroma Demodulator

Quadrature I/Q demodulation. The separated chroma signal is multiplied by cos(wt) and sin(wt) to recover the I (orange-cyan) and Q (green-magenta) color difference signals. The modulator shader runs in IQ mode (mode 3): one input produces two outputs simultaneously.

Phase is reset per scanline to match the PPU's dot crawl cycle -- the subcarrier phase offset advances per scanline, and the demodulator must track this exactly for correct color decode. Followed by FIR bandwidth limiting (0.35-1.5 MHz depending on TV quality) to suppress the double-frequency residual from the multiplication.

### Stage 8: Luma Processing

FIR lowpass filtering on the luma signal (whether from comb filter output or raw composite). The bandwidth depends on connection type: RF gets 2.5-3.0 MHz (tuner-limited), composite gets 4.2-4.5 MHz (NTSC baseband), S-Video and above get 5.5-6.0 MHz (full bandwidth). Per-scanline boundary clamping prevents color bleeding across scanline boundaries.

### Stage 9: Matrix Decode

YIQ to RGB via a 3x3 matrix with per-channel bias. The matrix coefficients fold in the standard NTSC decode matrix, color temperature adjustment (D65 6500K standard, D93 9300K for Japanese TVs), and per-gun drive/cutoff.

The color killer is implemented here: a `smoothstep` suppression of I/Q when luma falls below a threshold. Real TVs do this to prevent color noise (random hue speckle) in dark picture areas where the chroma signal-to-noise ratio is poor. The Basement TV preset sets `color_killer` to 0.08 -- colors fade out in the darkest regions.

--- Signal domain ends. Display domain begins. ---

### Stage 10: Video Amplifier

Per-channel R/G/B bandwidth limiting via independent FIR filters. This models the TV's three separate amplifier circuits driving the three electron guns. Each gun can have different bandwidth: a cheap TV might have 5 MHz red and green but only 4.8 MHz blue (the blue amplifier was cheaper because the eye is least sensitive to blue detail).

Followed by gamma correction: 2.2 for calibrated PVMs, 2.4 for standard consumer CRTs, 2.5 for cheap sets with elevated gamma from aging.

### Stage 11: Electron Beam

The most complex stage. Converts signal-resolution RGB (2048x240) into display-resolution RGBA. This is where scanline structure, bloom, convergence error, and noise happen -- all in a single compute dispatch.

**Brightness-dependent Gaussian beam profile.** The electron beam is not a uniform line. It has a Gaussian cross-section whose width depends on beam current (brightness). Dark pixels produce a narrow beam (visible gaps between scanlines), bright pixels produce a wide beam that fills the inter-scanline gap and blooms into adjacent lines. The sigma interpolates between `sigma_narrow` (dark, 0.20 typical) and `sigma_wide` (bright, 0.70 typical) based on `pow(luma, bloom_gamma)`.

**Per-channel convergence error.** The three electron guns are physically separated. Aligning their beams across the entire screen is impossible -- there is always some residual offset, worst at the edges. Red and blue are shifted relative to green by `(conv_r_x, conv_r_y)` and `(conv_b_x, conv_b_y)` signal samples, scaled by `edge_factor = cx^2 + cy^2`. A well-calibrated PVM has offsets near zero. The Basement TV has `conv_r_x = 6.0, conv_b_x = -5.0` -- visible red/blue fringing on every edge.

**Horizontal timebase jitter.** Slow sinusoidal sway of the entire image, like a real CRT's horizontal oscillator drifting. Composed of two incommensurate sine waves plus a per-scanline variation, amplitude controlled by `h_jitter`.

**Mains hum bars.** Brightness modulation from AC power interference. Three harmonics: 60 Hz fundamental, 120 Hz (40% amplitude), 180 Hz (15% amplitude). The bar rolls slowly upward through the frame. `hum_bar_amplitude = 0` disables it; `0.08` produces the heavy rolling bands of a basement TV on a shared power strip.

**Per-pixel Gaussian noise.** Six independent Murmur3 hash streams per pixel generate uniform random values. Box-Muller transform converts them to Gaussian distribution (physically correct -- electronic noise is Gaussian, not uniform). Noise amplitude scales inversely with local luminance: more visible in shadows (where the signal-to-noise ratio is lowest).

**Edge focus degradation.** Real CRT beams defocus at screen edges due to longer throw distance and yoke astigmatism. `edge_focus` widens the beam sigma by `(1 + edge_factor * edge_focus)`.

**Velocity dimming.** The beam sweeps faster at screen edges (nonlinear deflection), depositing less phosphor energy per pixel. Separate from vignette, which models optical cos^4 falloff.

**Geometry warp.** Pincushion distortion plus S-correction, applied as horizontal jitter modulated by vertical position.

**RF interference.** Stepped vertical zigzag from electromagnetic interference, modeled as `floor(sin(sy * 0.45) * rf_interference + 0.5)` -- discrete pixel shifts, not smooth, matching the look of real RF interference.

### Stage 12: Phosphor Screen

Temporal blending between the current and previous frame's beam output. Per-channel phosphor persistence weights model P22 phosphor decay: green persists ~2x longer than blue (`persistence_g = 1.0`, `persistence_b = 0.65-0.82` depending on preset). This temporal blend also cancels NTSC dot crawl -- the subcarrier phase shifts by 180 degrees between frames, so averaging cancels the crawl pattern.

The `temporal_blit` shader reads two packed float16x4 buffers (current and previous beam output), blends with per-channel weights, and writes directly to a storage texture. No CPU roundtrip.

### Stage 13: CRT Glass

Halation: light scattering through the glass faceplate creates a soft glow around bright areas. Implemented as a two-pass separable Gaussian blur with brightness threshold extraction. Pass 1 extracts pixels above the threshold and blurs horizontally. Pass 2 blurs vertically. The result is additively blended with the main image.

Glass tint attenuates all light passing through the faceplate (0.70 for dirty glass, 0.90 for clean).

Barrel distortion: asymmetric horizontal/vertical curvature. The Wega's flat tube has `barrel = 0.0`. The Basement TV has `barrel = 0.05, barrel_v = 0.08` -- significantly more vertical curvature from the aged deflection yoke.

### Stage 14: Environment

Vignette: cos^4 radial falloff modeling the illumination angle. Ambient light: reflected room light that lifts the black floor (a dark room vs. a kitchen with fluorescent lighting). Display gamma and HDR/SDR tone mapping: Reinhard soft rolloff for SDR, linear pass-through for EDR.

## Connection Types

| Connection | Active Stages | What's Different |
|------------|---------------|-----------------|
| RF | All 14 | Full RF mod/demod, worst quality |
| Composite | 1-3, 5-14 | Skips RF, still needs comb filter |
| S-Video | 1-3, 5 (bypass comb), 7-14 | Y/C pre-separated, no comb needed |
| Component | 1-3, 5, 8-14 | No chroma mod/demod (baseband Cb/Cr) |
| RGB | 1-3, 10-14 | No color space conversion |
| Direct | 1-2, 10-14 | No cable, shortest path (test/reference) |

This is not a quality slider. It is what physically happens when you unplug the RF cable and connect composite instead. The RF modulator and TV tuner are literally not in the signal path. You cannot get RF artifacts from a composite connection any more than you can get wet from unplugged plumbing.

## The Signal Chain Runner

All dispatch boilerplate is handled by a generic, data-driven runner shared between video and audio chains.

```c
typedef struct {
    const char     *name;           // human-readable (for visualiser)
    ChainKernelType kernel_type;    // which compute shader
    bool            enabled;        // false = skip
    bool            bypass;         // user-toggled (visualiser B key)
    uint8_t         params[128];    // uniform data
    uint32_t        params_size;
    uint32_t        dispatch_x, dispatch_y, dispatch_z;
    double          timing_us;      // wall-clock dispatch time
    bool            needs_carry;    // RC filter: inter-block state
    bool            needs_taps;     // FIR: tap coefficient buffer
    bool            dual_output;    // modulator IQ: writes 2 buffers
} ChainStage;
```

The `SignalChain` holds an array of up to 32 stages, a pair of ping-pong GPU buffers, auxiliary buffers for multi-output stages, carry buffers for sequential filters, and FIR tap coefficient buffers. `chain_run()` iterates the array, dispatches each enabled stage, and ping-pongs the buffers automatically. The same runner handles both video (491,520 float samples, 2048x240) and audio (~29,829 float samples per frame).

Adding a stage is appending a struct. No C code changes. Hot-switching connection type is toggling stage enabled flags. The runner does not know or care what the stages do -- it dispatches kernel types and manages buffer routing.

## The Preset System

Presets are not "low / medium / high" quality settings. Each preset configures the entire signal chain to match a specific real-world NES setup. Every parameter is physically sourced.

**Living Room 1988.** NES front-loader, 2m cheap RCA composite (80 pF/m), 19" Zenith shadow mask. No comb filter. `gamma = 2.45`, `hue_offset = 2.0`, `noise_level = 0.035`. Lamp on in the living room (`ambient_light = 0.08`). Dad just bought it at Sears.

**Bedroom RF 1990.** NES through RF switch on channel 3, 1.5m coax (67 pF/m) to a 13" GE portable. `noise_floor_dbm = -55.0` (poor reception), `rf_interference = 0.8`, `beam_spot_size = 6.0`. Ghost reflection from the antenna switch box (`ghost_delay = 16, ghost_level = 0.06`). Heavy snow, tinny speaker.

**Studio PVM.** S-Video modded NES on a Sony PVM-20M4U. Aperture grille, calibrated D65 6500K. `beam_sharpness = 1.00`, `convergence_static = 0.02`, `gamma = 2.20`. Gold-plated mini-DIN connectors (`connector_resistance = 0.05`). Rock-solid timebase (`h_jitter = 0.0`). No hum, no noise.

**Famicom Kitchen.** Original Famicom, RF adapter, 3m coax to a small Japanese TV. `color_temperature = 9300.0` (D93, the Japanese standard). `console_coupling_C = 100e-6` (Famicom's 100 uF cap). Japanese channel 1 at 95.75 MHz. Kitchen fluorescent lighting (`ambient_light = 0.12`).

**Arcade Cabinet.** PlayChoice-10, 0.5m internal RGB harness to a 25" arcade monitor. `r_drive = 1.10, g_drive = 1.05` (arcade monitors run hot). Shadow mask, fine 0.28mm pitch. Shared cabinet PSU introduces hum (`console_psu_hum = 0.004`). Speaker driven into soft clip (`audio_saturation_drive = 2.0`).

**Late CRT (Wega).** NES composite on a 27" Sony KV-27FS120. 2-line comb filter, flat tube (`barrel = 0.0`), aperture grille 0.25mm pitch. The best composite ever got on a consumer set.

**Retro Gaming Setup.** S-Video modded NES on PVM with headphones. The modern purist configuration. Headphones bypass the speaker model entirely for flat response. Dark room, no ambient light.

**Basement TV.** 3m corroded coax (`connector_resistance = 3.0, shield_effectiveness = 0.45`), ancient 19" TV with bad convergence (`conv_r_x = 6.0, conv_b_x = -5.0`), heavy barrel distortion, visible ghosts (`ghost_delay = 32, ghost_level = 0.12`). `noise_floor_dbm = -44.0` -- heavy snow. `noise_level = 0.080`. `hum_bar_amplitude = 0.08`. Worn phosphors (`persistence_ms = 5.0`). Everything is bad. It is perfect.

**Stas's Favourite.** Basement aesthetic with a clean signal -- sharp beam, no ringing, no ghosts, neutral hue. The "I like the vibe but want to see the game" preset.

## The Audio Chain

The same `SignalChain` runner processes audio through 10 stages:

1. **Coupling capacitor** -- RC high-pass DC blocking (10 uF NES, 100 uF Famicom, 47 uF Dendy)
2. **Feedback network** -- RC high-pass bass shaping (~440 Hz corner)
3. **Amplifier bandwidth** -- RC low-pass (9.6 kHz Famicom, 14.1 kHz NES front-loader)
4. **Amplifier saturation** -- tanh soft-clip (drive 1.0 = linear, 4.0 = heavy)
5. **PSU hum injection** -- additive 60 Hz + harmonics
6. **Noise floor** -- additive white noise (xorshift32)
7. **Cable capacitance** -- RC low-pass (length-dependent)
8. **TV input coupling** -- RC high-pass
9. **Speaker model** -- biquad resonance peak + high-frequency rolloff (350 Hz resonance for small TV, 20 Hz for headphones)
10. **Decimation** -- FIR filter, 1.79 MHz to 48 kHz

Console variants have different component values from the actual hardware schematics. The Famicom's 100 uF coupling cap (corner frequency 0.16 Hz) passes more low-frequency content than the NES front-loader's 10 uF (1.6 Hz). The Dendy's 47 uF splits the difference at 0.34 Hz. Speaker types range from a small TV (350 Hz resonance, 400-6000 Hz bandwidth, Q = 2.0) to headphones (20 Hz resonance, 20-20000 Hz, Q = 0.7 -- essentially flat).

## Debug Infrastructure

**DebugTap.** GPU buffer readback at any stage. Each tap point captures the float buffer contents after a specific signal processing stage, along with metadata (frame number, timing, stage parameters). Zero cost when disabled. The SwiftUI visualiser enables tap points it wants to monitor; the emulator captures them after each `chain_run()`.

**ChainVis.** A real-time overlay (toggled with Ctrl+L) showing both video and audio signal chains in a two-column layout. Per-stage timing, enabled/bypassed state, and keyboard navigation to toggle bypass on individual stages (B key).

**Debug Server.** A Unix domain socket server that the SwiftUI visualiser app connects to. Binary protocol with message types: snapshot (per-stage timing), parameter update, preset change, tap request, and tap data. The server listens on a background thread; all message processing happens on the emulator's main thread via `debug_server_frame()` to avoid threading issues.

## What Makes This Different

**CRT Royale / CRT Geom.** Fragment shader post-effects. No signal processing. The scanlines are uniform darkening bands, not brightness-dependent beam profiles. No composite artifacts (dot crawl, chroma bleed, rainbow shimmer) because there is no composite signal to decode. The "CRT look" without the CRT physics.

**Blargg's nes_ntsc.** CPU-side NTSC encode/decode in a single pass. Good cross-color and dot crawl results. But single-pass: no comb filter modes, no RF simulation, no per-channel convergence, no temporal effects, no display-domain modeling. A signal processor without a display.

**NTSC-CRT (LMP88959).** Authentic composite encode/decode. CPU-only, single resolution. This project vendors the PAL version. No GPU acceleration, no display-domain modeling. A reference implementation, not a real-time pipeline.

**This project.** Full pipeline from DAC to glass, each stage a separate GPU compute kernel, configurable by connection type, with physically-sourced parameters. The artifacts are not painted on. They emerge from the signal processing. The dot crawl exists because the subcarrier phase advances between frames. The chroma bleed exists because the FIR bandwidth limit at 1 MHz allows low-frequency I/Q components to spread horizontally. The convergence fringing exists because the red and blue beams are offset from green. None of these effects are special-cased. They are consequences of the math.

## Trade-offs

- **Requires SDL3 GPU** (Vulkan/Metal/D3D12). No OpenGL fallback. The compute shader model does not exist in OpenGL ES or WebGL.
- **~14 compute dispatches + 3 render passes per frame.** Needs discrete GPU or modern integrated. An M1 handles it comfortably. Integrated Intel from 2015 will struggle.
- **SPIR-V shaders must be compiled offline** (`glslc`) then converted to MSL for Metal. No runtime shader compilation.
- **The presets are opinionated.** "Living Room 1988" is a specific TV, not a generic "composite" mode. If you want a different 1988 living room, you edit the physical parameters.
- **Parameters are physical values** (pF/m, MHz, ohms), not perceptual sliders. Harder to tune by hand. Easier to verify against datasheets.
