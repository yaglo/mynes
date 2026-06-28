# GPU Pipeline Technical Reference

Reference documentation for the GPU composite video pipeline. Covers shader interfaces, signal chain runner API, video chain configuration, presets, audio chain, and debug infrastructure.

Source files are in `frontends/gpu/`. Shaders are in `frontends/gpu/shaders/compute/` and `frontends/gpu/shaders/render/`.

## Shader Library

### Compute Shaders

All compute shaders are compiled from GLSL 450 to SPIR-V (via `glslc`) and cross-compiled to MSL for Metal. Each shader has three variants: `.comp.glsl` (source), `.comp.spv` (SPIR-V), `.comp.msl` (Metal).

---

#### `dac_2c02.comp` -- 2C02 Video DAC (Stage 1)

Converts 256x240 palette+emphasis indices into composite waveform voltages.

**Input buffers:**
- `binding 0` (readonly): `IndexBuf` -- uint16 palette+emphasis indices packed as uint32 pairs (2 pixels per uint32). Bits 0..5 = palette index, bits 6..8 = emphasis.
- `binding 1` (readonly): `SignalTable` -- precomputed signal table. 512 entries x 24 floats. Entry index = `(emph << 6) | palette_idx`. 24 floats = 12 phases duplicated for wraparound safety.

**Output buffer:**
- `binding 0` (writeonly): `WaveformBuf` -- `samples_per_line * 240` floats. Value range [0.0, 1.0].

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `samples_per_pixel` | uint | 8 (NTSC) or 10 (PAL) |
| `samples_per_line` | uint | 2048 (NTSC) or 2560 (PAL) |
| `phase_base` | uint | Starting subcarrier phase for this frame |
| `phase_line_adv` | uint | Phase advance per scanline |
| `phase_field_adv` | uint | Phase advance per frame (CPU-tracked) |
| `frame_field` | uint | Field index within dot crawl cycle |

**Dispatch:** 1 workgroup of 256 threads x 240 workgroups. Each thread handles one NES pixel. Total: 61,440 threads.

**Algorithm:** Read packed uint16 index, compute per-pixel subcarrier phase from scanline and pixel position, index into signal table at `entry * 24 + pixel_phase`, write `samples_per_pixel` consecutive floats.

---

#### `pointwise.comp` -- Per-Sample Transfer Functions (Stages 2, 4, 5)

Six modes selected by uniform:

| Mode | Function | param_a | param_b | param_c |
|------|----------|---------|---------|---------|
| 0 | `y = gain * x + offset` | gain | offset | -- |
| 1 | `y = tanh(x * drive) / tanh(drive)` | drive | -- | -- |
| 2 | `y = clamp(x, lo, hi)` | lo | hi | -- |
| 3 | `y = sign(x) * pow(abs(x), exp)` | exponent | -- | -- |
| 4 | `y = x + k * (x^3 - x)` | k | -- | -- |
| 5 | `y = x + amp * sin(phase + n * dp)` | amplitude | phase | dp |

**I/O:** Reads `binding 0` (readwrite), writes `binding 1` (readwrite). Same buffer allowed for in-place.

**Uniforms:** `count` (uint), `mode` (uint), `param_a`, `param_b`, `param_c` (float).

**Dispatch:** `ceil(count / 256)` workgroups x 1. Workgroup size 256.

---

#### `rc_filter.comp` -- First-Order IIR Filter (Stages 2, 3, 5, 7, 8)

Sequential per-scanline: `y[n] = a * y[n-1] + b * x[n]`.

Each thread processes one complete scanline (2048 samples) sequentially. 240 scanlines are dispatched in parallel. In-place operation on `buf[src]`.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `a` | float | IIR feedback coefficient (exp(-2pi*fc/fs)) |
| `b` | float | Input coefficient (1 - a) |
| `total_count` | uint | Total samples in buffer |
| `block_offset` | uint | Unused in sequential mode |
| `samples_per_line` | uint | 2048 (NTSC) or 2560 (PAL) |
| `num_lines` | uint | 240 |

**Dispatch:** `ceil(num_lines / 256)` workgroups x 1. Warm-start: `y[-1] = x[0]`.

**Buffer routing:** In-place on `buf[src]`, carry buffer bound but unused in sequential mode. `dst = src` (no ping-pong swap).

---

#### `fir.comp` -- FIR Convolution (Stages 7, 8, 10)

`y[n] = sum(h[k] * x[n-k])` for k = 0..tap_count-1.

Supports decimation (output[n] = FIR(input[n * decimation_ratio])). Per-scanline boundary clamping with mirror extension prevents color bleeding.

**Input buffers:**
- `binding 0` (readonly): Input signal
- `binding 1` (readonly): FIR tap coefficients

