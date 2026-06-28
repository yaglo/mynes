# PAL Composite Simulation Plan

Status: **deferred until APU rework is done.**

## Context

The NTSC waveform composite pipeline in `src/nes/ntsc_composite.h` generates
the video signal directly from 2C02 palette indices using Bisqwit's canonical
voltage-level model, then decodes it with FIR filters + adaptive comb. It's
entirely NTSC-specific: 3.579 MHz subcarrier, YIQ decoding, NTSC 2C02 palette
waveform.

For completeness and authenticity, PAL NES systems (2C07 PPU) should get the
same treatment. A chunk of the infrastructure already exists — this doc
captures what's there, what needs adding, and the order of operations.

## What already works

The **NES-side PAL support** is in place:

| Component | Status | Notes |
|---|---|---|
| `PPU_REGION_PAL` | ✅ existing | `src/ppu/ppu.h:157-159`, `ppu_set_region()` |
| PAL scanline count (312) | ✅ | `ppu.prerender_line = 311` |
| PAL odd-frame skip disabled | ✅ | `skip_odd_frames = false` |
| PAL frame rate (50 Hz) | ✅ | `main.c:689` `frame_ticks = freq / 50` |
| ROM region detection | ✅ | `rom.h` TV system flag |
| PAL CPU/PPU clock ratio | ✅ | Set via `ppu_set_region()` |
| APU frame counter (PAL) | ⚠️ partial | Separate work — covered by APU rework |
| **PAL composite pipeline** | ❌ missing | this plan |
| **PAL palette LUT (2C07)** | ❌ missing | fallback for non-composite mode |

The NTSC waveform pipeline doesn't know about regions — it always generates
with 2C02 voltage levels and NTSC subcarrier math. Running a PAL ROM today
gives wrong colors and slightly wrong timing in composite mode.

## PAL vs NTSC: what has to change in the pipeline

### 1. Subcarrier frequency

- **NTSC**: 3.579545 MHz = 2/3 cycles per NES pixel
- **PAL**:  4.43361875 MHz = **1.25 cycles per NES pixel** on a 2C07
  (actual ratio depends on PAL master clock 26.601712 MHz and PAL PPU
  clock 5.320342 MHz; worth double-checking from NesDev)

Our signal buffer runs at `NTSC_SAMPLES_PER_PIXEL = 8` samples per pixel.
At NTSC that's 8 samples per subcarrier cycle (subcarrier period = 12 pixel
slots × 2/3 cycles = 8 sample period). At PAL we'd want the subcarrier period
in samples to still be an integer (for clean wraparound in precomputed tables),
which means a different `SAMPLES_PER_PIXEL`:

```
PAL subcarrier = 1.25 cycles/pixel  →  period = 0.8 pixels = 6.4 samples
  (at 8 samples/pixel)                   ← non-integer, bad
                                      →  period = 8 samples = 1.25 pixels
  (at 10 samples/pixel)                  ← works cleanly
```

Options:
- **(a)** Bump the internal sample rate to `12` samples/pixel — integer for
  both NTSC (12 samples = 1.5 subcarrier cycles = 2/3 × 12/8 = 1 cycle? let
  me re-derive) and PAL (12 samples = 1.5 subcarrier cycles = valid).
  Doubles the FIR cost — tractable but noticeable.
- **(b)** Use separate sample rates per region — NTSC stays at 8 spp,
  PAL goes to 10 spp (so subcarrier period = 8 samples). Pipeline needs
  a runtime-parameterizable `samples_per_pixel` instead of the current
  compile-time constant.
- **(c)** Accept non-integer period on PAL — use a longer phase cycle
  (80 samples = 10 pixels) as the canonical period, precompute a table
  big enough to cover it without wrapping. Uglier.

**Recommended**: option (b). Add a `samples_per_pixel` field to `NTSCSignal`,
branch the hot loops on it, precompute tables at the right rate. The FIR
code already handles variable widths; only the signal-emission loop has a
hardcoded 8.

### 2. Phase alternation on the V (R-Y) component

PAL's defining feature: the V component's phase is **inverted every scanline**.
The decoder recovers it with a 1-line delay line that averages adjacent
scanlines — which cancels any residual phase error.

Our NTSC pipeline already has `phase_line_adv = 6` (180° flip per scanline).
That gives 2D comb Y/C separation exactly as a PAL decoder would — we get
it for free. What's different on PAL:
- The flip is on V only (not both I and Q in NTSC quadrature)
- Averaging the V component between lines is MANDATORY for correct color
  (not just an optimization like in our NTSC comb filter)

In our code: the comb filter is already "on" by default and does the work.
Implementation-wise, PAL just needs the adaptive comb to always be enabled
with no edge fallback on the V channel — or accept the edge-case luma
artifacts from the existing adaptive logic.

### 3. Color space: YUV vs YIQ

| Encoding | Coefficients |
|---|---|
| NTSC (YIQ) | I, Q are rotated 33° from U, V |
| PAL (YUV) | U = 0.492 (B-Y),  V = 0.877 (R-Y) |

