# Hook Reference

## Overview

The hook system (`src/nes/hooks.h`) provides 9 event callbacks. All hooks
are stored in a single global `DebugHooks` struct. When a hook function
pointer is NULL, the overhead is one branch per event site.

## Setup

```c
#include "nes/hooks.h"

hooks_init();  // Zero all hook pointers
// ... install hooks ...
// ... run emulation ...
hooks_clear(); // Remove all hooks
```

## Hook Reference

### on_cpu_step

**Fires when**: CPU instruction completes (microcode counter returns to 0).

```c
typedef void (*hook_cpu_step_fn)(uint16_t pc, uint8_t opcode, uint64_t cycles);
```

| Parameter | Description |
|-----------|-------------|
| `pc` | Program counter of the NEXT instruction |
| `opcode` | Opcode byte of the completed instruction (from IR register) |
| `cycles` | Total CPU cycles elapsed |

**Use cases**: Instruction logging, breakpoints, execution tracing.

**Note**: `pc` points to the next instruction, not the one that just ran. To
get the address of the completed instruction, subtract the instruction size:
`pc - cpu_op_size[opcode]`.

---

### on_cpu_irq

**Fires when**: CPU consumes an NMI or IRQ (transitions from pending to
handling).

```c
typedef void (*hook_cpu_irq_fn)(bool is_nmi, uint16_t vector_addr, uint16_t return_addr);
```

| Parameter | Description |
|-----------|-------------|
| `is_nmi` | true for NMI, false for IRQ |
| `vector_addr` | Vector address ($FFFA for NMI, $FFFE for IRQ) |
| `return_addr` | PC that was pushed to the stack |

**Use cases**: Tracking interrupt timing, debugging missed NMIs.

---

### on_mem_access

**Fires when**: CPU bus read or write occurs.

```c
typedef void (*hook_mem_access_fn)(uint16_t addr, uint8_t value, bool is_write);
```

| Parameter | Description |
|-----------|-------------|
| `addr` | 16-bit address |
| `value` | Byte read or written |
| `is_write` | true for write, false for read |

**Use cases**: Memory watchpoints, tracking register writes, debugging
mapper bank switches.

**Warning**: This fires for every bus access including instruction fetches.
High overhead when enabled.

---

### on_ppu_scanline

**Fires when**: PPU starts a new scanline (dot 0).

```c
typedef void (*hook_ppu_scanline_fn)(int scanline, uint64_t frame);
```

| Parameter | Description |
|-----------|-------------|
| `scanline` | Scanline number (0-261 NTSC, 0-311 PAL) |
| `frame` | Current frame number |

**Use cases**: Mid-frame rendering effects, scroll split debugging.

---

### on_ppu_vblank

**Fires when**: VBlank begins or ends.

```c
typedef void (*hook_ppu_vblank_fn)(bool entering, uint64_t cpu_cycle);
```

| Parameter | Description |
|-----------|-------------|
| `entering` | true when entering VBlank, false when leaving |
| `cpu_cycle` | CPU cycle count at this event |

**Use cases**: Frame timing analysis, VBlank timing verification.

---

### on_ppu_reg

**Fires when**: PPU register ($2000-$2007) is read or written.

```c
typedef void (*hook_ppu_reg_fn)(uint16_t addr, uint8_t value, bool is_write);
```

| Parameter | Description |
|-----------|-------------|
| `addr` | Register address ($2000-$2007) |
| `value` | Value read or written |
| `is_write` | true for write, false for read |

**Use cases**: Debugging scroll glitches, tracking PPUCTRL/PPUMASK changes,
monitoring PPUSTATUS reads.

---

### on_apu_frame

**Fires when**: APU frame counter triggers a quarter-frame or half-frame
clock.

```c
typedef void (*hook_apu_frame_fn)(int step, bool quarter, bool half, bool irq);
```

| Parameter | Description |
|-----------|-------------|
| `step` | Frame counter step number |
| `quarter` | true if quarter-frame clocked (envelopes, linear counter) |
| `half` | true if half-frame clocked (length counters, sweep) |
| `irq` | true if frame counter IRQ flag is set |

**Use cases**: Audio timing debugging, frame counter mode verification.

---

### on_apu_reg

**Fires when**: APU register ($4000-$4017) is written.

```c
typedef void (*hook_apu_reg_fn)(uint16_t addr, uint8_t value);
```

| Parameter | Description |
|-----------|-------------|
| `addr` | Register address ($4000-$4017) |
| `value` | Value written |

**Use cases**: Audio debugging, tracking channel configuration.

---

### on_frame

**Fires when**: A complete frame has been rendered (PPU frame_complete flag).

```c
typedef void (*hook_frame_fn)(uint64_t frame_number, uint64_t cpu_cycles);
```

| Parameter | Description |
|-----------|-------------|
| `frame_number` | Frame count since reset |
| `cpu_cycles` | Total CPU cycles at frame completion |

**Use cases**: Frame counting, per-frame state inspection, game state
monitoring.

## Macro Reference

Each hook has a corresponding macro used in the emulation core:

| Macro | Used In |
|-------|---------|
| `HOOK_CPU_STEP(pc, op, cyc)` | `nes.h` (instruction boundary detection) |
| `HOOK_CPU_IRQ(is_nmi, vec, ret)` | `nes.h` (NMI/IRQ consumption detection) |
| `HOOK_MEM_ACCESS(addr, val, wr)` | `nes.h` (`nes_cpu_read`/`nes_cpu_write`) |
| `HOOK_PPU_SCANLINE(sl, frame)` | `ppu.h` (scanline transition) |
| `HOOK_PPU_VBLANK(enter, cyc)` | `ppu.h` (VBlank flag changes) |
| `HOOK_PPU_REG(addr, val, wr)` | `ppu.h` (`ppu_reg_read`/`ppu_reg_write`) |
| `HOOK_APU_FRAME(step, q, h, irq)` | `apu.h` (frame counter clocking) |
| `HOOK_APU_REG(addr, val)` | `apu.h` (register writes) |
| `HOOK_FRAME(num, cyc)` | `nes.h` (frame completion) |

## Example: Logging PPU Writes

```c
void log_ppu_writes(uint16_t addr, uint8_t val, bool is_write) {
    if (!is_write) return;
    const char *names[] = {
        "PPUCTRL", "PPUMASK", "PPUSTATUS", "OAMADDR",
        "OAMDATA", "PPUSCROLL", "PPUADDR", "PPUDATA"
    };
    printf("%s ($%04X) <- $%02X\n", names[addr & 7], addr, val);
}

hooks_init();
debug_hooks.on_ppu_reg = log_ppu_writes;
```

## Related Files

- `src/nes/hooks.h` -- type definitions and macros
- `src/nes/hooks.c` -- global instance, init/clear
- `src/nes/trace.c` -- trace system hook handlers
- `src/nes/cpu_trace.c` -- CPU disassembly hook handler