**Output buffer:**
- `binding 0` (writeonly): Filtered signal

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `input_count` | uint | Number of input samples |
| `output_count` | uint | Number of output samples |
| `tap_count` | uint | Number of FIR taps (odd) |
| `decimation_ratio` | uint | 1 = no decimation |
| `samples_per_line` | uint | 0 = no per-line boundary, >0 = clamp per scanline |

**Dispatch:** `ceil(output_count / 256)` workgroups x 1. Max taps: 64 (`VIDEO_GPU_MAX_FIR_TAPS`).

---

#### `delay.comp` -- Delay Line (Stage 3)

Four modes:

| Mode | Function | Description |
|------|----------|-------------|
| 0 | `y[n] = x[n - D]` | Pure delay |
| 1 | `y[n] = x[n] + level * x[n - D]` | Ghost (single echo) |
| 2 | `y[n] = (curr[n] + prev[n]) * scale` | Comb add |
| 3 | `y[n] = (curr[n] - prev[n]) * scale` | Comb subtract |

**Uniforms:** `count` (uint), `mode` (uint), `delay_samples` (int), `level` (float).

**Dispatch:** `ceil(count / 256)` workgroups x 1.

---

#### `comb_filter.comp` -- Y/C Separator (Stage 6)

Separates composite signal into luma (Y) and chroma (C) using inter-scanline phase relationships.

**Input:** `binding 0` (readonly): Composite signal.
**Output:** `binding 0` (writeonly): Y buffer. `binding 1` (writeonly): C buffer.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `count` | uint | Total samples |
| `samples_per_line` | uint | 2048 (NTSC) or 2560 (PAL) |
| `mode` | uint | 0=bypass, 1=1-line, 2=2-line, 3=3-line |
| `blend` | float | Comb strength (0..1) |

**Comb algorithms:**

- Mode 1: `Y = (sig + sig[-1H]) / 2`, `C = blend * (sig - sig[-1H]) / 2`
- Mode 2: `Y = (sig + sig[-2H]) / 2`, `C = blend * (sig - Y)`
- Mode 3: `Y = (sig + sig[-1H] + sig[-2H] + sig[-3H]) / 4`, `C = blend * (sig - Y)`

First scanline(s) zero-pad (no prior data).

**Buffer routing:** Input is readonly `buf[src]`, outputs are readwrite `buf[dst]` (Y) and `aux[0]` (C).

**Dispatch:** `ceil(count / 256)` workgroups x 1.

---

#### `modulator.comp` -- Chroma Modulator/Demodulator (Stage 7)

Four modes:

| Mode | Function |
|------|----------|
| 0 | `y[n] = x[n] * cos(phase + n * dp)` |
| 1 | `y[n] = x[n] * sin(phase + n * dp)` |
| 2 | `y[n] = (1 + mod_index * x[n]) * cos(phase + n * dp)` |
| 3 | `I[n] = x[n] * gain * cos(p)`, `Q[n] = x[n] * gain * sin(p)` (dual output) |

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `count` | uint | Number of samples |
| `mode` | uint | 0=cos, 1=sin, 2=AM, 3=IQ |
| `phase` | float | Base phase (radians) |
| `dp` | float | Phase increment per sample = 2*pi*Fsc/Fsample |
| `param_a` | float | mod_index (mode 2) or gain (mode 3) |
| `samples_per_line` | uint | >0 = reset phase per scanline |
| `line_phase_inc` | float | Phase advance per scanline (radians) |

Per-scanline phase reset: when `samples_per_line > 0`, phase is computed as `phase + scanline * line_phase_inc + sample_in_line * dp` for dot crawl accuracy.

**Buffer routing:** Mode 3 (IQ): input readonly, `data_out` = I channel (readwrite), `data_out2` = Q channel (readwrite).

---

#### `matrix_decode.comp` -- YIQ to RGB (Stage 9)

3x3 matrix multiply with per-channel bias and color killer.

**Input:** 3 readonly buffers (Y, I, Q at signal resolution).
**Output:** 1 writeonly buffer (interleaved R,G,B,R,G,B,...).

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `count` | uint | Total samples |
| `m00..m22` | float | 3x3 decode matrix (row-major, R/G/B x Y/I/Q) |
| `bias_r, bias_g, bias_b` | float | Per-channel bias |
| `color_killer_threshold` | float | 0 = disabled, 0.05-0.10 = suppress chroma below luma threshold |

Color killer: `kill = smoothstep(threshold * 0.7, threshold * 1.3, Y)`. I and Q are multiplied by `kill`.

Output is clamped to [0, 1] for SDR.

---

#### `video_amp.comp` -- Per-Channel RGB Bandwidth (Stage 10)

