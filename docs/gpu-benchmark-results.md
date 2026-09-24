# GPU performance — 24 September 2026, decoded border

Apple M5, 24 GiB, Metal, Release build, validation off, the method below (twelve warmups, 60 fenced frames per size, panel mask mode). The comparison is the parent commit's build against the build that decodes the receiver's whole active raster (the border around the picture): 15.0% more samples through the matrix, RGB amplifiers, rail load, gun current and horizontal spot spread on NTSC, 34.1% on PAL. Other sessions kept the load average at 6 to 9 during the runs, so each binary ran twice, alternating, and the table gives the faster median of the two.

| Preset | 640×480 | 1280×960 | 1920×1440 | 2560×1920 |
|---|---:|---:|---:|---:|
| Sony PVM-14L2 | 2.611 → 2.743 ms | 3.901 → 3.831 ms | 5.351 → 5.543 ms | 7.602 → 7.923 ms |
| JVC D-Series | 3.824 → 4.125 ms | 5.127 → 5.441 ms | 7.311 → 7.644 ms | 10.537 → 10.559 ms |
| Toshiba 14AF43 | 3.942 → 4.167 ms | 5.275 → 5.467 ms | 7.581 → 7.645 ms | 10.478 → 10.668 ms |
| Stas's Favourite | 3.889 → 4.179 ms | 5.337 → 5.583 ms | 7.755 → 7.974 ms | 11.010 → 11.249 ms |

