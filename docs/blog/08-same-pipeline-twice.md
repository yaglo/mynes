# The Same Pipeline Twice

*CPU composite vs GPU composite -- why both exist*

*Part 8 of "Building a NES Emulator That Thinks Like Hardware"*

---

This emulator has two complete composite video pipelines. The CPU path lives in `src/nes/composite.h`. The GPU path lives in `frontends/gpu/`. Both take the same indexed framebuffer as input. Both produce NTSC composite artifacts -- dot crawl, chroma bleed, rainbow shimmer on high-contrast edges. Why build the same signal processing chain twice?

## The CPU path

The CPU composite pipeline is a single header file: `composite.h`, about 2,500 lines including all SIMD paths. No external dependencies beyond `math.h`. It runs a single-pass pipeline per scanline: waveform generation from the 2C02's palette index, FIR bandwidth limiting, Y/I/Q demodulation, matrix decode to RGB, scanline darkening. Output is RGB888 written directly to the framebuffer.

The hot path is the FIR filter. A Hamming-windowed sinc lowpass, applied symmetrically -- which means half the multiplies, since `taps[k] == taps[n-1-k]`. The CPU path has explicit SIMD for this: NEON on ARM, AVX2+FMA on x86, scalar fallback everywhere else.

Here's the NEON inner loop from `comp_fir_symmetric`:

```c
#if defined(__ARM_NEON)
    for (; x + 4 <= w; x += 4) {
        const float *pw = p_in + x - half;
        float32x4_t a0 = vdupq_n_f32(0.0f);
        float32x4_t a1 = vdupq_n_f32(0.0f);
        float32x4_t a2 = vdupq_n_f32(0.0f);
        float32x4_t a3 = vdupq_n_f32(0.0f);
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            a0 = vfmaq_n_f32(a0, vld1q_f32(pw + k + 0), tps[k + 0]);
            a1 = vfmaq_n_f32(a1, vld1q_f32(pw + k + 1), tps[k + 1]);
            a2 = vfmaq_n_f32(a2, vld1q_f32(pw + k + 2), tps[k + 2]);
            a3 = vfmaq_n_f32(a3, vld1q_f32(pw + k + 3), tps[k + 3]);
        }
        float32x4_t acc = vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3));
        for (; k < n; k++) {
            acc = vfmaq_n_f32(acc, vld1q_f32(pw + k), tps[k]);
        }
        vst1q_f32(&out_p[x], acc);
    }
```

Four independent accumulators (`a0` through `a3`), each advancing through the taps by four. No loop-carried dependency chain. On an M1, the CPU can dispatch four FMAs per cycle through this, and the whole pipeline -- waveform emission, luma FIR, chroma FIR, demodulation, color matrix, scanline assembly -- runs in well under a millisecond per frame. The AVX2 path processes eight pixels per iteration with the same four-way ILP pattern.

The CPU path is used by the SDL2 frontend, the headless renderer, and the test runner. Anything that needs composite output and doesn't have a GPU.

## The GPU path

The GPU pipeline is a different animal. Instead of a single-pass scanline loop, it's a chain of discrete compute shader stages, each modeling a physical component in the analog signal path. The full chain for an RF connection runs 14 stages:

1. **2C02 DAC** -- palette index to composite waveform (same Bisqwit model)
2. **Console output** -- coupling capacitor, amplifier bandwidth
3. **Cable** -- RC low-pass from distributed capacitance (80 pF/m for cheap RCA)
4. **RF modulator/demodulator** -- vestigial sideband modulation, AGC, thermal noise
5. **TV input** -- coupling, automatic gain control
6. **Comb filter** -- Y/C separation (none, 1-line, 2-line, or bypass for S-Video)
7. **Chroma demodulator** -- QAM decode of I and Q, with FIR on each channel
8. **Luma processing** -- bandwidth limiting, FIR filtering
9. **Matrix decode** -- YIQ to RGB with color temperature and gun drive controls
10. **Video amplifier** -- per-channel bandwidth limiting
11. **Electron beam** -- spot profile, convergence, bloom
12. **Phosphor screen** -- shadow mask / aperture grille, persistence
13. **CRT glass** -- halation, barrel distortion, glass tint
14. **Environment** -- vignette, ambient light, black floor

