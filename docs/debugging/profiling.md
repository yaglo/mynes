# Core CPU sampling — 2026-09-20

See [expanded gameplay and repeated comparisons](profiling-round2.md) for the follow-up experiment.

Baseline: `8c38bd4`, Apple Silicon MacBook Air, macOS 27.0, AppleClang,
Release `-O3 -DNDEBUG -g` with the project's native architecture flags.
Instruments Time Profiler sampled the unpaced `bench_core` executable.
Audio synthesis was enabled through a sample callback; there was no SDL,
audio playback, frame pacing, or GPU rendering in these core measurements.

The machine was running other work. These are shares of **active CPU samples**,
not wall-clock throughput or predicted speedups. Contention can still affect
cache behavior and CPU placement. Both workloads are mapper 0; commercial games,
other mappers, PAL, and sustained non-silent audio need separate captures.

## Results

The first two seconds of each recording were excluded. NEStress ran with a
20,000-frame limit and was stopped by the 15-second recording limit; AccuracyCoin
ran for 5,000 frames and exited. Both warmed up for 120 frames, then held Start
for two frames. AccuracyCoin is a fixed-duration workload, potentially including
the result screen; this benchmark does not verify suite completion or correctness.

| Self-sample group | NEStress (13,725 samples) | AccuracyCoin (6,426 samples) |
|---|---:|---:|
| PPU | 70.50% | 56.04% |
| CPU | 8.78% | 16.78% |
| APU | 7.40% | 7.95% |
| Mapper | 2.54% | 3.11% |
| System glue and other | 10.79% | 16.12% |

Groups use the sampled leaf function's prefix. Inlined functions may appear as
separate symbols; these are exclusive shares, not inclusive call-tree totals.
System glue includes `nes_step`, memory access, DMA, and tracing bookkeeping.

| Sampled function | NEStress | AccuracyCoin |
|---|---:|---:|
| `ppu_step` | 38.70% | 36.82% |
| `ppu_render_pixel` | 11.27% | 4.42% |
| `ppu_advance_to_master_tick` | 5.80% | 6.35% |
| `cpu_microcycle` | 6.89% | 13.46% |
| `ppu_sprite_evaluation` | 3.71% | 2.30% |
| `nes_cpu_step_traced` | 1.40% | 1.74% |

## Optimization priorities

1. **PPU sprite loops and pixel selection.** Source-resolved NEStress samples
   concentrate around `src/ppu/ppu.h:1130` (pattern shifts), `:1163` (position
   counters), and `:956` (pixel selection). Investigate hoisting common conditions,
   reducing repeated array walks, and compiler vectorization. Preserve pixel
   selection before shifting, counter-enable latch timing, and counter advancement
   while rendering is disabled. These details are covered by AccuracyCoin.
2. **Per-dot PPU dispatch.** `ppu_step` and `ppu_advance_to_master_tick` dominate
   both captures. Investigate a cheaper common visible-dot path with uncommon
   events separated. Do not skip bus clocks, sprite evaluation, register effects,
   or NMI edges merely because no pixel is visible. Sample after each small change.
3. **Disabled tracing work.** `src/nes/nes.h:420` re-reads the completed instruction's
   opcode before `HOOK_CPU_STEP` checks whether a callback exists. Guard the opcode
   lookup with the callback check. Keep instruction-PC and interrupt bookkeeping
   correct when hooks are installed dynamically. The wrapper's self share is small;
   some lookup cost is attributed to mapper functions, and neither is all removable.
4. **CPU dispatch after the PPU work.** `cpu_microcycle` matters more on AccuracyCoin
   than NEStress. Inspect generated code and instruction-cache behavior before
   changing the DSL or dispatch strategy. Audio is not the leading target in these
   captures, but the workloads do not establish its cost during music playback.

These were the initial measured priorities and code-review hypotheses.
Any core optimization must retain the 144/144 AccuracyCoin baseline and relevant
CPU/PPU/DMA regression tests. Compare new active-sample profiles; use controlled,
repeated throughput runs later to establish actual speedups.

## Implemented optimization pass

- Instruction opcode lookup now runs only when `on_cpu_step` is installed.
  Instruction-PC bookkeeping remains active, so enabling tracing during execution
  still reports the correct instruction.
