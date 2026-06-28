# Trace System

## Overview

The trace system (`src/nes/trace.h`) provides runtime-configurable logging
across 12 categories. It is built on top of the hook system -- enabling a
trace category installs the corresponding hook handler. Categories are
represented as a bitmask for fast checking.

## Setup

```c
#include "nes/trace.h"

trace_init();                                          // Reset state, set default output
trace_set_categories(trace_parse_categories("cpu,irq")); // Enable categories
trace_set_mem_range(0x6000, 0x7FFF);                   // Filter memory traces
trace_install_hooks();                                 // Wire hooks

// ... run emulation (traces print to stdout) ...

trace_remove_hooks();                                  // Clean up
```

## Categories

### Primary Categories

| Category | Bitmask | CLI Name | Description |
|----------|---------|----------|-------------|
| `TRACE_CPU_INSTR` | `1 << 0` | `cpu` | One-line CPU instruction log (PC, opcode, cycles) |
| `TRACE_CPU_DETAIL` | `1 << 1` | `cpudetail` | Full nestest-format disassembly with registers |
| `TRACE_CPU_IRQ` | `1 << 2` | `irq` | NMI and IRQ events |
| `TRACE_PPU_REG` | `1 << 3` | `ppureg` | PPU register reads/writes ($2000-$2007) |
| `TRACE_PPU_SCAN` | `1 << 4` | `ppuscan` | Scanline and VBlank transitions |
| `TRACE_PPU_VRAM` | `1 << 5` | `vram` | VRAM reads/writes |
| `TRACE_APU_REG` | `1 << 6` | `apureg` | APU register writes ($4000-$4017) |
| `TRACE_APU_FRAME` | `1 << 7` | `apuframe` | Frame counter quarter/half frame events |
| `TRACE_APU_IRQ` | `1 << 8` | `apuirq` | APU IRQ events |
| `TRACE_MEM_READ` | `1 << 9` | `memr` | CPU bus reads |
| `TRACE_MEM_WRITE` | `1 << 10` | `memw` | CPU bus writes |
| `TRACE_TIMING` | `1 << 11` | `timing` | Frame completion with cycle counts |

### Convenience Aliases

| Alias | Expands To |
|-------|------------|
| `ppu` | `ppureg + ppuscan + vram` |
| `apu` | `apureg + apuframe + apuirq` |
| `mem` | `memr + memw` |
| `all` | All categories |

## Output Format

Each trace line is prefixed with a category tag:

```
[CPU] PC=$C000 OP=$4C CYC=7
[IRQ] NMI VEC=$FFFA RET=$8075
[MEM] W $2000 = $90
[MEM] R $2002 = $80
[PPU] Scanline 241 frame 1
[PPU] VBlank START
[PPU] W $2000 = $80
[APU] Frame step=0 Q=1 H=0 IRQ=0
[APU] W $4015 = $0F
[FRAME] #60 cycles=1788900
```

The `cpudetail` category uses the CPU trace formatter for nestest-compatible
output:

```
C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD
C5F5  A2 00     LDX #$00                         A:00 X:00 Y:00 P:24 SP:FD
```

## Memory Address Filtering

Memory traces (`memr`, `memw`) can be filtered to a specific address range:

```c
trace_set_mem_range(0x2000, 0x2007);  // Only trace PPU register accesses
```

The default range is $0000-$FFFF (all addresses). The `TRACE_MEM` macro
checks the address against the range:

```c
#define TRACE_MEM(cat, addr, fmt, ...) \
    TRACE_IF(cat, (addr) >= trace_mem_start && (addr) <= trace_mem_end, \
             fmt, ##__VA_ARGS__)
```

## Output Routing

By default, traces print to stdout via `printf`. You can redirect output:

```c
// Custom output function
void my_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
}

trace_set_output(my_log);
```

Pass NULL to restore the default printf output.

## Programmatic API

```c
trace_enable(TRACE_CPU_INSTR);              // Enable one category
trace_disable(TRACE_MEM_READ);              // Disable one category
trace_set_categories(TRACE_CPU_INSTR | TRACE_CPU_IRQ);  // Set exact mask
uint32_t cats = trace_get_categories();     // Read current mask
const char *name = trace_category_name(TRACE_CPU_INSTR); // -> "cpu"
```

## CLI Usage

The test runner accepts trace options:

```bash
# Trace CPU instructions
./bin/test_runner rom.nes --blargg --trace cpu

# Trace PPU register writes in a specific address range
./bin/test_runner rom.nes --trace memw --trace-mem 2000-2007

# Full nestest-compatible CPU trace
./bin/test_runner rom.nes --trace cpudetail

# Multiple categories
./bin/test_runner rom.nes --trace cpu,irq,ppureg

# Everything
./bin/test_runner rom.nes --trace all
```

## Category String Parsing

`trace_parse_categories()` accepts comma, pipe, plus, or space-separated
category names:

```c
trace_parse_categories("cpu,irq")        // -> TRACE_CPU_INSTR | TRACE_CPU_IRQ
trace_parse_categories("ppu")            // -> TRACE_PPU_REG | TRACE_PPU_SCAN | TRACE_PPU_VRAM
trace_parse_categories("all")            // -> TRACE_ALL (0xFFFF)
```

## Hook Installation Details

`trace_install_hooks()` selectively installs hook handlers based on which
categories are enabled:

- `TRACE_CPU_DETAIL` -> installs `cpu_trace_install_hook()` (full disassembly)
- `TRACE_CPU_INSTR` -> installs `trace_on_cpu_step` (one-liner)
- `TRACE_CPU_IRQ` -> installs `trace_on_cpu_irq`
- `TRACE_MEM_READ | TRACE_MEM_WRITE` -> installs `trace_on_mem_access`
- `TRACE_PPU_SCAN` -> installs `trace_on_ppu_scanline` and `trace_on_ppu_vblank`
- `TRACE_PPU_REG` -> installs `trace_on_ppu_reg`
- `TRACE_APU_FRAME` -> installs `trace_on_apu_frame`
- `TRACE_APU_REG` -> installs `trace_on_apu_reg`
- `TRACE_TIMING` -> installs `trace_on_frame`

Note: `TRACE_CPU_DETAIL` takes priority over `TRACE_CPU_INSTR` since both
use `on_cpu_step`.

## Related Files

- `src/nes/trace.h` -- category enum, macros, API declarations
- `src/nes/trace.c` -- implementation, hook handlers, category table
- `src/nes/cpu_trace.h` -- CPU disassembly formatter declaration
- `src/nes/cpu_trace.c` -- nestest-format trace implementation
- `src/nes/hooks.h` -- underlying hook system
