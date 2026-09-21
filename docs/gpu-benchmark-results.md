# GPU-chain measurements — earlier 21 September 2026 snapshot

These figures predate the latest colour/spot-growth and RF changes. The complete-playback measurements below use the current presets.

Apple M5, 24 GiB, Metal, validation off. These are CPU-submission-to-final-GPU-fence times, including code upload, waveform, receiver, CRT, mask and glass. Each resolution uses 12 warmups and 60 individually fenced frames. Emulation, audio, vsync and readback are excluded. No other MyNES frontend was running; this was not an otherwise isolated machine.

| Preset | 640×480 median | 1280×960 median | 1920×1440 median | 2560×1920 median / p95 / max |
|---|---:|---:|---:|---:|
| Sony PVM-14L2 (nominal) | 2.718 ms | 3.831 ms | 5.724 ms | 8.147 / 10.980 / 11.234 ms |
| JVC D-Series (nominal) | 3.569 ms | 4.556 ms | 6.418 ms | 8.972 / 11.875 / 12.163 ms |
| Toshiba 14AF (nominal) | 3.547 ms | 4.543 ms | 6.423 ms | 9.050 / 12.005 / 12.146 ms |
| Stas's Favourite | 3.204 ms | 4.267 ms | 6.209 ms | 8.785 / 11.791 / 11.839 ms |

Panel mask mode, offscreen scale 1:1. A 2560×1920 target is larger than this laptop’s 2560×1664 panel; it is a workload measurement, not a fullscreen screenshot. Raw measurements and load metadata are in [JSON](gpu-benchmark-results.json).

The chain fits within the approximately 16.64 ms NTSC frame interval in this sample. This does not establish end-to-end latency or guarantee pacing under contention. The visible cadence also depends on the host refresh rate.

Optimization replaced repeated per-sample Gaussian exponentials with dispatch-prepared weights, and procedural dot/slot supersampling with cached half-float coverage mipmaps. Kernel checks verify impulse response and energy; this run also includes the new comb BPF and nonlinear PPU output stage.

Reproduce:

```sh
python3 frontends/gpu/tests/benchmark_pipeline.py build/bin/mynes_gpu /tmp/gpu-bench
```

## Full real-game playback, current presets

Contra, 720 emulated frames per run, 120 warmup frames, offscreen 2560×1664. Includes core, APU, CPU/GPU audio processing with an active muted SDL queue, and every GPU signal/CRT/glass pass. Readback additionally captures final PPM+PFM once per 60 emulated frames. Window presentation/vsync are excluded by the user-requested offscreen mode. Another MyNES process and desktop load were present; runs were sequential, not isolated. Do not attribute differences between sequential presets solely to their shaders.

| Preset | Audio / capture | Render FPS | Emulated FPS | Skipped pictures | p95 frame cadence | p95 audio queued |
|---|---|---:|---:|---:|---:|---:|
| sony_pvm_14l2 | cpu | 60.12 | 60.12 | 0 | 19.53 ms | 43.5 ms |
| sony_pvm_14l2 | gpu | 60.08 | 60.08 | 0 | 20.38 ms | 43.5 ms |
| sony_pvm_14l2 | gpu-readback | 55.47 | 60.15 | 53 | 20.22 ms | 43.5 ms |
| jvc_d_series_2000 | cpu | 60.08 | 60.08 | 0 | 20.25 ms | 43.5 ms |
| jvc_d_series_2000 | gpu | 60.08 | 60.08 | 0 | 19.17 ms | 43.4 ms |
| jvc_d_series_2000 | gpu-readback | 55.98 | 60.12 | 45 | 20.30 ms | 43.7 ms |
| toshiba_14af43 | cpu | 55.79 | 59.90 | 42 | 25.78 ms | 44.4 ms |
| toshiba_14af43 | gpu | 57.27 | 60.08 | 28 | 21.86 ms | 43.7 ms |
| toshiba_14af43 | gpu-readback | 52.29 | 60.08 | 82 | 21.82 ms | 43.4 ms |
| stass_favourite | cpu | 56.42 | 60.13 | 37 | 24.29 ms | 43.4 ms |
| stass_favourite | gpu | 53.77 | 60.10 | 64 | 26.21 ms | 43.5 ms |
| stass_favourite | gpu-readback | 52.69 | 60.07 | 78 | 23.01 ms | 44.1 ms |

[Raw playback metrics and contention metadata](gpu-playback-results.json). The later runs lost pictures despite maintaining approximately 60.1 Hz emulation/audio; further profiling is required. These results do not justify claiming that every preset holds 60 FPS under contention.

Implemented optimizations include acquisition before taking the latest emulation picture, source-weighted precomputed beam kernels, cached mask coverage, and fenced capture with half-float transfer lookup and buffered row writes. Diagnostic readback still stalls the render thread; its cost is reported separately. Frame-age timestamps end at submission, not panel scanout.

```sh
python3 frontends/gpu/tests/benchmark_playback.py build/bin/mynes_gpu game.nes /tmp/full-playback
```