Four alternated `--benchmark` runs of the PVM on a Super Mario Bros. state give the steadier figure: 2.502 → 2.584 ms at 640×480 and 7.502 → 7.613 ms at 2560×1920, 0.07 to 0.11 ms a frame. The presets with a loaded video rail (JVC, Toshiba, Stas's Favourite) pay more at small sizes, about 0.2 to 0.3 ms, since the rail's per-line walk is serial and 14% longer. The stages at display resolution are unchanged.

# GPU performance — 23 September 2026

Apple M5, 24 GiB, Metal, Release build, validation off, the same method as the 21 September run below: twelve warmups and 60 individually fenced frames at each resolution, panel mask mode, offscreen scale 1:1, no other MyNES frontend running. One ffmpeg encode held one CPU core during the run (load average 6.4 before, 5.9 after); the GPU was otherwise idle. [Raw chain measurements and load metadata](gpu-benchmark-results.json).

| Preset | 640×480 median | 1280×960 median | 1920×1440 median | 2560×1920 median / p95 / max |
|---|---:|---:|---:|---:|
| Sony PVM-14L2 | 2.574 ms | 3.564 ms | 6.678 ms | 11.218 / 11.974 / 12.116 ms |
| JVC D-Series | 4.492 ms | 5.532 ms | 8.875 ms | 13.439 / 13.923 / 14.246 ms |
| Toshiba 14AF43 | 4.233 ms | 5.395 ms | 8.908 ms | 13.515 / 13.731 / 13.868 ms |
| Stas's Favourite | 4.242 ms | 5.936 ms | 9.508 ms | 14.619 / 14.976 / 15.199 ms |

The chain is slower than on 21 September at every size above 640×480, and the gap grows with the pixel count:

| Preset | 2560×1920, 21 Sep | 2560×1920, 23 Sep | Change | 640×480, 21 Sep | 640×480, 23 Sep |
|---|---:|---:|---:|---:|---:|
| Sony PVM-14L2 | 7.285 ms | 11.218 ms | +54% | 2.700 ms | 2.574 ms |
| JVC D-Series | 9.363 ms | 13.439 ms | +44% | 3.653 ms | 4.492 ms |
| Toshiba 14AF43 | 10.731 ms | 13.515 ms | +26% | 3.898 ms | 4.233 ms |
| Stas's Favourite | 10.993 ms | 14.619 ms | +33% | 4.009 ms | 4.242 ms |

The extra time is in the display pass, whose per-pixel work grew between the two runs: each colour is now sampled at the panel's own subpixel (three fetches per pixel in place of one), the exact stripe grille is evaluated per subpixel, the output shoulder spills what a stripe cannot show into its triad's other pixels, and the mask peak and white level are measured rather than estimated (commits 1e3d37d to b923f46). About 0.8 ns per output pixel at 2560×1920, or 3.9 ms of the 11.2 ms. The signal stages before the display pass are unchanged, which the flat 640×480 numbers show. No optimisation was attempted for this run.

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

After the optical, overdrive-beam and complex-IF changes, Super Mario Bros.
was measured at a 3840×2160 output target (2880×2160 active 4:3 picture). Each
run used 300 emulated frames, 60 warmups, GPU audio with deadline fallback, and
one full image readback per 60 frames. No applicable signal, CRT or display
stage was disabled. The separate VHS preset includes its recording/playback stage.
These are sequential offscreen runs: the GPU is fenced, the audio queue is
active and muted, and physical presentation/vsync is not measured.

| Preset | Render / emulated FPS | Skipped | p95 cadence | p95 audio queue |
|---|---:|---:|---:|---:|
| sony_pvm_14l2 | 60.13 / 60.13 | 0 | 19.19 ms | 43.4 ms |
| jvc_d_series_2000 | 60.07 / 60.07 | 0 | 19.43 ms | 43.6 ms |
| toshiba_14af43 | 60.14 / 60.14 | 0 | 19.07 ms | 43.6 ms |
| stass_favourite | 60.10 / 60.10 | 0 | 18.84 ms | 43.5 ms |
| vhs_sp_consumer | 60.12 / 60.12 | 0 | 18.04 ms | 43.6 ms |

[Raw UHD measurements](gpu-uhd-playback-results.json) include timing distributions,
readback/write costs, actual GPU-audio share and contention metadata. This final run missed no pictures. The preceding RF and VHS runs missed one and two respectively; their summaries are retained in the raw report. CPU audio fallback remained active when GPU work missed its deadline. These short shared-machine runs do not
establish zero-drop playback or an optimization speedup. No additional MyNES
frontend ran during this batch.

The new IF and tape FIRs cache overlapping inputs in workgroup memory. The IF
also pairs symmetric real/antisymmetric imaginary taps. This reduces redundant
memory reads and arithmetic while retaining the measured response. Optional
slow phosphor history is allocated and processed only when enabled. Earlier
pipeline-only timings remain above as historical comparisons; they exclude
core/audio/presentation and should not be substituted for this complete run.

On the user's 120 Hz MacBook, a BFI trace contains a continuous 126.77-second
segment at 119.78 submissions/s, with 120.02/s over the last 1,000 submissions.
Each dark slot reuses its bright slot's source frame. The user reported a visible
motion improvement. Submission timestamps and subjective viewing do not measure
physical scanout or panel response. See [BFI controls and limitations](gpu-controls.md#high-refresh-presentation).

## Lossless filter optimization (2026-09-21)

The scalar FIR and horizontal beam-spread shaders now load overlapping input
windows once into workgroup memory. The FIR also skips per-tap edge checks for
interior samples. Tap counts, coefficients, accumulation order, edge rules,
signal resolution, beam broadening and frame phase are unchanged. Longer FIRs
and decimating FIRs retain a direct-buffer path.

Measured against `2df9cbf` on Apple M5, 24 GiB, Metal, Release, validation off.
Each run uses 12 warmup frames and 60 measured frames per resolution. The table
shows the average of two independently measured medians before and after;
the final repeat ran optimized then baseline to check run-order effects.
All runs were sequential with no other MyNES process. The metric includes the
complete video chain through the final display pass and GPU fence, excluding
emulation, audio, presentation waits and captures.

| Look | 1280×960 before → after | 2560×1920 before → after |
|---|---:|---:|
| Reference composite | 3.177 → 3.013 ms (5.2%) | 6.785 → 6.651 ms (2.0%) |
| Basement TV | 5.352 → 5.157 ms (3.7%) | 10.587 → 10.387 ms (1.9%) |
| Sony PVM-14L2 | 3.896 → 3.639 ms (6.6%) | 7.313 → 7.085 ms (3.1%) |
| VHS SP consumer | 7.061 → 6.735 ms (4.6%) | 12.335 → 12.014 ms (2.6%) |

Savings at 1280×960 through 2560×1920 were 0.13–0.35 ms per frame. At 640×480,
the measured reductions were 5.6–10.6%, but those runs had more startup/desktop
variability. These are local full-chain measurements, not a guarantee of the
same gain on other GPUs or a measurement of visible presentation cadence.

Fidelity checks:

- All 21 looks: byte-identical PPM and linear PFM captures for two consecutive
  frames at 960×720, using the mixed-color/detail chart (84 files).
- Reference composite, Basement TV, PVM-14L2 and VHS SP: byte-identical PPM and
  linear PFM pairs at 2560×1920 with chroma transitions and fine neutral detail
  after 32 frames (16 files).
- GPU FIR regression fixtures cover scanline/frame reflection, NTSC/PAL line
  widths, asymmetric/even taps, short inputs, partial workgroups, decimation
  and the long-filter fallback, with exact binary-fraction results.
- Beam tests cover energy conservation, current-dependent spot growth and
  flat fields across short/partial scanlines at the maximum 32-sample radius.
- `gpu_fidelity_tests`, `gpu_pipeline_test`, `gpu_kernel_tests` and
  `gpu_signal_precompute_tests` pass.

The generated MSL and SPIR-V are updated alongside GLSL. Runtime validation and
image comparisons used Metal; Vulkan performance has not been measured.
[Raw runs, shader hashes and capture hashes](gpu-filter-optimization-results.json)
include the intermediate FIR-only run as well as both final repeats. Its VHS SP
capture hashes record the stage that existed then; the FM deck model that
replaced it on 2026-09-23 renders different pictures.

## Scanline parallelism (2026-09-21)

The next pass, against `c5ebbc0`, changes how work is distributed:

- RC filtering, sync/burst detection and CRT loading use 32-invocation groups
  instead of 256. Independent scanlines occupy more schedulable groups. The
  sequential recurrence and arithmetic within each line are unchanged.
- AGC uses a complete 256-invocation group per scanline. One lane computes
  sync/porch measurements and updates the original gain history. After a
  barrier, all lanes apply that gain to different samples. Detector sums and
  attack/release updates retain their original order.
- Receiver PLL and shared CRT-supply history remain ordered across lines.
  Stage dependencies, frame queue depth and presentation timing are unchanged.

On the same M5/Metal machine, two baseline and two final runs produced the
following averages of per-run medians. Each run again used 12 warmups and 60
measurements per size; the final pair ran optimized then baseline.

| Look | 1280×960 before → after | 2560×1920 before → after |
|---|---:|---:|
| Reference composite | 3.016 → 2.976 ms | 6.684 → 6.667 ms |
| Basement TV | 5.164 → 4.750 ms | 10.463 → 10.092 ms |
| Sony PVM-14L2 | 3.638 → 3.614 ms | 7.131 → 7.103 ms |
| VHS SP consumer | 6.758 → 6.593 ms | 14.389 → 14.178 ms |

The clearest gain is RF/AGC: Basement TV saves 0.37–0.41 ms at 1280×960
through 2560×1920, or 3.5–8.0% of the complete video-chain time. The much
smaller Reference/PVM differences are close to measurement noise. Shared-host
contention was higher during this batch (especially the tails and 640×480
startup), so compare these paired runs, not their absolute times against
earlier sections. The 640×480 Reference result was 3.5% slower on the average
of medians, with substantial variation between runs. A separate 32×4 beam
workgroup experiment gave no convincing improvement and was discarded.

All 84 PPM/linear-PFM files for consecutive frames across all 21 looks at
960×720 match the baseline byte-for-byte. GPU tests now also check every RC
and AGC output sample across complete NTSC/PAL rasters, AGC bootstrap/release/
attack history, and sync/burst measurements beyond the first workgroup and
through vertical retrace. All four relevant GPU test suites pass. Vulkan
runtime/performance has not been measured.

[Raw paired runs and capture/shader hashes](gpu-scanline-optimization-results.json)
record this pass separately from the earlier filter optimization.
