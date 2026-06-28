# Build and Test Commands

## Prerequisites

- C11 compiler (gcc or clang)
- CMake 3.16+
- Chicken Scheme (`csi`) -- for building from DSL source (optional; falls
  back to pre-generated code)
- SDL2 -- for the desktop frontend (optional)

## Building

### Standard Build

```bash
mkdir cmake-build && cd cmake-build
cmake ..
cmake --build .
```

### Build Options

```bash
cmake .. -DNES_BUILD_TESTS=ON        # Build test executables (default: ON)
cmake .. -DNES_BUILD_FRONTENDS=ON    # Build SDL frontend (default: ON)
cmake .. -DNES_BUILD_EXAMPLES=ON     # Build example apps (default: ON)
```

### Disable Optional Components

```bash
cmake .. -DNES_BUILD_FRONTENDS=OFF   # Skip SDL frontend
cmake .. -DNES_BUILD_TESTS=OFF       # Skip test executables
```

### Build Just the Library

```bash
cmake --build . --target nes_static
```

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
./bin/nes_sdl <rom.nes>
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
