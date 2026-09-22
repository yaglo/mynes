# Build and Test Commands

## Prerequisites

- C11 compiler (gcc or clang)
- CMake 3.16+
- Chicken Scheme (`csi`) -- for building from DSL source (optional; falls
  back to pre-generated code)
- SDL2 -- for the desktop frontend (optional)
- SDL3 -- for the GPU frontend (optional; on macOS a pinned copy is built
  in by default via `MYNES_BUNDLED_SDL3`, elsewhere pass
  `-DMYNES_BUNDLED_SDL3=ON` or install the system package)
- `glslc` (from shaderc), `spirv-cross` and Python 3 -- only to recompile
  the GPU shaders after editing a `.glsl`. Every compiled `.spv`/`.msl` is
  committed under `frontends/gpu/shaders/`, and when the tools are missing
  CMake prints a status line and copies those into `build/shaders/`
  instead of failing.

## Building

### Standard Build

```bash
mkdir cmake-build && cd cmake-build
cmake ..
cmake --build .
```

### Build Options

```bash
cmake .. -DNES_BUILD_TESTS=ON          # Build test executables (default: ON)
cmake .. -DNES_BUILD_FRONTENDS=ON      # Build frontends (default: ON)
cmake .. -DNES_BUILD_GPU_FRONTEND=ON   # Build the SDL3 GPU frontend (default: ON)
cmake .. -DMYNES_BUNDLED_SDL3=ON       # Build SDL3 in instead of finding it (default: ON on macOS)
cmake .. -DMYNES_PORTABLE_BINARY=ON    # Fixed ISA baseline instead of -march=native (default: OFF)
```

Release builds default to `-march=native`, which is fastest but ties the
binary to the machine that built it. `MYNES_PORTABLE_BINARY=ON` uses
`-march=x86-64-v3` (AVX2/FMA) on x86-64 and `-mcpu=apple-m1` on Apple
Silicon instead; the configure summary prints which one was picked. The
release scripts turn it on (see [release.md](release.md)).

### Disable Optional Components

```bash
cmake .. -DNES_BUILD_FRONTENDS=OFF   # Skip SDL frontend
cmake .. -DNES_BUILD_TESTS=OFF       # Skip test executables
```

### Build Just the Library

```bash
cmake --build . --target nes_static
```

### GPU Shaders

The GPU frontend loads SPIR-V (Vulkan) and MSL (Metal) from
`build/shaders/{compute,render}/`. A build never writes into the source
tree:

- With `glslc`, `spirv-cross` and `python3` installed, the `gpu_shaders`
  target compiles `frontends/gpu/shaders/**/*.glsl` into `build/shaders/`
  (and repairs the Metal buffer indices in the MSL there).
- Without them, CMake says so at configure time and copies the committed
  `.spv`/`.msl` files into the same layout. Force this path for testing
  with `-DGLSLC_EXECUTABLE=NOTFOUND`.

After editing a `.glsl`, regenerate the committed copies and commit them
together with the source:

```bash
cmake --build . --target shaders_regenerate
```

`ctest -R gpu_shader_layout_test` checks that `build/shaders/` holds exactly
the committed set, whichever way it was populated.

### Building on Linux

Debian/Ubuntu packages for a full build:

```bash
sudo apt-get install cmake build-essential libsdl2-dev glslc spirv-cross \
    libvulkan-dev mesa-vulkan-drivers
```

(`glslc` and `spirv-cross` are optional, see above; `mesa-vulkan-drivers`
provides `lavapipe`, the software Vulkan device the headless tests use.)

**Desktop build.** Use the distribution's SDL3 if it has one (Ubuntu 25.04+,
Debian 13+, Fedora 41+, Arch), otherwise let CMake build the pinned copy:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release            # system SDL3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMYNES_BUNDLED_SDL3=ON
cmake --build build -j
./build/bin/mynes_gpu game.nes
```

The bundled SDL3 build compiles SDL from source and needs its X11/Wayland
and audio development packages (`libx11-dev libxext-dev libwayland-dev
libxkbcommon-dev libdecor-0-dev libpulse-dev libasound2-dev libgl-dev`).

**Headless CI build.** No display server, software Vulkan only. This is
what `.github/workflows/ci.yml` runs:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DNES_BUILD_TESTS=ON -DNES_BUILD_FRONTENDS=ON -DNES_BUILD_GPU_FRONTEND=ON \
    -DMYNES_BUNDLED_SDL3=ON -DSDL_UNIX_CONSOLE_BUILD=ON \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_VULKAN=ON -DSDL_OFFSCREEN=ON
cmake --build build -j
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy \
    ctest --test-dir build --output-on-failure -E '^gpu_fidelity_tests$'
```

