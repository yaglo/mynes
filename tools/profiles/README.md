# Core sampling workloads

Run from the repository root. Build an optimized binary with debug symbols:

```sh
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -g'
cmake --build build-profile --target bench_core -j
```

Save the original binary **before** editing emulation code, rebuild the candidate,
then capture both serially. Each round reverses binary order to reduce order bias:

```sh
cp build-profile/bin/bench_core /private/tmp/nes-baseline
dsymutil /private/tmp/nes-baseline -o /private/tmp/nes-baseline.dSYM
# Edit the core, rebuild, and run correctness tests before comparing.
cp build-profile/bin/bench_core /private/tmp/nes-candidate
dsymutil /private/tmp/nes-candidate -o /private/tmp/nes-candidate.dSYM
python3 tools/profile_matrix.py tools/profiles/core.json /private/tmp/nes-samples \
  --binary baseline=/private/tmp/nes-baseline \
  --binary candidate=/private/tmp/nes-candidate --rounds 3
```

The output directory must be new. Instruments permissions may be required. The
runner captures each process to completion (60-second safety limit), exports its
active CPU samples, and writes `results.json`, resolved workload arguments, and
binary SHA-256 hashes. It rejects incomplete runs and differing checksum/audio
summaries. It prints median and min/max sampled CPU milliseconds after all rounds.
Raw traces, XML, and stdout remain available for inspection.
Preserve each binary's dSYM before the next rebuild overwrites its object files;
otherwise inline/source attribution for a saved binary may be unavailable or stale.

All samples, including the identical warmup, are included in fixed-work totals.
Use `profile_samples.py TRACE.xml` to inspect hotspots with the default two-second
startup exclusion, or `--skip-seconds 0 --json` for full-process comparison data.
These are CPU samples, not wall-clock FPS benchmarks. CPU placement, frequency,
thermal state, and cache contention can still affect results: inspect repeated
distributions and reject apparent gains that do not reproduce.

`core.json` uses only ROMs already in this repository: NTSC/PAL Fifteen Puzzle,
MMC1 and MMC3 demos, a UxROM PCM demo, NEStress, and AccuracyCoin. The MMC3 demo
is a graphics workload and was silent in the initial survey. The AccuracyCoin
workload is fixed at 2,000 frames; it is not a full conformance run. Run
`ctest --test-dir build-profile -R accuracy_coin_tests --output-on-failure`
separately to check all 144 tests.

`games.example.json` adds locally supplied commercial ROMs. No commercial ROM data
is included here. Its filenames match the user's local collection; adapt them to
your copies as needed:

```sh
export NES_ROM_DIR="$HOME/Downloads/NES Mega Pack"
python3 tools/profile_matrix.py tools/profiles/games.example.json /private/tmp/nes-game-samples \
  --binary baseline=/private/tmp/nes-baseline \
  --binary candidate=/private/tmp/nes-candidate --rounds 3
```

The example covers mapper 0 (Super Mario Bros.), mapper 1 (Metroid), mapper 2
(PCM demo), mapper 4 (Super Mario Bros. 3 world map), PAL gameplay, and active
audio. The SMB and Metroid replays were visually checked at frame 700 to verify
gameplay. The SMB3 replay reaches the world map; it is not a verified level replay.

## Controller replay and region selection

`bench_core` runs 120 warmup frames, optionally taps Start for two frames with
`--start`, then applies replay events relative to the first measured frame.
Each line contains a decimal frame number and a hexadecimal controller mask.
Frames must increase strictly and fall within the requested run. The mask remains
held until the next event; zero releases all buttons.

| Button | Mask |
|---|---|
| A / B | `01` / `02` |
| Select / Start | `04` / `08` |
| Up / Down | `10` / `20` |
| Left / Right | `40` / `80` |

The puzzle replay enables the auto-solver and later changes inputs. Its PAL build
does not declare PAL in its iNES header, so `--pal` is required. `--ntsc` is also
available for explicit overrides. Unsupported mappers are rejected rather than
silently profiled as mapper 0.

`audio_energy` sums squared samples; unlike a signed sample sum, it identifies
non-silent output even when positive and negative values cancel. An optional
`--frame-output FILE.ppm` writes the final RGB image after the timed frame loop
for workload inspection. Do not enable image output in comparison captures.