Independent horizontal FIR lowpass on each R, G, B channel.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `total_pixels` | uint | samples_per_line * 240 |
| `samples_per_line` | uint | 2048 (NTSC) |
| `tap_count` | uint | Number of FIR taps (odd, max 9) |
| `taps[12]` | float | FIR coefficients (padded to vec4 alignment) |

Per-scanline boundary mirroring. One thread per pixel; each thread applies the same FIR independently to R, G, B at that position.

---

#### `h_blur_rgb.comp` -- Horizontal Gaussian Blur (Stage 11 pre-pass)

Pre-blurs the signal-resolution RGB buffer to eliminate the expensive h-blur inner loop from the beam profile shader.

**Uniforms:** `signal_w` (uint), `num_lines` (uint), `sigma` (float, blur sigma in signal samples).

Kernel radius: `ceil(sigma * 3.0)`, clamped to max 24. Per-scanline boundary clamping.

**Dispatch:** `ceil(signal_w / 256)` x 240 x 1.

---

#### `beam_profile.comp` -- Electron Beam (Stage 11)

Signal-to-display resolution conversion with full CRT beam physics. 16x16 workgroups for 2D dispatch.

**Input:** Interleaved RGB floats (readonly). Per-sample: `rgb_in[sample * 3 + channel]`.
**Output:** Packed float16x4 as two uint32 per pixel: `rgba_out[pixel*2+0] = packHalf2x16(R, G)`, `rgba_out[pixel*2+1] = packHalf2x16(B, 1.0)`.

**Uniforms (21 fields, ~84 bytes):**

| Field | Type | Typical Range | Description |
|-------|------|---------------|-------------|
| `signal_w` | uint | 2048 | Samples per scanline |
| `out_w` | uint | 1170 | Output width |
| `out_h` | uint | 960 | Output height |
| `rows_per_scanline` | uint | 4 | Output rows per NES scanline |
| `sigma_narrow` | float | 0.20 | Beam sigma for dark pixels |
| `sigma_wide` | float | 0.70 | Beam sigma for bright pixels |
| `v_jitter_amp` | float | 0.0-0.005 | Vertical jitter amplitude in output rows |
| `black_floor` | float | 0.005-0.04 | Minimum output level |
| `conv_r_x` | float | -6.0 to 6.0 | Red horizontal convergence offset |
| `conv_r_y` | float | -2.5 to 2.5 | Red vertical convergence offset |
| `conv_b_x` | float | -6.0 to 6.0 | Blue horizontal convergence offset |
| `conv_b_y` | float | -2.5 to 2.5 | Blue vertical convergence offset |
| `noise_level` | float | 0.0-0.08 | Per-pixel Gaussian noise amplitude |
| `h_jitter` | float | 0.0-0.8 | Horizontal timebase jitter |
| `rf_interference` | float | 0.0-1.5 | RF stepped zigzag amplitude |
| `geometry_warp` | float | 0.0-3.5 | Pincushion + S-correction strength |
| `frame_counter` | uint | 0+ | For noise variation and hum phase |
| `hum_bar_amplitude` | float | 0.0-0.08 | Mains hum bar strength |
| `bloom_gamma` | float | 1.0-1.8 | Bloom nonlinearity (pow(luma, gamma)) |
| `edge_focus` | float | 0.0-0.40 | Focus degradation at edges |
| `velocity_dim` | float | 0.0-0.18 | Edge dimming from beam velocity |

**Algorithm:** For each output pixel: compute scanline index and sub-row position, apply jitter and geometry warp to horizontal position, compute per-channel convergence offsets scaled by `edge_factor`, composite beams from 3 adjacent scanlines with brightness-dependent Gaussian profiles, apply velocity dimming, hum bars (3 harmonics), Gaussian noise (Murmur3 hash + Box-Muller), and black floor.

**Dispatch:** `ceil(out_w / 16)` x `ceil(out_h / 16)` x 1.

---

#### `temporal_blit.comp` -- Phosphor Persistence (Stage 12)

Blends current and previous beam output with per-channel phosphor decay weights. Writes directly to a storage texture.

**Input:** Two readonly buffers (current and previous frame, packed float16x4 as uint32 pairs).
**Output:** `image2D out_tex` (rgba16f storage texture).

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `width` | uint | Output width |
| `height` | uint | Output height |
| `blend_factor` | float | Base blend (0.0-0.5). 0 = no temporal blend, 0.5 = full dot crawl cancellation |
| `decay_r` | float | Red persistence weight (0.80-0.90 typical) |
| `decay_g` | float | Green persistence weight (1.0 = reference) |
| `decay_b` | float | Blue persistence weight (0.65-0.82 typical) |

Per-channel blend: `bf_channel = blend_factor * decay_channel`. Green persists longest (P22 phosphor), blue decays fastest.

