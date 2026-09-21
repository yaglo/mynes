# GPU performance — 21 September 2026

Apple M5, 24 GiB, Metal, Release build, validation off. All runs were sequential with no other MyNES frontend detected. Desktop/UI-test activity remained; these are observations on a shared machine, not isolated laboratory measurements.

## Complete GPU chain

CPU submission through the final GPU fence, including code upload, DAC waveform, receiver, beam, phosphors, mask and glass. Twelve warmups and 60 individually fenced frames at each resolution. This measures the GPU path separately from emulation, audio, vsync and readback; the real-game tests below include those first two workloads and optional readback.

| Preset | 640×480 median | 1280×960 median | 1920×1440 median | 2560×1920 median / p95 / max |
|---|---:|---:|---:|---:|
| Sony PVM-14L2 | 2.700 ms | 3.580 ms | 5.072 ms | 7.285 / 8.985 / 9.060 ms |
| JVC D-Series | 3.653 ms | 4.712 ms | 6.496 ms | 9.363 / 11.078 / 11.492 ms |
| Toshiba 14AF43 | 3.898 ms | 4.856 ms | 6.877 ms | 10.731 / 11.241 / 11.441 ms |
| Stas's Favourite | 4.009 ms | 4.949 ms | 7.025 ms | 10.993 / 11.538 / 11.678 ms |

Panel mask mode, offscreen scale 1:1. A 2560×1920 target exceeds this laptop's 2560×1664 panel; it is a workload measurement. [Raw chain measurements and load metadata](gpu-benchmark-results.json).

### Geometry optimization, controlled comparison

Unchanged deflection maps are reused only when every load/time/audio-dependent geometry control is zero. Parameters, output size, buffer replacement and temporal resets invalidate the cache. The consumer profiles retain their active load response. Zero-valued optional effects also bypass unnecessary shader calculations.

A cached / recomputed / recomputed / cached sequence with the same binary and PVM preset measured:

| Resolution | Cached medians | Recomputed medians | Reduction from average medians |
|---|---:|---:|---:|
| 640×480 | 2.665, 2.673 ms | 2.752, 3.269 ms | 11.3% |
| 1280×960 | 3.571, 3.558 ms | 3.873, 3.913 ms | 8.4% |
| 1920×1440 | 5.098, 5.091 ms | 5.836, 5.846 ms | 12.8% |
| 2560×1920 | 7.158, 7.112 ms | 8.444, 8.443 ms | 15.5% |

[Raw A/B runs](gpu-geometry-cache-results.json). At 2560×1920 the gain is about 15.5%. PVM, Toshiba and Stas linear Contra captures remained bit-identical to the pre-optimization build. JVC's arithmetic regrouping changed a few half-float values: maximum difference 0.00097656, RMS 0.0000022. Direct offscreen readback versus rerendering for capture is bit-identical for both phases of all four presets. GPU tests cover parameter invalidation and continued visualizer snapshots while geometry is cached.

## Full real-game playback

Contra, 720 emulated frames per run, 120 warmup frames, offscreen 2560×1664. Includes core, APU, CPU/GPU audio processing with an active muted SDL queue, and every signal/CRT/display stage. The capture mode writes PPM and linear PFM once per 60 emulated frames, with one bounded encoding/writer thread that is joined before exit. Process CPU accounting includes that worker. No window/vsync is exercised in the user-requested offscreen mode. Timestamps end at submission, not panel scanout.

| Preset | Audio / capture | Render FPS | Emulated FPS | Skipped pictures | p95 cadence | p95 audio queued |
|---|---|---:|---:|---:|---:|---:|
| Sony PVM-14L2 | cpu | 60.10 | 60.10 | 0 | 20.38 ms | 43.6 ms |
| Sony PVM-14L2 | gpu | 60.09 | 60.09 | 0 | 19.70 ms | 43.4 ms |
| Sony PVM-14L2 | gpu-readback | 60.12 | 60.12 | 0 | 20.25 ms | 43.4 ms |
| JVC D-Series | cpu | 60.10 | 60.10 | 0 | 20.43 ms | 43.8 ms |
| JVC D-Series | gpu | 60.11 | 60.11 | 0 | 20.66 ms | 43.4 ms |
| JVC D-Series | gpu-readback | 60.12 | 60.12 | 0 | 20.21 ms | 43.4 ms |
| Toshiba 14AF43 | cpu | 60.09 | 60.09 | 0 | 20.86 ms | 43.5 ms |
| Toshiba 14AF43 | gpu | 60.06 | 60.06 | 0 | 21.09 ms | 43.6 ms |
| Toshiba 14AF43 | gpu-readback | 60.13 | 60.13 | 0 | 20.00 ms | 43.4 ms |
| Stas's Favourite | cpu | 60.08 | 60.08 | 0 | 20.16 ms | 43.5 ms |
| Stas's Favourite | gpu | 60.10 | 60.10 | 0 | 19.88 ms | 43.4 ms |
| Stas's Favourite | gpu-readback | 60.15 | 60.15 | 0 | 20.46 ms | 43.7 ms |