YUV → RGB matrix:
```
R = Y              + 1.140 V
G = Y - 0.395 U    - 0.581 V
B = Y + 2.032 U
```

vs YIQ → RGB (current code):
```
R = Y + 0.956 I + 0.619 Q
G = Y - 0.272 I - 0.647 Q
B = Y - 1.106 I + 1.703 Q
```

Implementation: add a second output helper `ntsc_yuv_to_rgb()` parallel to
`ntsc_yiq_to_rgb()`, dispatched from `ntsc_emit_output_row` based on the
region flag. Or: unify them by having a single matrix stored in `NTSCComposite`
and swap the coefficients at init time.

**Recommended**: unified matrix. Cleaner.

### 4. 2C07 signal table (PAL palette voltage levels)

The 2C07 PPU has its own voltage-level model. From NesDev:
- Different voltage swings than 2C02 (slightly different luma tiers)
- The hues are at the same 12-slot phase positions, but rotated by some
  constant offset relative to NTSC
- Color $0D behaves differently (not the "blacker than black" NTSC trick)
- Some emphasis bit interactions differ

**Reference material**: `tests/nes-test-roms/ppu_read_buffer/source/gfx/nes_palette.php`
has the NTSC formula. NesDev wiki has a corresponding PAL voltage table.

Implementation: a second precompute function `ntsc_precompute_signal_table_pal(NTSCSignal*)`
that fills `signal_table[][12]` with 2C07 voltages. Dispatch at init time
based on region.

### 5. 2C07 fallback palette LUT

The non-composite path (legacy RGB LUT) uses `ppu_palette_2c02` from
`src/ppu/ppu.h`. For PAL games, we'd want a `ppu_palette_2c07` LUT as the
default color source.

There are several community-derived 2C07 palette LUTs. The canonical one
is FirebrandX's PAL palette. Copy the 64 RGB triples into a new const in
`ppu.h` and pick it based on region in `ppu_render_pixel`.

### 6. Frame rate / audio sample rate

Already handled in the main loop (`frame_ticks = freq / (region == PAL ? 50 : 60)`),
but the APU sample rate needs PAL-correct timing. Covered by the APU rework,
not this plan.

## Implementation plan

### Commit 1: 2C07 palette LUT

**File**: `src/ppu/ppu.h`

Add `ppu_palette_2c07[64][3]` const next to the existing 2C02 table. Switch
the `ppu_render_pixel` lookup to pick between them based on `ppu->region`.
Or better: have `ppu->color_palette` default to the region-appropriate one
in `ppu_set_region` and keep `render_pixel` region-agnostic.

**Source**: use FirebrandX's "PAL (FBX)" palette or equivalent — already in
`palettes/` directory?  Check first; if yes, extract; if no, generate from
Bisqwit's 2C07 voltage formula.

Additive change. Non-composite PAL mode starts rendering with correct colors.

### Commit 2: `NTSCSignal` region field + parameterized samples_per_pixel

**File**: `src/nes/ntsc_composite.h`

- Add `int region` to `NTSCSignal` (0 = NTSC, 1 = PAL)
- Add `int samples_per_pixel` (runtime field replacing the
  `NTSC_SAMPLES_PER_PIXEL` macro in the hot paths)
- Update signal emission loop to use the field instead of the macro
- Update the FIR frequency scales to track the sample rate
- NTSC path still works — `samples_per_pixel = 8`, `region = 0` by default

Pure refactor. Output should be bit-identical for NTSC.

### Commit 3: 2C07 signal table precompute

**File**: `src/nes/ntsc_composite.h`