**Dispatch:** `ceil(width / 16)` x `ceil(height / 16)` x 1.

---

#### `rf_mod_demod.comp` -- RF Channel Simulation (Stage 4)

In-place RF degradation: adds noise (snow) and 60 Hz hum. RF bandwidth limiting is handled by a separate FIR stage in the signal chain, not this shader.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `count` | uint | Total samples |
| `samples_per_line` | uint | 2048 (NTSC) |
| `noise_amplitude` | float | RF snow (0.02-0.05 typical) |
| `hum_amplitude` | float | 60 Hz hum (0.01-0.03) |

Noise: LCG-style PRNG. Hum: 60 Hz sine at `dp = 2*pi / 715909.0` (Fsc * 12 / 60).

**Buffer routing:** In-place on `buf[src]`. `dst = src`.

---

#### `agc.comp` -- Automatic Gain Control (Stage 5)

Sequential per-scanline gain normalization.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `total_count` | uint | Total samples |
| `samples_per_line` | uint | 2048 (NTSC) |
| `num_lines` | uint | 240 |
| `target_level` | float | Desired peak amplitude (0.85-0.95) |
| `attack_coeff` | float | Gain decrease smoothing (0.1 = fast) |
| `release_coeff` | float | Gain increase smoothing (0.02 = slow) |
| `min_gain` | float | Floor (0.5) |
| `max_gain` | float | Ceiling (2.0) |

Three passes per scanline: (1) find peak absolute value, (2) compute smoothed gain with asymmetric envelope, (3) apply gain. Carry buffer persists smoothed gain across frames.

**Buffer routing:** In-place on `buf[src]`, carry buffer for per-scanline gain state.

**Dispatch:** `ceil(num_lines / 256)` workgroups x 1.

---

### Fragment Shaders

#### `crt_display.frag` -- CRT Display (Stages 13-14)

Full-screen fragment shader for display-domain effects.

**Textures:**
- `tex_composite` (sampler2D): Beam/temporal output texture
- `tex_halation` (sampler2D): Blurred bright areas

**Uniforms (DisplayParams):**

| Field | Type | Description |
|-------|------|-------------|
| `src_size` | vec2 | Composite texture dimensions (1170, 960) |
| `out_size` | vec2 | Display viewport dimensions |
| `barrel` | float | Horizontal curvature (0-0.05) |
| `barrel_v` | float | Vertical curvature (0 = same as barrel) |
| `convergence_static` | float | Fixed R/B horizontal offset |
| `convergence_dynamic` | float | Edge-dependent convergence |
| `mask_strength` | float | Phosphor mask modulation depth (0-1) |
| `mask_type` | int | 0=shadow, 1=aperture grille, 2=slot |
| `mask_pitch_pixels` | float | Mask pitch in display pixels |
| `halation_strength` | float | Halation blend amount |
| `vignette_strength` | float | Corner darkening (0-0.3) |
| `gamma` | float | >0.1: EDR mode (sRGB->linear), 0: SDR Reinhard |
| `black_floor` | float | Minimum black level |
| `ambient_light` | float | Reflected room light |
| `glass_tint` | float | Glass transmittance multiplier |
| `hdr_gain` | float | Output multiplier (compensate mask darkening) |
| `subpixel_layout` | int | 0=none, 1=RGB stripe, 2=BGR stripe |

**Processing order:** Barrel distortion, convergence sampling, phosphor mask (beam spot convolution over 5x5 grid), halation blend, glass tint, vignette (cos^4), ambient light, HDR gain, tone mapping (Reinhard for SDR, pow for EDR).

**Phosphor mask types:**
- Shadow mask: hexagonal dot triads with `smoothstep` circular dots
- Aperture grille: vertical RGB stripes (Trinitron), no Y structure
- Slot mask: rectangular RGB groups with half-triad row stagger

---

#### `halation_blur.frag` -- Halation Blur

Two-pass separable Gaussian with brightness threshold extraction.

**Uniforms:**

| Field | Type | Description |
|-------|------|-------------|
| `direction` | vec2 | (1/w, 0) for horizontal, (0, 1/h) for vertical |
| `radius` | int | Kernel radius in texels (12-32) |
| `threshold` | float | Brightness threshold for extraction |
| `do_threshold` | int | 1 = extract bright pixels (pass 1 only) |

Sigma = radius * 0.4. Symmetric taps (halves exp() calls). Luma extraction: `max(luma - threshold, 0) / (1 - threshold)`.

---

#### `fullscreen.vert` -- Fullscreen Triangle

Attribute-less fullscreen triangle. 3 vertices, no VBO.

Vertex positions: `(-1,-1)`, `(3,-1)`, `(-1,3)`. UV flipped so texture row 0 = top.