`SDL_UNIX_CONSOLE_BUILD` drops every windowing backend so SDL builds without
X11/Wayland headers; `SDL_OFFSCREEN` keeps a display-less video driver that
the GPU tests claim through `SDL_VIDEO_DRIVER=offscreen`, and
`VK_ICD_FILENAMES` points the Vulkan loader at lavapipe.

## Test Commands

### Quick CI Tests

```bash
./scripts/run_priority_tests.sh
```

Runs CPU, PPU, NES, and AccuracyCoin tests. Exit 0 = all pass.

### Individual Unit Tests

```bash
./bin/test_cpu          # 14 CPU instruction tests
./bin/test_cpu_cycles   # CPU cycle count tests
./bin/test_ppu          # 14 PPU rendering tests
./bin/test_nes          # 9 NES integration tests
./bin/test_rom          # ROM loading tests
./bin/accuracy_coin     # AccuracyCoin accuracy tests
```

### Run All Tests via CTest

```bash
cd cmake-build
ctest --output-on-failure
```

### Convenience Targets

```bash
cmake --build . --target cputest    # Build and run CPU tests
cmake --build . --target pputest    # Build and run PPU tests
cmake --build . --target nestest    # Build and run NES tests
cmake --build . --target alltests   # Build and run all tests
```

### Full ROM Test Suite

```bash
./scripts/run_all_tests.sh
```

Runs all unit tests plus Blargg CPU, PPU, APU, and sprite test ROMs.

## Test Runner

The headless test runner supports Blargg detection, tracing, scripting, and
screen dumps.

### Basic Usage

```bash
./bin/test_runner <rom.nes> [options]
```

### Common Invocations

```bash
# Run Blargg test ROM
./bin/test_runner tests/nes-test-roms/instr_test-v5/rom_singles/01-basics.nes --blargg

# With CPU trace
./bin/test_runner rom.nes --blargg --trace cpu

# Trace specific memory range
./bin/test_runner rom.nes --trace memw --trace-mem 2000-2007

# Full nestest-compatible CPU trace
./bin/test_runner nestest.nes --trace cpudetail --frames 100

# Screen dump on exit
./bin/test_runner rom.nes --dump-on-exit --charmap data/charmaps/default.map

# Screenshot on exit
./bin/test_runner rom.nes --screenshot output.ppm --frames 600

# Script-driven test
./bin/test_runner rom.nes --script tests/my_test.script --blargg

# Verbose with max frame limit
./bin/test_runner rom.nes --blargg --verbose --frames 36000
```

### All Options

| Option | Argument | Description |
|--------|----------|-------------|
| `--blargg` | -- | Enable Blargg test auto-detection |
| `--script` | file | Execute test script |
| `--frames` | N | Max frames before timeout (default: 18000) |
| `--trace` | categories | Enable trace categories (comma-separated) |
| `--trace-mem` | S-E | Filter memory traces to hex range |
| `--charmap` | file | Load custom tile-to-ASCII character map |
| `--dump-on-exit` | -- | Dump nametable as text on exit |
| `--screenshot` | file | Save framebuffer as PPM on exit |
| `--verbose` | -- | Print additional status info |

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Test passed |
| 1 | Test failed |
| 2 | Timeout (frame limit reached) |
| 3 | ROM load error |

## SDL Frontend

```bash
./build/bin/mynes <rom.nes>
```

Requires SDL2 to be found at build time. If SDL2 is not available, the
frontend is skipped during build.

## Headless ROM Runner

```bash
./bin/run_rom <rom.nes>
```

Runs a ROM headlessly (no video/audio output). Useful for quick tests or
benchmarking.

## Code Generation

If you modify `src/cpu/nes6502.dsl`, the CPU code is automatically
regenerated during the next build (requires Chicken Scheme):

```bash
cmake --build .   # Automatically regenerates if DSL changed
```

To manually regenerate:

```bash
csi -s tools/dsl2c.scm src/cpu/nes6502.dsl generated/cpu_gen.c
```

## Related Files

- `CMakeLists.txt` -- build system configuration
- `scripts/run_priority_tests.sh` -- fast CI test script
- `scripts/run_all_tests.sh` -- full test suite script
- `tools/test_runner.c` -- headless test runner source
- `tools/dsl2c.scm` -- DSL-to-C compiler
