# MyNES

A NES emulator that generates the console's composite video signal, decodes it
the way a television would, and draws the beam, phosphors, mask and glass of a
CRT. The emulation core is cycle accurate; the picture comes from signal
processing, not from a post-process filter.

[![Kirby's Adventure title through the JVC D-Series preset](docs/images/kirby-jvc.gif)](https://yaglo.github.io/mynes-web/gallery/showcase/)

*Actual output, alternating NTSC phases kept separate. More games, presets and
4K captures on the [project site](https://yaglo.github.io/mynes-web/).*

## What it does

- **Cycle-accurate 6502.** All official and unofficial opcodes are described as
  cycle patterns in a small DSL; a compiler generates the C. The core passes
  144 of 144 [AccuracyCoin](tests/accuracy_coin/README.md) tests and the blargg
  CPU, PPU and APU suites.
- **A signal, not a filter.** PPU colour codes drive a measured DAC model. The
  waveform goes through a cable or RF path, comb or notch Y/C separation, the
  colour decoder and the RGB amplifiers before it reaches the tube model.
- **A tube that responds to the picture.** Beam width grows with current, bright
  areas load the supply, phosphors decay, and aperture grille, slot and shadow
  masks give the texture. Twenty-three presets range from a Sony PVM-14L2 to a
  worn RF set and a VHS recording.
- **Playable.** Gamepads with hot-plug, two players, battery saves, save-state
  slots, fast-forward, NTSC and PAL, an on-screen menu for every control, and a
  native macOS editor for the whole chain.

Supported mappers: 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 34, 66, 69, 71, 206 and 227
(NROM, MMC1, UxROM, CNROM, MMC3, MMC5, AxROM, MMC2, MMC4, Color Dreams,
BNROM/NINA-001, GxROM, FME-7, Camerica, DxROM and the 1200-in-1 board). Unsupported boards report their mapper number
in the ROM browser.

## Download

Builds for macOS (Apple Silicon) are on the
[releases page](https://github.com/yaglo/mynes/releases). Linux builds from
source below. Windows is untested. No ROMs are included.

## Build from source

macOS:

```bash
brew install cmake
git clone https://github.com/yaglo/mynes.git && cd mynes
cmake -S . -B build && cmake --build build -j
./build/bin/mynes_gpu path/to/game.nes
```

The first configure downloads and builds a pinned SDL3. `glslc` and
`spirv-cross` are optional; without them the committed shader binaries are
used.

Linux (Vulkan):

```bash
sudo apt install cmake libvulkan-dev libx11-dev libxext-dev libxrandr-dev \
    libwayland-dev libxkbcommon-dev libpulse-dev libasound2-dev
cmake -S . -B build -DMYNES_BUNDLED_SDL3=ON && cmake --build build -j
./build/bin/mynes_gpu path/to/game.nes
```

See [docs/dev/commands.md](docs/dev/commands.md) for options, the headless CI
recipe, and the SDL2 frontend.

## Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| D-pad, A, B | Arrows, X, Z | D-pad or left stick, East, South |
| Select, Start | Tab, Return | Back, Start |
| Menu | Escape or M | Guide |
| Pause | Space | |
| Fast-forward (hold) | ` (grave) | Right shoulder |
| Save state, load state, next slot | F5, F7, F6 | |
| Reset, room reflections, next preset | R, G, P | |
| ROM browser, fullscreen, screenshot | O, F, F12 | |

Player 2 uses W/A/S/D with J, H, U and Y, or the second gamepad.

The full map, including player 2, gamepad buttons, save states and the
developer keys, is in [docs/gpu-controls.md](docs/gpu-controls.md).

## How the picture is made

```text
PPU colour codes → NES DAC waveform → cable / RF receiver → Y/C separation
                 → colour decoder → RGB amplifiers → beam & phosphors
                 → mask & glass → SDR / HDR display
```

Each stage is a compute shader with a CPU reference kernel and a unit test.
The [pipeline reference](docs/gpu-pipeline-reference.md) lists every stage and
its limits; the [blog series](https://yaglo.github.io/mynes-web/blog/) explains
the ideas: timing as data, what the compiler knows about DMA, the signal nobody
sees, and why the beam is not a line.

Measurements: the full chain renders in about 7 ms per frame for the PVM
preset at 2560×1920 on an Apple M5, and around 11 ms for the consumer sets.
A render-scale option keeps 60 fps on slower GPUs.
[Benchmark methodology](docs/gpu-benchmark-results.md).

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Unit tests, mapper tests, PAL APU ROM tests, the complete AccuracyCoin suite
and the GPU kernel, preset and playback tests run on every push through
GitHub Actions on Ubuntu and macOS.

## Documentation

- [Architecture](docs/architecture/overview.md), [timing](docs/architecture/timing.md),
  [mappers](docs/architecture/mappers.md), [testing](docs/architecture/testing.md)
- [CPU DSL design](docs/dsl-design.md) and [reference](docs/dsl-reference.md)
- [GPU pipeline design](docs/gpu-pipeline-design.md) and [reference](docs/gpu-pipeline-reference.md)
- [Controls and menus](docs/gpu-controls.md), [Signal Studio](tools/visualiser/README.md)
- [Debugging workflows](docs/debugging/common-workflows.md), [commands](docs/dev/commands.md)
- Galleries, preset reviews, hardware research and the blog:
  [yaglo.github.io/mynes-web](https://yaglo.github.io/mynes-web/)

## Project layout

```text
include/nes/        Public API
src/cpu/            6502 DSL source
src/nes/            System, APU, mappers, ROM loading, composite video
src/ppu/            Dot-accurate PPU
frontends/gpu/      SDL3 signal-chain frontend and shaders
frontends/sdl/      SDL2 frontend
frontends/shared/   Config, ROM browser, saves
tests/              Unit tests, AccuracyCoin, test ROMs
tools/              DSL compiler, test runner, Signal Studio, review scripts
docs/               Developer documentation
```

MyNES began in 2012; the current core is a C11 rewrite. The original is on the
[`legacy`](https://github.com/yaglo/mynes/tree/legacy) branch.
[Contributing](CONTRIBUTING.md) · Apache-2.0 [license](LICENSE)
