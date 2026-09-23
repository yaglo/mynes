# MyNES

MyNES is an NES emulator that generates the console's composite video signal,
decodes it the way a television would, and draws the beam, phosphors, mask and
glass of a CRT. The emulation core is cycle accurate: it passes 144 of 144
[AccuracyCoin](tests/accuracy_coin/README.md) tests and the blargg CPU, PPU
and APU suites. The picture is computed from the composite signal, with no
post-process filter over the NES frame.

Flicker warning: the preview GIF alternates 2 frames at 25 Hz.

Preview: [Kirby's Adventure title screen on the JVC D-Series preset](docs/images/kirby-jvc.gif),
consecutive frames with the alternating NTSC phases kept separate. The GIF is
640×480 with a fixed palette, scaled down from 960×720 renders, which alters
the mask pattern. The [Gallery](https://yaglo.github.io/mynes-web/gallery/) of
the [project site](https://yaglo.github.io/mynes-web/) has more games, presets
and 3840×2880 captures.

## Features

- CPU: all official and unofficial 6502 opcodes are described as cycle
  patterns in a small DSL, and a compiler generates the C code.
- Video signal: PPU color codes drive a measured DAC model. The waveform goes
  through a cable or RF path, comb or notch Y/C separation, the color decoder
  and the RGB amplifiers before it reaches the tube model.
- Tube: beam width grows with beam current, bright areas load the supply, and
  phosphors decay. The mask is an aperture grille, a slot mask or a shadow
  mask.
- Presets: 23, including a Sony PVM-14L2, a worn RF set and a VHS recording.
- Play: gamepads with hot-plug, 2 players, battery saves, save-state slots,
  fast-forward, NTSC and PAL, an on-screen menu for every control, and a
  native macOS editor for the whole chain.

Supported mappers: 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 34, 66, 69, 71, 206 and 227
(NROM, MMC1, UxROM, CNROM, MMC3, MMC5, AxROM, MMC2, MMC4, Color Dreams,
BNROM/NINA-001, GxROM, FME-7, Camerica, DxROM and the 1200-in-1 board).
Unsupported boards report their mapper number in the ROM browser.

## Download

No binaries are published yet. On macOS and Linux, build from source as shown
below. Windows is untested. No ROMs are included.

## Build from source

### macOS

```bash
brew install cmake
git clone https://github.com/yaglo/mynes.git && cd mynes
cmake -S . -B build && cmake --build build -j
./build/bin/mynes_gpu path/to/game.nes
```

The first configure downloads and builds a pinned SDL3. `glslc` and
`spirv-cross` are optional; without them the build uses the committed shader
binaries.

### Linux (Vulkan)

```bash
sudo apt install cmake libvulkan-dev libx11-dev libxext-dev libxrandr-dev \
    libwayland-dev libxkbcommon-dev libpulse-dev libasound2-dev
cmake -S . -B build -DMYNES_BUNDLED_SDL3=ON && cmake --build build -j
./build/bin/mynes_gpu path/to/game.nes
```

[docs/dev/commands.md](docs/dev/commands.md) lists the build options, the
headless CI recipe and the SDL2 frontend.

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
its limits. The [blog series](https://yaglo.github.io/mynes-web/blog/) covers
the 6502 timing DSL, DMA timing in the DSL compiler, the composite waveform
and the size of the beam spot.

## Performance

On an Apple M5 (24 GiB, Metal, Release build, measured 2026-09-21), the full
GPU chain takes a median 7.3 ms per frame for the Sony PVM-14L2 preset at
2560×1920. The JVC D-Series, Toshiba 14AF43 and Stas's Favourite presets take
9.4 to 11.0 ms. The render scale setting (1, 0.75, 0.5 or auto) draws the beam
and phosphor stages at that fraction of the viewport, which keeps 60 fps on
slower GPUs. The method and raw data are in the
[benchmark results](docs/gpu-benchmark-results.md).

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Unit tests, mapper tests, PAL APU ROM tests, the complete AccuracyCoin suite
and the GPU kernel and preset tests run on every push through GitHub Actions
on Ubuntu and macOS. The GPU pipeline, audio, fence, playback and frame-capture
tests run in CI on Ubuntu only, on lavapipe (software Vulkan). The macOS job
runs the GPU tests that need no GPU device. `gpu_fidelity_tests` needs a
hardware GPU and runs outside CI.

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

## History and license

MyNES began in 2012. The current core is a C11 rewrite, and the original is
on the [`legacy`](https://github.com/yaglo/mynes/tree/legacy) branch.
[Contributing](CONTRIBUTING.md) · Apache-2.0 [license](LICENSE)