The current repeats delivered every picture at approximately 60.1 Hz. Earlier sequential runs of Toshiba and Stas dropped frames under different host load; this is not a guarantee under contention. Host-refresh pacing still requires an onscreen check. [Raw playback runs, timing distributions and contention metadata](gpu-playback-results.json).

### Screenshot stalls

The old capture rerendered the final display and blocked the render thread for conversion and disk writes. Offscreen capture now downloads the existing final target; encoding/writing uses one owned CPU image in a background thread. A slow writer is joined before accepting another image, keeping memory bounded. Batch `--screenshot-after` captures remain synchronous. Write errors propagate through shutdown.

| Preset | Before capture FPS | Current capture FPS | Before render-thread capture median | Current readback/copy median | Background encode/write median |
|---|---:|---:|---:|---:|---:|
| Sony PVM-14L2 | 55.83 | 60.12 | 94.37 ms | 18.85 ms | 66.12 ms |
| JVC D-Series | 56.74 | 60.12 | 83.87 ms | 19.82 ms | 58.62 ms |
| Toshiba 14AF43 | 55.77 | 60.13 | 86.30 ms | 18.99 ms | 57.05 ms |
| Stas's Favourite | 54.75 | 60.15 | 123.16 ms | 19.85 ms | 60.03 ms |

All 44 requested writes succeeded. Background write cost has moved off the render thread, not disappeared. Readback itself still waits for GPU completion. Unit checks compare synchronous/asynchronous files and cover ownership, HDR, row orientation, RGB/BGR order and failure reporting.

Audio playback also passed a deliberate 250 ms render-thread stall: both backends produced 440,216 samples, maximum queued audio about 50.3 ms, and maximum CPU/GPU sample difference 0.00001341. This checks bounded queueing and processing continuity, not measured acoustic latency.

```sh
python3 frontends/gpu/tests/benchmark_pipeline.py build/bin/mynes_gpu /tmp/gpu-bench
python3 frontends/gpu/tests/benchmark_pipeline.py build/bin/mynes_gpu /tmp/pvm-uncached sony_pvm_14l2 --recompute-geometry
python3 frontends/gpu/tests/benchmark_playback.py build/bin/mynes_gpu game.nes /tmp/full-playback
```

## UHD playback and high-refresh presentation

After the voltage-range, HDR colour and gun-bias corrections, Super Mario Bros. playback
was measured at a 3840×2160 output target (2880×2160 active 4:3 picture). Each
run used 300 emulated frames, 60 warmups, GPU audio and one full image readback
per 60 frames. All source, signal, CRT and display stages remained enabled.
These are sequential offscreen runs: the GPU is fenced, the audio queue is
active and muted, and physical presentation/vsync is not measured.

| Preset | Render / emulated FPS | Skipped | p95 cadence | p95 audio queue |
|---|---:|---:|---:|---:|
| Sony PVM-14L2 | 59.80 / 60.05 | 1 | 19.09 ms | 44.4 ms |
| JVC D-Series | 60.09 / 60.09 | 0 | 19.50 ms | 43.6 ms |
| Toshiba 14AF43 | 60.10 / 60.10 | 0 | 19.04 ms | 43.5 ms |
| Stas's Favourite | 60.10 / 60.10 | 0 | 20.23 ms | 43.7 ms |

[Raw UHD measurements](gpu-uhd-playback-results.json). A separate larger
3840×2880 target dropped presented frames with Stas's Favourite; that workload
is 11.1 million active pixels versus UHD's 6.2 million active picture pixels.
The current PVM run dropped one picture; the previous run dropped none. These
short runs on a shared machine do not establish a speedup or a regression from
the physics corrections. No additional MyNES frontend ran during this batch.

On the user's 120 Hz MacBook, a BFI trace contains a continuous 126.77-second
segment at 119.78 submissions/s, with 120.02/s over the last 1,000 submissions.
Each dark slot reuses its bright slot's source frame. The user reported a visible
motion improvement. Submission timestamps and subjective viewing do not measure
physical scanout or panel response. See [BFI controls and limitations](gpu-controls.md#high-refresh-presentation).
