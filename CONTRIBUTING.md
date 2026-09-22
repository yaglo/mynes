# Contributing

Thanks for your interest in improving this project. The guidelines below
are meant to keep contributions focused and easy to review.

## Getting started

1. Fork and clone the repo.
2. Build it following [README.md](README.md). The SDL2 frontend is the
   simplest starting point — if that builds and runs a ROM, you're set.
   You need a C11 compiler, CMake 3.16+ and SDL2 and/or SDL3; the GPU
   frontend's shader tools (`glslc` from shaderc, `spirv-cross`, Python 3)
   are optional because the compiled shaders are committed — install them
   only if you edit a `.glsl`, then run the `shaders_regenerate` target
   and commit the results. Chicken Scheme is likewise optional (the
   generated CPU code is committed). Platform recipes are in
   [docs/dev/commands.md](docs/dev/commands.md).
3. Run the test suite: `ctest` from the build directory.

## Scope

Contributions that fit the project well:

- **CPU / PPU / APU accuracy fixes** — backed by an AccuracyCoin page or
  a Blargg test that starts passing. Include before/after pass counts.
- **New mappers** — implement under `src/nes/mappers/` and register in
  `mapper.c`. A representative ROM that exercises the mapper is helpful.
- **Bug fixes in either frontend** — minimal diff preferred.
- **Documentation** — especially filling gaps in the GPU frontend and
  DSL areas.

Less welcome without discussion first:

- Large refactors or architectural rewrites. Open an issue first.
- New frontends. The SDL2 + GPU pair is intentional; a third frontend
  doubles maintenance cost.
- Anything that ties the core to a specific OS or windowing library.

## Coding conventions

- **C11** for the core and both frontends. Keep it warning-clean under
  `-Wall -Wextra -Wpedantic`.
- **Platform-agnostic core** — `src/` and `include/nes/` must not
  reference SDL, OS APIs, or GPU libraries. Those belong in `frontends/`.
- **4-space indent**, no tabs. Line length soft-capped around 100.
- **Comments explain *why*, not *what*.** The identifier name already
  says what; use a comment for a subtle invariant, a workaround for a
  specific bug, or a hidden constraint.
- **No trailing whitespace.** Your editor should handle this.

## Commits

- One logical change per commit. Small, reviewable commits beat large
  omnibus ones.
- Commit message: an imperative subject line, with an optional subsystem
  prefix when it helps. Both `Fix CLI delay on IRQ alignment` and
  `gpu: mask pitch in pixels, not mm` are fine.
- The body explains *why* the change is needed if it isn't obvious
  from the subject line.
- Reference the AccuracyCoin page or test ROM when a change affects
  accuracy numbers.

## Pull requests

- Keep the PR focused on a single change. If you found two bugs, send
  two PRs.
- Include `ctest` output if you touched anything the tests cover.
- For GPU-frontend PRs, describe which preset / connection type you
  tested against. GPU frontend changes should also run the
  `test_gpu_kernels`, `test_signal_precompute`, `test_presets`, and
  `test_preset_json` suites.
- For PPU / CPU changes, include a before/after AccuracyCoin breakdown
  (`./build/bin/accuracy_coin tests/accuracy_coin/AccuracyCoin.nes <page>`
  from the repository root).

## Testing philosophy

- **CPU:** integration tests against real ROMs (Blargg, AccuracyCoin).
  Unit tests are fine for addressing modes and flag behaviour.
- **PPU:** prefer visual diffs via ROM runs over synthetic tests.
- **GPU frontend:** unit-test the signal precompute math, preset
  loading, and individual kernels. Visual correctness is verified by
  running a real ROM through each preset.

## Questions?

Open a GitHub issue with the `question` label. For longer design
discussions, a draft PR with a `WIP:` prefix and a written proposal is
often easier than an issue thread.
