# Virtual Analog Signal Path: GPU Compute Frontend

> **Note:** This is the original design document that shaped the GPU
> frontend's architecture. It's preserved for historical context — many
> items marked "[~]" or "[ ]" in the progress section below are now
> implemented. For current status, see the frontend source itself.

## Context

Build a new frontend (`frontends/gpu/`) modeling the complete NES analog signal path as a chain of discrete physical components. SDL3 + SDL_GPU (Vulkan/Metal/D3D12) + HDR. Every capacitor, resistor, cable, and amplifier is an individual compute kernel stage.

## Design Principles (from review)

1. **Interface contracts first** — define waveform + audio buffer formats as documented headers BEFORE any shader code
2. **RC prefix scan: keep simple** — sequential N-section cascades, verify against CPU reference before optimizing
3. **CPU audio is primary** — GPU audio chain for verification/offline, not real-time (readback latency)
4. **No premature abstraction** — 6 standalone kernels with tests BEFORE the chain composition system
5. **Chain Visualiser** — in-app overlay with oscilloscope traces, per-stage bypass, signal meters, split-screen A/B (Phase 8)
6. **Hot-switchable connection type** — RF/Composite/S-Video/RGB at runtime, pre-allocate for longest chain
7. **Synthetic test signals** — colour bars (video) + sine sweep (audio) for per-stage regression testing
8. **SDR first, HDR as flag** — compute >1.0 always, tone-map for SDR, remove for HDR
9. **Instrument from day one** — GPU timestamp queries per dispatch, per-stage timing behind debug flag

## Implementation Phases

## Progress

- [x] Phase 0: Interface contracts (signal_format.h, audio_format.h)
- [x] Phase 1: 5 CPU reference kernels + 15 tests + 5 GLSL→SPIR-V compute shaders
- [x] Phase 2: SDL3 + SDL_GPU window with composite passthrough + audio + input
- [x] GPU compute dispatch (gpu_compute.h/c — SPIR-V loading, buffer management, dispatch)
- [~] Phase 3: Audio chain definition (audio_chain.h/c — 10 stages with real R/C values)
- [~] Phase 3: Audio GPU dispatch (audio_gpu.h/c — wiring stages into compute dispatches)
- [~] Video chain definition (video_chain.h — 14 stages, 6 connection types)
- [~] Video chain init (video_chain.c — default component values per connection type)
- [~] Physical presets (presets.h/c — 8 historically-accurate chain configurations)
- [~] Chain visualiser architecture design
- [ ] Phase 4: Cable / transmission medium
- [ ] Phase 5: Video signal chain on GPU
- [ ] Phase 6: RF path
- [ ] Phase 7: Display domain (CRT physics) + HDR
- [ ] Phase 8: Physical preset system (UI integration)
- [ ] Phase 9: Chain visualiser & editor
- [ ] Phase 10: PAL integration

## OSD Rendering

Two modes:
1. **In-signal OSD** (default): render menu into ppu.framebuffer + index_framebuffer
   BEFORE comp_process() — text goes through the full analog chain and looks
   like a real CRT's built-in OSD. Already works (inherited from SDL2's
   menu_render_nes approach).
2. **Clean overlay** (toggle): render sharp text AFTER the signal chain as a
   GPU render pass on top of the processed output. Useful for the chain
   visualiser where readability matters more than authenticity.

### Phase 0: Interface Contracts
Define the CPU→GPU buffer formats. This is the foundation everything depends on.

**Deliverables:**
- `frontends/gpu/signal_format.h` — waveform buffer format (samples/scanline, bit depth, layout, upload granularity)
- `frontends/gpu/audio_format.h` — audio buffer format (sample rate, samples/frame, channel layout)
- Comments explaining the physical meaning of each field

### Phase 1: Kernel Primitives (standalone, with tests)
6 compute shaders + CPU reference implementations + unit tests. No chain runner yet.

**Deliverables:**
- 6 GLSL compute shaders (pointwise, rc_filter, fir, delay, modulator, blur2d)
- 6 matching CPU reference functions in C
- Test harness: synthetic inputs → GPU kernel → compare with CPU reference
- GPU timestamp instrumentation from the start

### Phase 2: SDL3 Window + Basic Rendering
Minimal SDL3 app that opens a window, runs NES emulation, uploads composite.output as texture, displays it.

### Phase 3: Audio Chain on GPU (verification path)
GPU audio for A/B testing against CPU. CPU path stays primary for real-time.

### Phase 4: Cable / Transmission Medium
Distributed RC cable model + impedance reflections + connection type selector (hot-switchable).

### Phase 5: Video Signal Chain on GPU
Console output → cable → TV input → comb → chroma demod → luma → matrix decode.

### Phase 6: RF Path
Full RF modulator/demodulator + noise model as optional insertable stages.

### Phase 7: Display Domain (CRT physics) + HDR
Video amp → beam → phosphor → glass → environment. SDR first, HDR flag.

### Phase 8: Physical Preset System
Component-chain-configured presets with real values.

### Phase 9: Chain Visualiser & Editor
In-app overlay: node graph, oscilloscope traces at tap points, per-stage bypass, signal meters, split-screen A/B, live parameter editing, preset save/load.

### Phase 10: PAL Integration

## Critical Files

```
frontends/gpu/
  signal_format.h       — waveform buffer format contract (Phase 0)
  audio_format.h        — audio buffer format contract (Phase 0)
  kernels/              — CPU reference implementations (Phase 1)
    pointwise_ref.c
    rc_filter_ref.c
    fir_ref.c
    delay_ref.c
    modulator_ref.c
    blur2d_ref.c
  shaders/compute/      — GPU compute kernels (Phase 1)
    pointwise.comp.glsl
    rc_filter.comp.glsl
    fir.comp.glsl
    delay.comp.glsl
    modulator.comp.glsl
  shaders/render/       — Display shaders (Phase 7)
    fullscreen.vert.glsl
    blur2d.frag.glsl
    crt_display.frag.glsl
  tests/                — Kernel tests (Phase 1)
    test_kernels.c
  main.c                — SDL3 app (Phase 2)
  gpu_device.c          — SDL_GPU device management
  signal_chain.c        — Chain runner (Phase 4+)
  audio_chain.c         — Audio chain (Phase 3)
  video_chain.c         — Video chain (Phase 5)
  presets.c             — Physical presets (Phase 8)
  visualiser.c          — Chain visualiser overlay (Phase 9)
```
