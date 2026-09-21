# MyNES picture, sound and internals

Start with the [game showcase](nes-visual-showcase.md): Darkwing Duck gameplay,
Kirby's Adventure, Little Samson, Super Mario Bros. 3 and Mega Man 2, recorded
through MyNES's GPU signal path with individual NTSC phases intact.

[![Darkwing Duck through the PVM model](images/showcase/4k/darkwing-pvm-gameplay.png)](images/showcase/4k/darkwing-pvm-gameplay.png)

The linked image contains a complete **3840×2880 game viewport**. The page preview
is scaled to fit; open the original to inspect its pixels.

## Explore the picture

- [Watch the five-game reel](images/showcase/showcase-reel.mp4) — 20 seconds at 60.1 fps.
- [Gameplay close-ups and beam height](gpu-beam-closeups.md) — native crops, four CRT profiles, measurements and calibration limits.
- [Preset guide and audit](gpu-preset-audit.md) — connections, tube identities and estimated parameters.
- [Signal Studio](../tools/visualiser/README.md) — edit the running chain and manage presets.

## Understand and reproduce it

- [GPU pipeline reference](gpu-pipeline-reference.md) — waveform, receiver, beam, phosphors and host display.
- [Hardware research](gpu-hardware-research.md) — published evidence and remaining approximations.
- [Performance measurements](gpu-benchmark-results.md) — full-path benchmarks and their conditions.
- [Capture workflow](../tools/review/README.md) — deterministic controller replay and linear-light beam measurements.
- [Phase and motion diagnostics](gpu-motion-review.md) and [all-preset Contra comparison](contra-preset-gallery.md) — controlled test scenes, separate from the showcase.

For emulator development, see [architecture](architecture/overview.md),
[developer conventions](dev/conventions.md), and [commands](dev/commands.md).