Dispatch: `draw(3, 1, 0, 0)` with no vertex buffers bound.

---

## Signal Chain Runner

### `ChainStage` Struct

```c
typedef struct {
    const char     *name;            // human-readable name
    ChainKernelType kernel_type;     // which compute shader
    bool            enabled;         // false = skip
    bool            bypass;          // user-toggled bypass
    uint8_t         params[128];     // uniform data (CHAIN_MAX_UNIFORM_SIZE)
    uint32_t        params_size;     // actual size in bytes
    uint32_t        dispatch_x;      // workgroups in X
    uint32_t        dispatch_y;      // workgroups in Y
    uint32_t        dispatch_z;      // workgroups in Z (always 1)
    double          timing_us;       // wall-clock dispatch time
    double          timing_avg_us;   // exponential moving average
    bool            needs_carry;     // RC filter: uses carry buffer
    bool            needs_taps;      // FIR: uses tap coefficient buffer
    int             taps_index;      // which tap buffer (0=Y, 1=C)
    bool            dual_output;     // modulator IQ: writes 2 buffers
    bool            reads_secondary; // delay comb: reads from secondary input
} ChainStage;
```

### `SignalChain` Struct

```c
typedef struct {
    ChainStage stages[32];           // CHAIN_MAX_STAGES
    int        num_stages;

    GpuPipeline pipelines[14];       // CHAIN_KERNEL_COUNT
    bool        pipeline_loaded[14];

    SDL_GPUBuffer *buf[2];           // ping-pong signal buffers
    int            current_buf;      // 0 or 1

    SDL_GPUBuffer *aux[4];           // CHAIN_MAX_AUX_BUFS (dual-output)
    SDL_GPUBuffer *carry_buf;        // RC prefix scan inter-block state
    SDL_GPUBuffer *tap_bufs[4];      // CHAIN_MAX_TAP_BUFS (FIR coefficients)
    int            num_tap_bufs;

    int            sample_count;
    int            sample_rate;
    int            samples_per_line;
} SignalChain;
```

### Buffer Management

**Ping-pong buffers.** Two identically-sized GPU buffers (`buf[0]` and `buf[1]`). Each stage reads from `buf[current_buf]` (src) and writes to `buf[1 - current_buf]` (dst). After dispatch, `current_buf` flips. Exceptions:

- **In-place kernels** (RC, RF, AGC): `dst = src`, no flip. The shader reads and writes the same buffer.
- **Dual-output kernels** (comb, modulator IQ): primary output to `buf[dst]`, secondary to `aux[0]`.

**Auxiliary buffers.** 4 buffers at the same size as ping-pong buffers. Used for:
- Comb filter: `aux[0]` = chroma output (luma goes to `buf[dst]`)
- Modulator IQ: `aux[0]` = Q channel (I goes to `buf[dst]`)
- Matrix decode: reads from `aux[0]` (I) and `aux[1]` (Q) as readonly inputs

**Carry buffer.** Sized for `max_blocks * 2 * sizeof(float)`. Used by RC filter and AGC for inter-block/inter-frame state persistence.

**Tap buffers.** Up to 4 FIR coefficient buffers, uploaded once via `chain_upload_taps()`. Each FIR stage references a tap buffer by index (`taps_index`).

### API

```c
// Initialize: allocate buffers, pre-load all shader pipelines
bool chain_init(SignalChain *chain, SDL_GPUDevice *gpu,
                int sample_count, const char *shader_dir);

// Add a stage. Returns stage index or -1.
int chain_add_stage(SignalChain *chain, const char *name,
                    ChainKernelType kernel_type,
                    const void *params, uint32_t params_size,
                    uint32_t dispatch_x, uint32_t dispatch_y);

// Upload FIR tap coefficients. Returns tap buffer index.
int chain_upload_taps(SignalChain *chain, SDL_GPUDevice *gpu,
                      const float *taps, int num_taps);

// Upload input data to buf[0], reset current_buf to 0.
bool chain_upload_input(SignalChain *chain, SDL_GPUDevice *gpu,
                        const void *data, uint32_t size);

// Run all enabled stages. Result in buf[current_buf].
bool chain_run(SignalChain *chain, SDL_GPUDevice *gpu);

// Record stages into an external command buffer (caller manages fence).
bool chain_run_cmd(SignalChain *chain, SDL_GPUCommandBuffer *cmd);

// Download output to CPU (blocking).
bool chain_download_output(SignalChain *chain, SDL_GPUDevice *gpu,
                            void *out, uint32_t size);

// Runtime control
void chain_set_stage_enabled(SignalChain *chain, int index, bool enabled);
void chain_set_stage_bypass(SignalChain *chain, int index, bool bypass);
void chain_update_params(SignalChain *chain, int index,
                          const void *params, uint32_t size);
```