- Sprite shifting and position-counter updates share one array pass when counters
  are active or pending. Once counters stop, patterns use an unconditional shift
  loop. Shift eligibility uses the old enable latch and old position; counter
  updates use the new latch. Pixel selection no longer computes a redundant offset.
- Rendering dispatch groups visible dots, sprite-fetch dots, and background
  prefetch dots. Bus clocks and register/NMI timing remain outside that dispatch.

The six core CTest suites pass, including **144 PASS, 0 FAIL, 0 NOT RUN** in
AccuracyCoin. New regression tests exercise sprite zero crossings, pending latch
activation, forced blanking, pre-render clocks, and enabling/disabling instruction
tracing while execution continues.

Fixed-work comparisons used 5,000 frames plus the same 120-frame warmup and Start
press in each binary. Unlike the hotspot table above, these totals include all
active samples from process start to normal exit (`--skip-seconds 0`), so a faster
binary does not have more of its work discarded by a fixed startup cutoff.

| Active sampled CPU milliseconds | Original | Tracing + sprites | All three changes |
|---|---:|---:|---:|
| NEStress | 11,132 | 9,611 | 9,726 |
| AccuracyCoin | 7,809 | 7,410 | 7,271 |

The final captures show approximately **13% less sampled CPU work on NEStress**
and **7% less on AccuracyCoin**. These are preliminary single-capture comparisons
on a contended machine, not guaranteed throughput improvements. The dispatch
change's incremental effect is mixed/small and cannot be distinguished confidently
from run variation. It is retained for the simpler shared dot-range checks.

Both final outputs match the original:

| Workload | RAM/framebuffer checksum | Audio samples | Audio sum |
|---|---|---:|---:|
| NEStress | `c7e79832` | 3,668,957 | 0 |
| AccuracyCoin | `08a4618e` | 3,668,971 | 9.65011625e-09 |

Comparison traces are `/private/tmp/mynes-before-stress.trace`,
`/private/tmp/mynes-final-stress.trace`, the original
`/private/tmp/mynes-accuracy-core.trace`, and
`/private/tmp/mynes-final-accuracy.trace`. The intermediate sprite-loop captures
are `/private/tmp/mynes-opt-stress.trace` and
`/private/tmp/mynes-opt-accuracy.trace`.

## Reproduce

```sh
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -g'
cmake --build build-profile --target bench_core -j
xcrun xctrace record --template 'Time Profiler' --time-limit 15s \
  --output /private/tmp/mynes-core.trace \
  --launch -- "$PWD/build-profile/bin/bench_core" \
  "$PWD/tests/nes-test-roms/stress/NEStress.NES" 20000 --start
xcrun xctrace export --input /private/tmp/mynes-core.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output /private/tmp/mynes-core.xml
python3 tools/profile_samples.py /private/tmp/mynes-core.xml
```

Use a fresh trace output path for each recording. Instruments may report a
nonzero exit status when the recording limit terminates its launched target;
check that the trace was saved before exporting. For the second workload,
substitute `tests/accuracy_coin/AccuracyCoin.nes` and `5000 --start`.

`bench_core` enables audio by default because the existing headless frontend
does not install an audio callback, causing the APU to skip output resampling
and filtering. `--no-audio` permits a deliberate comparison. It prints a final
RAM/framebuffer checksum and audio summary for repeatability checks; those are
not a substitute for conformance tests. Two 120-frame NEStress smoke runs matched:
checksum `1b28bf6a`, 88,055 audio samples, audio sum zero.

Local captures from this session are `/private/tmp/mynes-core-profile.trace` and
`/private/tmp/mynes-accuracy-core.trace`; they are not committed and may be removed
by the OS. A GPU frontend capture was also collected, but GPU optimization is
being handled separately and no GPU performance conclusions are drawn here.

Profiling exposed a build issue: shader compilation always copied output into
`build/shaders`, leaving `build-profile/shaders` empty. CMake now supplies the
selected shader output directory to `compile_shaders.sh`, which writes only
there (invoked by hand without an argument it regenerates the committed copies
in the source tree). A rebuilt profiling frontend successfully loaded its
shaders. This is independent of the core hotspot findings.