Add `ntsc_precompute_signal_table_pal(NTSCSignal *s)` alongside the existing
NTSC version. It fills the same `signal_table[512][NTSC_TABLE_STRIDE]`
structure but with:
- PAL voltage levels (derive from NesDev's 2C07 table)
- `samples_per_pixel = 10` so the subcarrier period is an integer number
  of samples (8)
- `NTSC_TABLE_PHASES = 8` for PAL (since the period is 8 samples vs 12 for
  NTSC). Need to either generalize `NTSC_TABLE_PHASES` to a runtime value
  or add a parallel `PAL_TABLE_PHASES`.

`ntsc_init` picks which precompute to call based on the region field.

### Commit 4: PAL-aware demod

**File**: `src/nes/ntsc_composite.h`

In `ntsc_process_waveform`:
- The demod carrier tables `demod_cos/sin` are currently 12 entries for NTSC.
  For PAL they need to be 8 entries and use PAL's phase convention.
- Use `demod_cos_pal/demod_sin_pal` or generalize to arrays of
  `TABLE_PHASES` entries.
- Apply **V flip** by negating the sin component on alternate scanlines (PAL
  phase alternation). This doubles the work slightly but is free — just a
  sign bit per sample.

### Commit 5: YUV → RGB output

**File**: `src/nes/ntsc_composite.h`

- Replace the hardcoded YIQ coefficients in `ntsc_yiq_to_rgb` with a matrix
  stored in `NTSCComposite`
- At init time, populate the matrix with YIQ (NTSC) or YUV (PAL)
  coefficients based on region
- The output function becomes `ntsc_decode_to_rgb` — same signature, just
  reads coefficients from the struct

The legacy RGB path (non-waveform) already produces correct RGB via the
palette LUT, so that path is unaffected.

### Commit 6: Region wiring in `ntsc_init` + SDL frontend

**Files**: `src/nes/ntsc_composite.h`, `frontends/sdl/main.c`

- `ntsc_init` takes an extra `region` parameter (or reads from `NES` state)
- SDL frontend detects ROM region and passes it in
- Runtime region switches (rare for NES games but possible for test ROMs)
  trigger a `ntsc_resize` + `ntsc_redesign_firs` to rebuild tables

### Commit 7: Tuning + PAL-specific presets

**File**: `frontends/sdl/main.c`

Add PAL variants of the existing presets, or one catch-all "PAL Consumer TV"
preset. Numbers will differ since PAL has:
- Softer chroma (half vertical resolution via mandatory V averaging)
- Different phase-flip characteristics
- Slightly different bloom/afterglow feel (50 Hz vs 60 Hz field rate)

The existing filters/phase/color presets stay NTSC-tuned; add PAL variants
to the Presets submenu.

## Verification

**Unit / static**:
- PAL signal table values cross-checked against NesDev reference
- YUV → RGB output for pure test colors matches external reference decoder

**Integration / visual**:
1. Load a PAL ROM (e.g., a European release of SMB3 or Castlevania 3).
   The CRC/checksum flag in the iNES header identifies it as PAL.
2. Verify game runs at 50 Hz (watch the clock in the HUD).
3. Verify colors match a PAL reference screenshot (FirebrandX palette
   comparison).
4. Verify composite artifacts are present but less drifty than NTSC
   (PAL V averaging ≈ no hue drift).
5. Take an A/B screenshot comparison of the same ROM run in NTSC vs
   PAL modes (if both versions of the game exist).
6. Toggle between `Consumer TV` NTSC and `Consumer TV PAL` presets on
   the same frame. Confirm:
   - PAL version has softer vertical chroma
   - No hue drift visible on test patterns (tvpassfail, 240p suite, etc.)

**Perf**:
- PAL composite pipeline runs at 50 Hz so the per-frame budget is actually
  **20 ms** instead of 16.7. More headroom for wider filters.
- If `samples_per_pixel = 10` the FIR work is ~25% higher than NTSC's 8 spp.

## Deferred / explicitly out of scope

- **SECAM**: won't be implemented. No NES games were ever encoded as SECAM;
  SECAM countries got NES via PAL-B transcoding or consumer TVs that decoded
  both. Our AM-quadrature pipeline fundamentally can't do FM.
- **PAL-M / PAL-N**: variants used in Brazil/Argentina, with NTSC-like
  subcarrier frequencies. Technically decodable with minor parameter tweaks
  but no NES games shipped for these markets specifically.
- **APU PAL timing**: covered by the separate APU rework, not this plan.
- **PAL-specific emphasis bit behavior**: the 2C07 handles emphasis bits
  slightly differently (different attenuation per octant). Document the
  spec difference but defer the implementation to a follow-up unless it
  visibly breaks known test ROMs.
- **Region auto-detect from ROM**: wire the existing `rom.h` region flag
  into the SDL frontend. Simple but not strictly required for the first
  PAL commit.

## Critical files

- **`src/nes/ntsc_composite.h`** — the signal table, precompute, FIRs,
  demod, and emit helpers. Most of the work lives here. Probably
  200–300 lines of added code to support PAL as a parallel path.
- **`src/ppu/ppu.h`** — add `ppu_palette_2c07[]` and wire region-aware
  default palette in `ppu_set_region`.
- **`frontends/sdl/main.c`** — pass region into `ntsc_init`, add PAL
  variants to the Presets submenu, optionally add an explicit region
  toggle for testing.
- **`tests/nes-test-roms/ppu_read_buffer/source/gfx/nes_palette.php`** —
  reference only for NTSC; NesDev wiki is the source for PAL 2C07
  voltage levels.

## Reference materials

- NesDev wiki: **PPU palettes** page — contains 2C07 voltage table and
  phase conventions
- NesDev wiki: **PAL vs NTSC** — hardware differences summary
- FirebrandX PAL palette — community-derived reference LUT
- Bisqwit's NES NTSC article — explains the 2C02 model, PAL analog
- ITU-R BT.470 — the PAL broadcast standard (overkill for NES but
  useful if the YUV matrix ever looks wrong)

## Estimated size

- New code: ~250 LOC in `ntsc_composite.h`
- New palette table: ~200 LOC in `ppu.h`
- Frontend wiring: ~40 LOC in `main.c`
- **Total**: ~500 LOC across 3 files, roughly one day of focused work.
  Less if the APU region work already passed region info into the
  composite pipeline.