### Dispatch Routing per Kernel Type

| Kernel | Buffer Routing | Readwrite Bindings | Readonly Bindings |
|--------|---------------|-------------------|-------------------|
| POINTWISE | src -> dst | buf[src], buf[dst] | -- |
| RC_FILTER | in-place | buf[src], carry_buf | -- |
| FIR | src -> dst | buf[dst] | buf[src], tap_buf |
| DELAY | src -> dst | buf[dst] | buf[src], aux[0] |
| COMB | src -> dst + aux | buf[dst], aux[0] | buf[src] |
| MODULATOR | src -> dst + aux | buf[dst], aux[0] | buf[src] |
| DAC | indices -> waveform | buf[dst] | buf_indices, signal_table |
| MATRIX | Y,I,Q -> RGB | buf[dst] | buf[src], aux[0], aux[1] |
| BEAM | RGB -> RGBA | buf[dst] | buf[src] |
| RF | in-place | buf[src] | -- |
| VIDEO_AMP | src -> dst | buf[dst] | buf[src] |
| H_BLUR_RGB | in-place (2 rw) | buf[src], buf[dst] | -- |
| TEMPORAL_BLIT | buffers -> texture | -- (writes storage tex) | buf[cur], buf[prev] |
| AGC | in-place | buf[src], carry_buf | -- |

---

## Video Chain Configuration

### `VideoConnectionType` Enum

```c
typedef enum {
    VIDEO_CONN_RF = 0,       // NES -> RF modulator -> coax -> TV tuner
    VIDEO_CONN_COMPOSITE,    // NES -> RCA cable -> TV composite input
    VIDEO_CONN_SVIDEO,       // NES (mod) -> S-Video cable -> TV
    VIDEO_CONN_COMPONENT,    // NES (mod) -> component cables -> TV
    VIDEO_CONN_RGB,          // NES (mod) -> RGB SCART -> TV
    VIDEO_CONN_DIRECT,       // No cable (test/reference mode)
} VideoConnectionType;
```

### `CableParams` Struct

| Field | Type | Typical Values | Description |
|-------|------|----------------|-------------|
| `length_meters` | float | 0.5-3.0 | Physical cable length |
| `resistance_per_m` | float | 0.08-0.25 | Series resistance (ohm/m) |
| `capacitance_per_m` | float | 50e-12 to 80e-12 | Shunt capacitance (F/m) |
| `num_sections` | int | 2-8 | RC ladder approximation order |
| `connector_resistance` | float | 0.02-3.0 | Total contact resistance at both ends (ohm) |
| `impedance` | float | 75.0 | Characteristic impedance (ohm) |
| `shield_effectiveness` | float | 0.45-0.97 | EMI shielding (0=none, 1=perfect) |
| `ghost_delay` | int | 0-32 | Impedance reflection delay (signal samples) |
| `ghost_level` | float | 0.0-0.12 | Ghost amplitude |

### `TVDisplayParams` Struct

Grouped by pipeline stage:

**Chroma demodulator (Stage 7):**
`chroma_bandwidth` (0.35e6-1.5e6 Hz), `luma_bandwidth` (2.5e6-6.0e6 Hz), `hue_offset` (degrees), `saturation` (multiplier), `fir_ringing` (0=Hamming, 1=rect), `rf_interference` (0-1.5), `geometry_warp` (0-3.5).

**Matrix decode (Stage 9):**
`color_temperature` (3200-9300 K), `r_drive`/`g_drive`/`b_drive` (0.8-1.2), `r_cutoff`/`g_cutoff`/`b_cutoff` (+/-0.05), `color_killer` (0-0.08).

**Video amplifier (Stage 10):**
`r_bandwidth`/`g_bandwidth`/`b_bandwidth` (4.2e6-7.5e6 Hz), `gamma` (2.2-2.5).

**Beam (Stage 11):**
`beam_sharpness` (0.0-1.0), `beam_height_min`/`beam_height_max` (0.55-1.36), `beam_spot_size` (3.0-8.0), `convergence_static` (0.02-0.40), `convergence_dynamic` (0.01-0.25), `conv_r_x`/`conv_r_y`/`conv_b_x`/`conv_b_y` (-6.0 to 6.0), `h_jitter` (0.0-0.8), `v_jitter` (0.0-0.005), `bloom_gamma` (1.0-1.8), `edge_focus` (0.0-0.40), `velocity_dim` (0.0-0.18).

**Phosphor (Stage 12):**
`mask_type` (enum: shadow/aperture_grille/slot), `mask_pitch_mm` (2.0-5.0), `mask_strength` (0.0-1.0), `subpixel_layout` (0/1/2), `persistence_ms` (1.0-5.0), `persistence_r`/`persistence_g`/`persistence_b` (0.65-1.0).