Each stage is a compute dispatch. Connection type determines which stages are active -- S-Video skips the comb filter (Y/C is already separated), RGB skips comb, chroma demod, and matrix decode, and Direct mode skips everything between the DAC and the display domain. The chain queries `video_chain_stage_active()` for each stage and skips the dispatch if it returns false.

The GPU path requires SDL3 GPU, which means Vulkan, Metal, or D3D12. It's used by the GPU frontend and the SwiftUI app.

## Why both exist

**Portability vs fidelity.** The CPU path runs anywhere with a C compiler. No GPU required, no graphics API required. It compiles on ARM, x86, and whatever else has a C99 toolchain. For headless testing, CI runners, or embedded targets, the CPU path is the only option. The GPU path needs modern graphics hardware and a specific backend.

**Correctness verification.** Two independent implementations of the same signal processing catch bugs in either. The waveform generation, FIR coefficients, and demodulation math should produce identical Y, I, and Q values at the decode stage. If the GPU path produces different cross-color patterns than the CPU path on the same input, one of them is wrong. The CPU path was written first and verified against known-good reference output. The GPU path was then verified against the CPU path. Having both means regressions in the GPU shader chain can be caught by comparing against the CPU reference.

**Different tradeoffs.** The CPU path is single-pass and fast, but it can't model multi-stage interactions. There's no cable RC filter between the console output and the TV input -- it goes straight from waveform to FIR. No comb filter modes (the CPU path only does simple bandpass Y/C separation). No RF simulation. No temporal phosphor persistence. No beam bloom or convergence error. The GPU path models all of these because each stage is a separate dispatch with its own physical parameters. But that costs 14 compute dispatches plus render passes per frame, and it requires a GPU.

**Shared signal math.** Both paths use the same underlying signal model. The GPU path's `signal_precompute.h` was extracted from `composite.h`'s precomputation functions so the GPU frontend has zero dependency on the CPU composite pipeline, but the math is the same. Both build a 512-entry signal table (64 palette colors times 8 emphasis states, 24 phase slots each) using Bisqwit's 2C02 voltage model. Both design their FIR taps as Hamming-windowed sinc functions normalized to unit DC gain. The core signal math is identical -- the GPU path just runs each step as a separate compute dispatch with physically-modeled stages between them.

## Where they converge, where they diverge

The convergence point is the signal table and the FIR design. Both use 12-phase composite waveforms, both use the same normalized sinc formula:

```c
float sinc = (m == 0)
    ? 2.0f * cutoff
    : sinf(2.0f * M_PI * cutoff * (float)m)
      / (M_PI * (float)m);
float w = 0.54f - 0.46f * cosf(2.0f * M_PI * (float)k / (float)(n - 1));
taps[k] = sinc * w;
```

Same cutoff frequencies, same window function, same normalization. A frame decoded by the CPU path and a frame decoded by the GPU path (with cable, RF, and display effects disabled) should produce visually identical output.

The divergence is everything after decode. The CPU path does waveform, FIR, demod, matrix in a tight scanline loop and writes RGB. The GPU path separates these into individual dispatches with cable RC filtering, comb filter Y/C separation, RF modulation, and AGC between them. Then the GPU path continues with five more stages -- video amplifier, electron beam, phosphor screen, CRT glass, and environment -- that the CPU path doesn't model at all. The CPU path handles scanline darkening and a few post-processing effects (barrel distortion, ghosting, snow, hum bars), but these are simple screen-space operations, not physical models with component values.

## The engineering pattern

Maintain two implementations of the same specification at different fidelity levels. The simple one validates the complex one. The complex one handles the cases the simple one can't. Neither is redundant.

This isn't unique to emulation. Any signal processing pipeline benefits from having a reference implementation alongside the production one. The reference is slow, simple, and obviously correct. The production code is fast, complex, and -- you hope -- equivalent. When they disagree, the reference tells you where to look. When they agree, you have confidence that neither is wrong.

The CPU path is the reference. The GPU path is the production code. Both process the same signal. Both exist because the alternative -- trusting a single complex pipeline to be correct -- is how you ship bugs that look like "the colors are slightly off" and never get caught.
