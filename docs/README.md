# MyNES documentation

Developer documentation lives here. Galleries, preset reviews, hardware
research notes and the blog series moved to the project site:
[yaglo.github.io/mynes-web](https://yaglo.github.io/mynes-web/).

## Using the emulator

- [Controls, menus, saves and performance settings](gpu-controls.md)
- [Signal Studio, the macOS chain editor](../tools/visualiser/README.md)
- [Build and test commands](dev/commands.md), including Linux and headless recipes
- [Play on a real CRT over USB](crt-usb.md)
- [Release and packaging](dev/release.md)

## How it works

- [Architecture overview](architecture/overview.md)
- [Timing and the master clock](architecture/timing.md)
- [CPU](architecture/cpu.md), [PPU](architecture/ppu.md), [APU](architecture/apu.md),
  [mappers](architecture/mappers.md), [memory map](architecture/memory-map.md)
- [CPU DSL design](dsl-design.md) and [DSL reference](dsl-reference.md)
- [GPU pipeline design](gpu-pipeline-design.md) and [pipeline reference](gpu-pipeline-reference.md)
- [GPU frontend design notes](gpu-frontend-design.md)
- [Interactive pipeline diagram](gpu-pipeline-interactive.html)

## Measurements

- [GPU benchmark results](gpu-benchmark-results.md) with the raw JSON beside it
- [Presentation validation](architecture/gpu-realism-validation.md)
- [Reproducible captures](../tools/review/README.md)

## Working on the code

- [Conventions](dev/conventions.md), [adding features](dev/adding-features.md)
- [Testing infrastructure](architecture/testing.md)
- [Debugging workflows](debugging/common-workflows.md), [hooks](debugging/hooks.md),
  [traces](debugging/traces.md), [CPU trace](debugging/cpu-trace.md),
  [state inspection](debugging/state-inspection.md), [profiling](debugging/profiling.md)