**Glass (Stage 13):**
`halation` (0.0-0.25), `glass_tint` (0.70-0.90), `barrel` (0.0-0.05), `barrel_v` (0.0-0.08).

**Environment (Stage 14):**
`vignette` (0.0-0.18), `ambient_light` (0.0-0.12), `black_floor` (0.005-0.05), `noise_level` (0.0-0.08), `hum_bar_amplitude` (0.0-0.08), `hdr_gain` (0.0-2.0).

### Stage Activation Logic (`video_chain_stage_active`)

| Stage | RF | Composite | S-Video | Component | RGB | Direct |
|-------|:--:|:---------:|:-------:|:---------:|:---:|:------:|
| 1 DAC | Y | Y | Y | Y | Y | Y |
| 2 Console | Y | Y | Y | Y | Y | Y |
| 3 Cable | Y | Y | Y | Y | Y | -- |
| 4 RF | Y | -- | -- | -- | -- | -- |
| 5 TV Input | Y | Y | Y | Y | Y | -- |
| 6 Comb | Y | Y | Y (bypass) | -- | -- | -- |
| 7 Chroma Demod | Y | Y | Y | -- | -- | -- |
| 8 Luma | Y | Y | Y | Y | Y | Y |
| 9 Matrix | Y | Y | Y | Y | -- | -- |
| 10-14 Display | Y | Y | Y | Y | Y | Y |

---

## Preset System

### `PhysicalPreset` Struct

```c
typedef struct {
    const char *name;
    const char *description;
    VideoConnectionType connection;
    VideoCombType       comb_type;
    PresetConsoleVariant console_variant;
    int                 speaker_type;
    int                 region;
    CableParams         video_cable;
    CableParams         audio_cable;
    TVDisplayParams     tv;
    float console_coupling_R;     // ohms
    float console_coupling_C;     // farads
    float console_amp_bw;         // Hz
    float console_psu_hum;
    RFModulatorParams   rf;
    float brightness, contrast, chroma_gain;
    float audio_psu_hum_amplitude;
    float audio_noise_floor;
    float audio_saturation_drive;
    float audio_cable_length_m;
} PhysicalPreset;
```

### Preset List

| Index | Name | Connection | Comb | Console | TV/Monitor |
|-------|------|-----------|------|---------|------------|
| 0 | Living Room 1988 | Composite | None | NES Front | 19" Zenith shadow mask |
| 1 | Bedroom RF 1990 | RF | None | NES Front | 13" GE portable |
| 2 | Studio PVM | S-Video | Bypass | NES Front | Sony PVM-20M4U aperture grille |
| 3 | Famicom Kitchen | RF | None | Famicom | 14" Japanese TV, D93 9300K |
| 4 | Arcade Cabinet | RGB | None | NES Front | 25" arcade monitor shadow mask |
| 5 | Late CRT (Wega) | Composite | 2-line | NES Front | 27" Sony KV-27FS120 flat Trinitron |
| 6 | Retro Gaming Setup | S-Video | Bypass | NES Front | PVM + headphones |
| 7 | Basement TV | RF | None | NES Front | 19" beat-up CRT |
| 8 | Stas's Favourite | Composite | None | NES Front | Basement aesthetic, clean signal |

### Console Variants

| Variant | Coupling Cap | fc (HP) | Amp BW | Notes |
|---------|-------------|---------|--------|-------|
| Famicom | 100 uF | 0.16 Hz | 10 kHz | Larger cap, narrower amp |
| NES Front-Loader | 10 uF | 1.6 Hz | 14 kHz | Standard US NES |
| NES Top-Loader | 10 uF | 1.6 Hz | 12 kHz | Slightly narrower amp |
| Dendy | 47 uF | 0.34 Hz | 8 kHz | Russian clone, narrowest amp |

### Speaker Types

| Type | Resonance | Low BW | High BW | Q | Breakup |
|------|-----------|--------|---------|---|---------|
| Small TV | 350 Hz | 400 Hz | 6 kHz | 2.0 | 5 kHz |
| Console TV | 150 Hz | 80 Hz | 10 kHz | 1.5 | 6 kHz |
| PVM | 100 Hz | 60 Hz | 15 kHz | 0.8 | 8 kHz |
| Arcade | 200 Hz | 100 Hz | 8 kHz | 3.0 | 4 kHz |
| Headphones | 20 Hz | 20 Hz | 20 kHz | 0.7 | 15 kHz |
| Famicom RF | 400 Hz | 500 Hz | 4 kHz | 2.5 | 3.5 kHz |

---

## Audio Chain

