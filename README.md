# MyNES

**A NES emulator that follows the picture from the console's video signal to the glow of a CRT.**

Play through a focused studio monitor, a soft living-room television, or a worn
RF set. MyNES builds the NES waveform from PPU colour codes, passes it through
a receiver, and renders the beam, phosphors and glass. Change the connection,
turn a control, or switch televisions while the game keeps running.

[![Castlevania castle hall on the PVM, 3840×2880](docs/images/showcase/4k/castlevania-pvm-gameplay.png)](docs/images/showcase/4k/castlevania-pvm-gameplay.png)

*Castlevania in play · Sony PVM-14L2 · click for the full 3840×2880 capture.*

[Get started](#quick-start) · [All 21 CRT presets](docs/contra-preset-gallery.md) ·
[Signal Studio](tools/visualiser/README.md) · [How the model works](docs/gpu-pipeline-reference.md)

## See the signal move

**[▶ Watch the five-game showcase — 20 seconds at 60.1 fps](docs/images/showcase/showcase-reel.mp4)**

| Kirby’s Adventure · JVC D-Series | Little Samson · JVC D-Series |
|---|---|
| [![Kirby title with unaveraged NTSC phases](docs/images/showcase/kirby-jvc_d_series_2000.gif)](docs/images/showcase/kirby-jvc_d_series_2000.mp4) | [![Little Samson palace opening](docs/images/showcase/little-samson-jvc_d_series_2000.gif)](docs/images/showcase/little-samson-jvc_d_series_2000.mp4) |
| **Darkwing Duck · Stas’s Favourite RF** | **Super Mario Bros. 3 · Toshiba 14AF** |
| [![Darkwing Duck bridge gameplay](docs/images/showcase/darkwing-stass_favourite.gif)](docs/images/showcase/darkwing-stass_favourite.mp4) | [![Super Mario Bros. 3 animated stage title](docs/images/showcase/mario-3-toshiba_14af43.gif)](docs/images/showcase/mario-3-toshiba_14af43.mp4) |

[▶ Mega Man 2 rooftop title on the PVM](docs/images/showcase/mega-man-2-sony_pvm_14l2.mp4)

*Actual MyNES output, with alternating NTSC phases kept separate. GIFs run at
50 fps for compatibility; click any image for its **60.1 fps video**. No temporal
averaging or added flicker. [More images and videos](docs/nes-visual-showcase.md).*

The colour fringes and fine patterns change from one frame to the next.
This native-pixel, lossless detail keeps the near-60 Hz cadence so you can
see what a merged screenshot hides:

![Kirby title phase detail at native pixels](docs/images/showcase/kirby-phase-detail.webp)

## Look closer

[![Castlevania: native-pixel close-up of Simon, the window and masonry](docs/images/showcase/4k/castlevania-pvm-detail.png)](docs/images/showcase/4k/castlevania-pvm-detail.png)

*The same frame, cropped to 1280×1120 without enlargement. Open at 100% to inspect the beam and grille.*

**[Four CRTs at 4K: native close-ups and measured beam height](docs/gpu-beam-closeups.md).**
The complete gameplay image is **3840 pixels wide**, with no side bars.
Dim strokes stay narrow; bright details spread and fill more of the gap
between scanlines. Detail crops are not enlarged. The behavior is modeled;
individual tube beam profiles remain uncalibrated.

## Choose your television

**[Compare all 21 presets on the same Contra scene →](docs/contra-preset-gallery.md)**

[![Contra through four CRT presets](docs/images/contra-gallery/overview-4.png)](docs/contra-preset-gallery.md#group-4)

[![Studio aperture grille: native face and bright-platform details](docs/images/contra-gallery/studio-pvm-beam-detail.png)](docs/contra-preset-gallery.md#group-5)

*Studio aperture grille (Y/C): two separate native crops of the face and platform, with a visible divider. From the gallery’s 3840×2880, unaveraged frame; the generic Studio preset is separate from the nominal 14L2.*

| Start here | What it brings to the picture |
|---|---|
| **Sony PVM-14L2** | Focused beam, fine aperture grille, neutral D65 white balance, composite decoding. |
| **JVC D-Series** | Cooler whites, estimated consumer red push, slot mask and two-line comb separation. |
| **Toshiba 14AF** | Softer beam, broader highlights, slot mask and three-line comb separation. |
| **Stas's Favourite** | RF reception, imperfect convergence, bright/dark recovery and load-dependent screen contraction. |

These four are the main tuning targets. The library also includes RGB and Y/C
monitors, older household sets, worn tubes, and two curated personal looks.
Commercial names identify nominal references: individual tube condition and
many circuit responses remain estimates. [Preset settings and evidence](docs/gpu-preset-audit.md).

[Compare all 21 presets on the same Contra frame](docs/contra-preset-gallery.md),
or inspect the [phase and scrolling review](docs/gpu-motion-review.md).

**[See VHS playback on a consumer CRT →](docs/nes-visual-showcase.md#vhs-playback)**
Contra through the separate VHS SP recording/playback preset, captured at 3840×2880.

## Follow the signal

```text
PPU colour codes → NES DAC waveform → cable / RF receiver → Y/C separation
                 → colour decoder → RGB amplifiers → beam & phosphors
                 → mask & glass → SDR / HDR display
```

- **A waveform before a picture.** The stock NES video path starts from measured
  DAC voltage levels, with NTSC/PAL phase and colour emphasis. Composite and RF
  artefacts emerge from signal processing. Ideal modified RGB, component and
  Y/C sources are also available.
- **CRT structure that responds to the image.** Beam width grows with current;
  bright areas can affect focus, recovery and raster size. Aperture grilles,
  slot masks and shadow masks give the display its texture. Mask sampling
  follows the actual viewport and host-display scale.
- **Light and sound through the set.** Phosphor decay, glass scatter, ambient
  black and available HDR headroom shape the output. The audio path models
  console filtering, cable, amplifier and generic speaker responses, with
  CPU and GPU processing options.
- **A live editor.** The in-game OSD and native macOS Signal Studio expose the
  chain's controls. Switch presets, save your own, rename them, and see which
  preset is active without restarting the game.
- **An inspectable emulator.** Cycle-by-cycle CPU microcode, a dot-stepped PPU,
  tracing, tests, offscreen captures and repeatable benchmarks underpin the
  visual experiments.

The GPU frontend is actively developed and playable; it is also a signal-chain
laboratory. The lightweight SDL2 frontend remains available. On the tested
Apple M5, complete offscreen Contra playback with emulation and audio sustained
about 60 fps at 2560×1664 across the four main profiles. See the
[benchmark methodology and limits](docs/gpu-benchmark-results.md), including
GPU timings and periodic capture overhead.

Still images cannot reproduce CRT motion on an LCD, and these presets are not
measurements of twenty individual televisions. RF is an equivalent baseband
model, not a complete tuner simulation. The [model reference](docs/gpu-pipeline-reference.md)
records those boundaries alongside the implemented stages.

## Play on a real CRT, too

The SDL2 frontend can stream PPU frames over USB to a programmed Tang Nano 20K
while keeping the Mac preview running. Build with `-DNES_CRT_USB=ON`, then launch
`./build-crt/bin/mynes --crt-usb your-ntsc-rom.nes`.
See [setup and hardware requirements](docs/crt-usb.md).

## Quick Start

### Prerequisites

Required for the core and tests:

- CMake 3.16+
- A C11 compiler
- Chicken Scheme (`csi`) if you want to regenerate CPU code from the DSL
  locally. Pre-generated CPU code is committed as a fallback.

The GPU frontend is enabled by default and requires SDL3, `glslc` (provided
by **shaderc**), `spirv-cross`, and Python 3 for shader compilation.
On macOS:

```bash
brew install cmake sdl3 shaderc spirv-cross python
command -v glslc spirv-cross python3
glslc --version
```

If a build reports **`glslc: command not found`**, install `shaderc`—the Homebrew
package is not named `glslc`—and ensure Homebrew's `bin` directory is on your
shell's `PATH`. Then rerun the build. Package references:
[shaderc](https://formulae.brew.sh/formula/shaderc),
[spirv-cross](https://formulae.brew.sh/formula/spirv-cross).

SDL2 is optional for the separate legacy `mynes` frontend.

### Build

Use an out-of-source build:

```bash
mkdir build
cd build
cmake ..
cmake --build . -j
```

On macOS, CMake resolves the SDK from the selected developer tools and
defaults the minimum deployment version to the running macOS version.
An explicit `-DCMAKE_OSX_DEPLOYMENT_TARGET=...` overrides this default;
dependencies must support that version too.

The main build options are:

```bash
cmake .. -DNES_BUILD_TESTS=ON        # default: ON
cmake .. -DNES_BUILD_FRONTENDS=ON    # default: ON
cmake .. -DNES_BUILD_GPU_FRONTEND=ON # default: ON, requires SDL3
```

### Run

From the build directory:

```bash
./bin/mynes_gpu                  # open the GPU frontend ROM browser
./bin/mynes_gpu path/to/game.nes # run a ROM directly
```

The repository does not include commercial ROMs. Keep local ROMs in
`roms/`; that directory is ignored by git.

### GPU Frontend

New build directories enable the GPU frontend automatically. If an existing
build cached the old OFF default, enable it explicitly:

```bash
cmake .. -DNES_BUILD_GPU_FRONTEND=ON
cmake --build . -j
```

To opt out, configure with `-DNES_BUILD_GPU_FRONTEND=OFF`.
`-DNES_BUILD_FRONTENDS=OFF` disables both graphical frontends. The optional
SDL2 frontend remains available as `./bin/mynes`.

Shaders are built automatically into the build tree. To rebuild them
manually from the repository root:

```bash
./frontends/gpu/shaders/compile_shaders.sh
```

The GPU frontend restores its last preset from
`~/.config/mynes/config.json`. Press **M** in-game for the on-screen
signal-chain menu, **P** to cycle presets, **C** to switch composite/raw
views, **Shift+C** for split view, and **O** to reopen the ROM browser.
The menu is translucent; Enter on a parameter opens a compact bottom adjustment
strip. Left/Right changes its value, and Enter or Escape returns to the menu.
Changing presets displays the selected name for three seconds. Bundled presets
load relative to the executable (`build/presets`), independently of the launch
directory; personal presets remain in `~/.config/mynes/presets`.

### Live signal editor (macOS)

```bash
# From the repository root, in separate terminals:
./build/bin/mynes_gpu --debug-server path/to/game.nes
swift run --package-path tools/visualiser -c release
```

See [Signal Studio](tools/visualiser/README.md) for preset management and controls.

## Architecture

The core is platform-agnostic C11. SDL, GPU APIs, audio devices, windows,
and file browsers live in `frontends/`; the emulator library itself does
not depend on them.

**CPU:** `src/cpu/nes6502.dsl` describes all official and unofficial
6502 opcodes as reusable cycle patterns. `tools/dsl2c.scm` generates
`cpu_step()` and bus-introspection helpers such as "what address would
the next read use?" and "is the next microcycle a write?". That derived
knowledge is used by DMC DMA and edge-case store instructions instead of
maintaining a second hand-written timing model.

**System timing:** `src/nes/nes.h` is the master scheduler. Each
`nes_step()` advances one CPU cycle worth of master-clock time, interleaves
PPU dots around CPU bus access, handles OAM DMA and DMC DMA cycle
stealing, propagates APU/mapper IRQs, and dispatches debug hooks.

**PPU/APU/mappers:** the PPU runs dot-by-dot, the APU produces filtered
audio, and cartridge mapper logic is isolated behind a mapper interface.
The supported mapper set is focused on real games and test ROMs rather
than trying to cover every board at once.

**Composite video:** `src/nes/composite.h` is the CPU path used by the
SDL2 frontend, headless renderer, and tests. It generates the 2C02/2C07
waveform, bandwidth-limits it, demodulates chroma, matrix-decodes it, and
applies the lightweight display treatment needed for real-time play.

**GPU signal chain:** `frontends/gpu/` contains the SDL3 signal-chain frontend.
PPU codes drive a measured voltage model, followed by the NTSC/PAL raster,
receiver sync/burst recovery, Y/C decoding, RGB amplifiers, CRT supply/loading,
beam deposition, phosphor decay, mask, glass, and SDR/HDR output. Emulation and
audio run independently of presentation. Presets and live controls are available
in the OSD and Signal Studio.

See the [model and its limits](docs/gpu-pipeline-reference.md) and
[benchmarks](docs/gpu-benchmark-results.md), [hardware research](docs/gpu-hardware-research.md),
and [visual review](docs/gpu-visual-review.md). The current
[20-preset gallery](docs/contra-preset-gallery.md) includes the strengths and
remaining weaknesses visible in the renders.

## Tests

Build with tests enabled, then run:

```bash
ctest --test-dir build --output-on-failure
```

Useful individual targets and tools:

```bash
cmake --build build --target test_cpu test_cpu_cycles test_ppu test_nes test_rom
cmake --build build --target accuracy_coin
./build/bin/accuracy_coin # all 144 AccuracyCoin tests; failures return nonzero
./build/bin/accuracy_coin tests/accuracy_coin/AccuracyCoin.nes 14 # one page
./build/bin/test_runner tests/nes-test-roms/.../some-test.nes --blargg
```

GPU-related tests are available when `NES_BUILD_GPU_FRONTEND=ON`:

```bash
ctest --test-dir build -R '^gpu_' --output-on-failure
```

## Documentation

- [Architecture overview](docs/architecture/overview.md) explains the core
  components and data flow.
- [Timing](docs/architecture/timing.md) covers master-clock scheduling,
  DMA, interrupts, and region timing.
- [Testing infrastructure](docs/architecture/testing.md) describes unit,
  AccuracyCoin, Blargg, script, and trace workflows.
- [DSL design](docs/dsl-design.md) and
  [DSL reference](docs/dsl-reference.md) explain the CPU microcode
  language and compiler.
- [GPU pipeline design](docs/gpu-pipeline-design.md) and
  [pipeline reference](docs/gpu-pipeline-reference.md) document the
  palette-index-to-phosphor path.
- [GPU frontend design](docs/gpu-frontend-design.md) preserves the design
  plan and implementation notes for the SDL_GPU frontend.
- [Blog notes](docs/blog/) are a narrative series about the emulator's
  main ideas: timing as data, compiler-derived DMA behaviour, composite
  signal generation, CRT stages, comb filtering, beam physics, presets,
  and why the CPU and GPU pipelines both exist.
- [Debugging workflows](docs/debugging/common-workflows.md) and
  [development commands](docs/dev/commands.md) are the operational guides.

## Project Layout

```text
include/nes/              Public API headers
src/cpu/                  6502 DSL source and generated CPU interface
src/nes/                  System integration, APU, mappers, ROM loading,
                          composite video, hooks, tracing
src/ppu/                  Dot-accurate PPU
frontends/sdl/            SDL2 playable frontend
frontends/gpu/            SDL3 + SDL_GPU analog signal-chain frontend
frontends/headless/       Non-interactive runner
tests/                    Unit tests, integration tests, AccuracyCoin,
                          and test ROM fixtures
tools/                    DSL compiler, test runner, visualiser tools
docs/                     Architecture, debugging, GPU pipeline, and blog notes
presets/                  GPU frontend physical display presets
```

## Origins

MyNES is a ground-up C11 rewrite of a NES emulator first written in 2012.
The original is preserved on the [`legacy`](https://github.com/yaglo/mynes/tree/legacy)
branch. The rewrite combines declarative CPU timing with signal-based video.

## License

See [LICENSE](LICENSE).
