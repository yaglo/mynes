# CPU Trace Format

## Overview

The CPU trace formatter (`src/nes/cpu_trace.c`) produces nestest-compatible
disassembly output. It uses side-effect-free debug reads so tracing does not
alter emulation state.

## Output Format

Each line follows the nestest log format:

```
ADDR  BYTES     MNEMONIC OPERAND                    REGISTERS
C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD
C5F5  A2 00     LDX #$00                         A:00 X:00 Y:00 P:24 SP:FD
C5F7  86 00     STX $00 = 00                     A:00 X:00 Y:00 P:26 SP:FD
```

Fields:
- **ADDR**: 4-hex-digit PC of the instruction
- **BYTES**: Raw opcode and operand bytes (1-3 bytes, space-padded)
- **MNEMONIC**: 3-letter instruction name (unofficial prefixed with `*`)
- **OPERAND**: Decoded operand with effective address and value
- **REGISTERS**: `A:XX X:XX Y:XX P:XX SP:XX`

## Addressing Mode Display

| Mode | Format | Example |
|------|--------|---------|
| Implied | (empty) or `A` for shifts | `RTS`, `ASL A` |
| Immediate | `#$XX` | `LDA #$42` |
| Zero Page | `$XX = VV` | `LDA $00 = 42` |
| Zero Page,X | `$XX,X @ EA = VV` | `LDA $10,X @ 15 = 42` |
| Zero Page,Y | `$XX,Y @ EA = VV` | `LDX $10,Y @ 15 = 42` |
| Absolute | `$XXXX` or `$XXXX = VV` | `JMP $C000`, `LDA $0200 = 42` |
| Absolute,X | `$XXXX,X @ EEEE = VV` | `LDA $0200,X @ 0205 = 42` |
| Absolute,Y | `$XXXX,Y @ EEEE = VV` | `LDA $0200,Y @ 0203 = 42` |
| Indirect | `($XXXX) = TTTT` | `JMP ($0200) = C000` |
| (Indirect,X) | `($XX,X) @ ZZ = EEEE = VV` | `LDA ($10,X) @ 15 = 0200 = 42` |
| (Indirect),Y | `($XX),Y = BBBB @ EEEE = VV` | `LDA ($10),Y = 0200 @ 0203 = 42` |
| Relative | `$TTTT` | `BNE $C010` |

**Legend**: XX=operand byte, VV=value at address, EA=effective address,
EEEE=effective address (16-bit), TTTT=branch target, BBBB=base address,
ZZ=zero-page pointer

## Opcode Tables

The opcode tables in `src/nes/cpu_opnames.h` define:

### cpu_op_name[256]

Mnemonic strings for all 256 opcodes. Unofficial opcodes are prefixed with
`*` (e.g., `*SLO`, `*LAX`, `*DCP`). NULL entries indicate KIL/JAM opcodes.

### cpu_op_mode[256]

`CPUAddressMode` enum values:

```c
typedef enum {
    AM_IMP,  AM_IMM,  AM_ZP,   AM_ZPX,  AM_ZPY,
    AM_ABS,  AM_ABX,  AM_ABY,  AM_IND,
    AM_IZX,  AM_IZY,  AM_REL,
} CPUAddressMode;
```

### cpu_op_size[256]

Instruction size in bytes (1, 2, or 3). Used to calculate the instruction's
starting PC from the current (post-execution) PC:

```c
uint16_t pc = (cpu->PC - cpu_op_size[opcode]) & 0xFFFF;
```

## Usage

### From C Code

```c
#include "nes/cpu_trace.h"

cpu_trace_set_nes(&nes);  // Must be called once before use

char buf[256];
int len = cpu_trace_format(&nes, buf, sizeof(buf));
printf("%s", buf);
```

### As a Hook

```c
cpu_trace_set_nes(&nes);
cpu_trace_install_hook();  // Sets debug_hooks.on_cpu_step
```

### Via Trace System

```c
trace_set_categories(trace_parse_categories("cpudetail"));
trace_install_hooks();  // Automatically calls cpu_trace_install_hook()
```

### From CLI

```bash
./bin/test_runner rom.nes --trace cpudetail
```

## Implementation Notes

- The formatter reads operand bytes using `debug_read_cpu()` which returns 0
  for I/O registers ($2000-$401F) instead of triggering side effects.
- For indirect modes, the formatter resolves the effective address by reading
  through the zero-page pointer, reproducing the 6502's page-wrap bug for
  `JMP ($xxFF)`.
- The `on_cpu_step` hook fires after the instruction completes, so `cpu->IR`
  still holds the opcode and `cpu->PC` points to the next instruction.

## Related Files

- `src/nes/cpu_trace.h` -- API declaration
- `src/nes/cpu_trace.c` -- formatting implementation
- `src/nes/cpu_opnames.h` -- opcode name/mode/size tables
- `src/nes/debug.h` -- `debug_read_cpu()` for safe operand reads