### 10-Stage Pipeline

| Stage | Kernel | Type | Description |
|-------|--------|------|-------------|
| 1 | RC_FILTER | HP | Coupling cap DC blocking |
| 2 | RC_FILTER | HP | Feedback network bass shaping |
| 3 | RC_FILTER | LP | Amplifier bandwidth limit |
| 4 | POINTWISE | mode 1 | tanh soft-clip saturation |
| 5 | POINTWISE | mode 5 | PSU hum (60 Hz + harmonics) |
| 6 | POINTWISE | -- | White noise injection |
| 7 | RC_FILTER | LP | Cable capacitance lowpass |
| 8 | RC_FILTER | HP | TV input coupling cap |
| 9 | FIR | -- | Speaker biquad (resonance + rolloff) |
| 10 | FIR | -- | Decimation (1.79 MHz -> 48 kHz) |

### Console Variant Differences

| Parameter | Famicom | NES Front | NES Top | Dendy |
|-----------|---------|-----------|---------|-------|
| Coupling R | 10 kohm | 10 kohm | 10 kohm | 10 kohm |
| Coupling C | 100 uF | 10 uF | 10 uF | 47 uF |
| Coupling fc | 0.16 Hz | 1.6 Hz | 1.6 Hz | 0.34 Hz |
| Feedback fc | 482 Hz | 442 Hz | 442 Hz | 442 Hz |
| Amp BW fc | 9.6 kHz | 14.1 kHz | 11.8 kHz | 7.9 kHz |

### Audio Buffer Format

- Input: 1 frame of float32 samples at CPU clock rate (~29,829 NTSC, ~33,252 PAL). ~116 KB.
- Output: Decimated to 48 kHz. 800 samples/frame NTSC, 960 PAL. ~3.2 KB.
- Workgroup size: 1024. Blocks per frame: ~30 (NTSC), ~33 (PAL).

---

## Debug Infrastructure

### DebugTap API

```c
// Create manager for both chains (either may be NULL).
DebugTapManager *debug_tap_create(const SignalChain *video_chain,
                                   const SignalChain *audio_chain);
void debug_tap_destroy(DebugTapManager *mgr);

// Enable/disable individual tap points.
bool debug_tap_set_enabled(DebugTapManager *mgr, int stage_index, bool enabled);
void debug_tap_set_all_enabled(DebugTapManager *mgr, bool video, bool audio);

// Capture enabled taps (call once per frame after chain_run).
void debug_tap_capture(DebugTapManager *mgr, SDL_GPUDevice *gpu,
                       uint64_t frame_number);

// Retrieve captured data (read-only safe from other threads).
const DebugTap *debug_tap_get(const DebugTapManager *mgr, int stage_index);
```

Each `DebugTap` contains:
- `float *data` -- CPU-side buffer copy
- `int sample_count`, `samples_per_line`, `lines` -- 2D interpretation
- `uint64_t frame_number`, `capture_timestamp_ns` -- metadata
- `double gpu_dispatch_us`, `readback_us` -- performance
- `uint8_t stage_params[128]` -- snapshot of stage uniforms
- `bool is_valid`, `const char *error_msg` -- quality flags

Typical readback latency: ~3-5 ms for video (1.9 MB NTSC), <1 ms for audio (120 KB).

### ChainVis Overlay

Toggled with F8. Renders into a 256x240 uint8 buffer (NES-resolution, palette-indexed) composited onto the PPU framebuffer.

Two-column layout: video stages (left), audio stages (right). Arrow keys navigate, B toggles bypass on the selected stage.

```c
ChainVis *chain_vis_create(const VideoChain *vc, const AudioChain *ac);
void chain_vis_toggle(ChainVis *vis);
void chain_vis_update(ChainVis *vis, int current_preset);
const uint8_t *chain_vis_get_overlay(const ChainVis *vis, int *w, int *h);
bool chain_vis_handle_key(ChainVis *vis, int scancode, bool down);
```

### Debug Server

Unix domain socket server for the SwiftUI chain visualiser.

```c
DebugServer *debug_server_create(const char *socket_path);
void debug_server_destroy(DebugServer *srv);
void debug_server_frame(DebugServer *srv,
                        const SignalChain *video_chain,
                        const SignalChain *audio_chain,
                        uint32_t frame_number);
void debug_server_send_tap(DebugServer *srv, int stage_index,
                           const float *data, int sample_count,
                           int samples_per_line, int lines);
bool debug_server_has_client(const DebugServer *srv);
```

Binary protocol: `[uint32 type][uint32 size][payload]`. Message types: 0=snapshot, 1=param_update, 2=preset_change, 3=tap_request, 4=tap_data.

Background listener thread; all processing on main thread via `debug_server_frame()`.
