# Expanded core profiling — 2026-09-20

Baseline core: `9034da6`. Both variants use the same extended benchmark harness
and Release `-O3 -DNDEBUG -g` build. The GPU frontend is outside this experiment.

## Workload coverage

The benchmark now supports deterministic controller replay, explicit PAL/NTSC,
non-silent audio verification through sample energy, and optional framebuffer
capture. Unsupported mappers are rejected. All captures use 2,000 measured frames
plus the same 120-frame warmup; `--start` adds two identical startup frames where
specified. Audio synthesis is enabled for every workload.

| Workload | Mapper | Region | What is exercised |
|---|---:|---|---|
| Fifteen Puzzle | 0 | PAL | Auto-solver gameplay and sound; explicit PAL override |
| PCM demo with graphics | 2 | NTSC | UxROM, graphics, sustained nonzero audio |
| NEStress | 0 | NTSC | Rendering stress, silent audio output |
| AccuracyCoin | 0 | NTSC | First 2,000 frames of suite execution; separate full conformance run |
| Super Mario Bros. (JU, PRG 0) | 0 | NTSC | World 1-1 with timed movement/jumps and music |
| Metroid (U) | 1 | NTSC | Gameplay with timed inputs and music |
| Super Mario Bros. 3 (U, PRG 0) | 4 | NTSC | World-map workload with music; not a verified level replay |

SMB and Metroid were visually checked at frame 700. Commercial ROMs were read
from the user's local collection; none are included in the repository. Checked-in
input scripts reproduce the captures. An early SMB survey had an extra Start
press that paused gameplay; the repeated comparison uses the corrected replay.

The initial survey placed the PPU at roughly 53–68% of active CPU samples across
these workloads. It also confirmed nonzero audio energy in every workload except
NEStress. APU sample share ranged roughly 8–13%; it was not the dominant cost.

## Broad candidate: rejected

The first candidate combined bitwise nametable mirroring, direct internal palette
lookup, and a rewrite of pixel-priority selection. It passed the core conformance
suite, but its performance did not generalize.

Three serial rounds reversed baseline/candidate order each round. The table is
the median of the three paired changes in **active sampled CPU milliseconds**.
Positive values mean more sampled CPU work. Full per-capture data, including
matching output signatures, is in [profiling-round2-broad.csv](profiling-round2-broad.csv).

| Workload | Median paired change | Range across three pairs |
|---|---:|---:|
| Fifteen Puzzle PAL | +1.8% | +1.6% to +60.1% |
| PCM demo | +17.0% | +2.8% to +53.7% |
| NEStress | -9.7% | -14.2% to +1.9% |
| AccuracyCoin | -9.1% | -15.5% to +1.5% |
| Super Mario Bros. | -8.5% | -33.9% to -7.7% |
| Metroid | +7.9% | -9.9% to +20.0% |
| Super Mario Bros. 3 world map | +7.1% | +0.8% to +156.8% |

All 42 capture outputs match within their workload: frame count, audio sample
count, signed audio sum, audio energy, RAM/framebuffer checksum, mapper, and region.
These are repeatability checks, not substitutes for the conformance tests.

The large variation is real measurement uncertainty. CPU sampling excludes time
descheduled, but cannot remove frequency, core-placement, thermal, or cache effects.
For example, an early PAL baseline moved among both E and S cores, whereas a later
baseline stayed on S cores. These results do not justify a universal speedup claim.

## Narrower follow-up: also rejected

The pixel-priority rewrite was removed. The follow-up kept the original priority
logic and changed only horizontal/vertical nametable address calculation and
internal palette lookup. Transparent output still selects palette entry zero;
opaque pixels cannot address a mirrored universal-color entry. Bus clocks, reads,
mapper callbacks, sprite-zero timing, and palette effects retain their semantics.

Each follow-up binary has its own preserved dSYM, generated before another rebuild
can replace its object files. This matters for reliable inline/source attribution;
the broad experiment's full-process sample totals do not depend on symbolication.

Another three alternating paired rounds on the pixel-heavy workloads produced:

| Workload | Median paired CPU change | Range across three pairs |
|---|---:|---:|
| Fifteen Puzzle PAL | +44.6% | +11.5% to +69.4% |
| Super Mario Bros. | +3.6% | -4.2% to +9.7% |
| Metroid | -6.2% | -43.2% to +4.7% |

These are observations under heavy contention, not isolated causal estimates of
the code's cost. Early follow-up captures ran almost entirely on E cores. Even in
the final PAL pair, however, both variants ran almost exclusively on S cores and
the candidate still used 11.5% more sampled CPU time. There is no repeatable win
that justifies retaining this variant. Full data and core-placement percentages
are in [profiling-round2-focused.csv](profiling-round2-focused.csv).

**Both PPU experiments were reverted.** The emulator core remains at `9034da6`.
Retained changes are the profiling harness, workload/replay files, sample reports,
and regression tests. This pass establishes broader coverage and rejects unsafe
performance conclusions; it does not claim another speedup.

## Correctness checks

New PPU regressions cover every nametable address from `$2000` through `$3EFF`
across horizontal, vertical, both single-screen modes, and four-screen behavior.
Pixel tests cover transparent/opaque backgrounds and sprites, all four palettes
for each, both priorities, greyscale, all emphasis combinations, and sprite-zero
suppression at X=255, checking both RGB and index output.

Both candidate variants passed the six core suites, including 144/144 AccuracyCoin.
All 60 paired captures matched their workload's output signature. The narrow
variant also matched the baseline on all seven workloads in standalone checks.
After reverting both variants, the restored core passed all six suites again,
including 22/22 PPU tests and 144/144 AccuracyCoin. The final build had no reported
compiler warnings or errors.

The replay parser rejects unordered/out-of-range frame events and invalid button
masks. The benchmark rejects unsupported mapper ROMs. Frame inspection and audio
energy guard against accidentally profiling only a title screen or silent output.

## Reproduce

See [the workload instructions](../../tools/profiles/README.md). The commercial
ROM example is `tools/profiles/games.example.json`; `core.json` is repository-only.
The profile runner preserves binary hashes, resolved arguments, raw traces, XML,
stdout, and JSON statistics, and rejects incomplete runs or output mismatches.

Local capture directories:

- `/private/tmp/mynes-round2-survey`: one initial capture per workload.
- `/private/tmp/mynes-round2-comparison`: three paired rounds of the rejected candidate.
- `/private/tmp/mynes-round2-focused-results`: narrower follow-up on PAL, SMB, and Metroid.

The old single-pair results in [profiling.md](profiling.md) remain preliminary.
The broader experiment demonstrates why neither wall-clock FPS nor one pair of
active CPU sample totals should be treated as a reliable speedup under contention.
