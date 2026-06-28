# Testing Infrastructure

## Overview

The emulator has three levels of testing:

1. **Unit tests** -- compiled test executables that verify CPU, PPU, and NES
   integration at the function level.
2. **Accuracy tests** -- AccuracyCoin test suite that validates emulation
   behavior against known-correct output.
3. **ROM tests** -- headless test runner that executes real test ROMs (Blargg
   and others) with auto-detection and scripting.

## Unit Tests

### test_cpu

14 tests covering all CPU instruction categories:

```bash
./bin/test_cpu
```

Tests instruction behavior, flag setting, addressing modes, and edge cases
like page-crossing penalties and the indirect JMP bug.

### test_cpu_cycles

Validates that every instruction takes the correct number of CPU cycles,
including page-crossing and branch-taken penalties.

### test_ppu

14 tests for PPU behavior: register reads/writes, VBlank timing, sprite
evaluation, nametable mirroring, palette access, scroll register updates.

### test_nes

9 integration tests: ROM loading, DMA timing, controller I/O, CPU-PPU
interaction, mapper switching.

### test_rom

ROM loading tests: iNES header parsing, mapper detection, CHR RAM detection,
error handling for invalid files.

## AccuracyCoin

The accuracy test (`tests/accuracy_coin/run_accuracy.c`) runs a specialized
test ROM that exercises timing-sensitive behavior:

```bash
./bin/accuracy_coin
```

Reports a pass/fail result per accuracy category with an overall score.

## Test Runner

`tools/test_runner.c` is a full-featured headless ROM executor built on top
of the emulation core with hooks, traces, and Blargg detection.

### Blargg Test Auto-Detection

Blargg test ROMs write a signature to PRG RAM at $6000:

| Address | Value | Meaning |
|---------|-------|---------|
| $6000 | $80 | Test running |
| $6000 | $00 | Test passed |
| $6000 | other | Test failed (error code) |
| $6001-$6003 | $DE $B0 $61 | Magic signature |
| $6004+ | ASCII | Result text (null-terminated) |

The test runner polls this signature each frame via `blargg_check()`:

```c
BlarggStatus status = blargg_check(&nes);
if (status == BLARGG_PASSED) { exit_code = NES_EXIT_PASS; running = false; }
if (status == BLARGG_FAILED) { exit_code = NES_EXIT_FAIL; running = false; }
```

### Script System

Test scripts are line-based text files with commands:

```
WAIT_FRAMES 120
PRESS_BUTTON start 10
WAIT_EVENT blargg_done
DUMP_SCREEN
SCREENSHOT output.ppm
STOP
```

| Command | Args | Description |
|---------|------|-------------|
| `WAIT_FRAMES` | count | Wait N frames |
| `PRESS_BUTTON` | button, frames | Hold button for N frames |
| `WAIT_EVENT` | name | Wait for named event (e.g., `blargg_done`) |
| `DUMP_SCREEN` | [filename] | Dump nametable as ASCII text |
| `SCREENSHOT` | [filename] | Save framebuffer as PPM |
| `STOP` | -- | Stop execution |

Scripts are loaded with `script_load()` and executed by the test runner's
main loop.

### Exit Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | `NES_EXIT_PASS` | Test passed |
| 1 | `NES_EXIT_FAIL` | Test failed |
| 2 | `NES_EXIT_TIMEOUT` | Frame limit reached (default: 18000 = 5 min) |
| 3 | `NES_EXIT_ROM_ERR` | ROM load error or unsupported mapper |

### CLI Options

```
test_runner <rom> [options]
  --blargg              Enable Blargg test auto-detection
  --script <file>       Execute script file
  --frames <n>          Max frames (default: 18000)
  --trace <categories>  Enable trace categories
  --trace-mem <S>-<E>   Filter memory traces to address range (hex)
  --charmap <file>      Load tile->ASCII character map
  --dump-on-exit        Dump screen as text on completion
  --screenshot <file>   Save screenshot on completion
  --verbose             Verbose output
```

## Test Scripts

### Priority Tests (fast CI)

```bash
./scripts/run_priority_tests.sh
```

Runs unit tests and AccuracyCoin. Fast feedback for CI.

### Full Test Suite

```bash
./scripts/run_all_tests.sh
```

Runs all unit tests plus Blargg CPU, PPU, APU, and sprite test ROMs.

## Writing a New Test ROM Test

1. Create a script file:

   ```
   WAIT_FRAMES 600
   DUMP_SCREEN
   STOP
   ```

2. Run with the test runner:

   ```bash
   ./bin/test_runner path/to/rom.nes --blargg --script tests/my_test.script
   echo $?  # 0=pass, 1=fail
   ```

3. For CI, add the command to `scripts/run_all_tests.sh`.

## Related Files

- `tools/test_runner.c` -- headless test runner
- `tests/cpu/test_cpu.c` -- CPU unit tests
- `tests/cpu/test_cpu_cycles.c` -- CPU cycle count tests
- `tests/ppu/test_ppu.c` -- PPU unit tests
- `tests/nes/test_nes.c` -- NES integration tests
- `tests/nes/test_rom.c` -- ROM loading tests
- `tests/accuracy_coin/run_accuracy.c` -- AccuracyCoin runner
- `src/nes/blargg.h` -- Blargg detection logic
- `src/nes/script.h` -- script parser
- `src/nes/exit_codes.h` -- CI exit code definitions
- `scripts/run_priority_tests.sh` -- fast CI script
- `scripts/run_all_tests.sh` -- full test suite script
