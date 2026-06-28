# State Inspection

## Overview

The debug API (`src/nes/debug.h`) provides tools for inspecting emulator state
without side effects. This is critical for debuggers and trace formatters that
need to read memory and registers while the emulation is running.

## Snapshots

Snapshots capture a component's state at a point in time. They are plain
structs that can be freely inspected without affecting emulation.

### Taking a Snapshot

```c
#include "nes/debug.h"

CPUSnapshot cpu_snap;
PPUSnapshot ppu_snap;
APUSnapshot apu_snap;

debug_get_cpu_snapshot(&nes, &cpu_snap);
debug_get_ppu_snapshot(&nes, &ppu_snap);
debug_get_apu_snapshot(&nes, &apu_snap);
```

### CPUSnapshot Fields

| Field | Type | Description |
|-------|------|-------------|
| `PC` | `uint16_t` | Program counter |
| `A` | `uint8_t` | Accumulator |
| `X` | `uint8_t` | X index register |
| `Y` | `uint8_t` | Y index register |
| `SP` | `uint8_t` | Stack pointer |
| `P` | `uint8_t` | Processor flags (NV-BDIZC) |
| `IR` | `uint8_t` | Current instruction register |
| `uPC` | `uint16_t` | Microcode position (0 = at instruction boundary) |
| `cycles` | `uint64_t` | Total CPU cycles elapsed |
| `irq_pending` | `bool` | IRQ line asserted |
| `nmi_pending` | `bool` | NMI pending |
| `reset_pending` | `bool` | Reset sequence pending |

### PPUSnapshot Fields

| Field | Type | Description |
|-------|------|-------------|
| `scanline` | `int` | Current scanline (0-261 NTSC) |
| `dot` | `int` | Current dot within scanline (0-340) |
| `v` | `uint16_t` | Current VRAM address (loopy v) |
| `t` | `uint16_t` | Temporary VRAM address (loopy t) |
| `fine_x` | `uint8_t` | Fine X scroll (0-7) |
| `w` | `bool` | Write toggle for $2005/$2006 |
| `ctrl` | `uint8_t` | PPUCTRL ($2000) value |
| `mask` | `uint8_t` | PPUMASK ($2001) value |
| `status` | `uint8_t` | PPUSTATUS ($2002) value |
| `oam_addr` | `uint8_t` | Current OAM address |
| `in_vblank` | `bool` | VBlank flag status |
| `sprite0_hit` | `bool` | Sprite 0 hit flag |
| `sprite_overflow` | `bool` | Sprite overflow flag |
| `frame_count` | `uint64_t` | Total frames rendered |

### APUSnapshot Fields

| Field | Type | Description |
|-------|------|-------------|
| `frame_counter_cycle` | `int` | Current frame counter cycle |
| `frame_irq_flag` | `bool` | Frame counter IRQ flag |
| `frame_irq_inhibit` | `bool` | Frame IRQ inhibit mode |
| `five_step_mode` | `bool` | 5-step frame counter mode |
| `dmc_irq_flag` | `bool` | DMC IRQ flag |
| `pulse1` / `pulse2` | `APUChannelSnapshot` | Pulse channel state |
| `triangle` | `APUChannelSnapshot` | Triangle channel state |
| `noise` | `APUChannelSnapshot` | Noise channel state |
| `dmc.enabled` | `bool` | DMC enabled |
| `dmc.bytes_remaining` | `int` | DMC bytes left to play |
| `dmc.output_level` | `uint8_t` | DMC DAC level |

Each `APUChannelSnapshot` has: `timer`, `length_counter`, `enabled`, `volume`.

## Side-Effect-Free Memory Reads

Normal memory reads through `nes_cpu_read()` trigger I/O register side
effects (e.g., reading $2002 clears the VBlank flag). The debug read
functions avoid this:

### debug_read_cpu(nes, addr)

Reads from the CPU address space without side effects:
- **$0000-$1FFF**: Returns RAM (with mirroring)
- **$2000-$401F**: Returns 0 (avoids PPU/APU side effects)
- **$6000-$7FFF**: Returns PRG RAM (via mapper if present)
- **$8000-$FFFF**: Returns PRG ROM (via mapper if present)

### debug_read_cpu_word(nes, addr)

Reads a 16-bit little-endian word (two `debug_read_cpu` calls).

### debug_read_ppu_vram(nes, addr)

Reads from the PPU address space:
- **$0000-$1FFF**: Pattern tables (via mapper for bank-switched CHR)
- **$2000-$3EFF**: Nametables (with mirroring applied)
- **$3F00-$3FFF**: Palette (with palette mirroring)

### debug_read_oam(nes, index)

Reads a byte from OAM at the given index (0-255).

## Flag Formatting

```c
const char *flags = debug_format_cpu_flags(cpu_snap.P);
// Returns "NV-BDIZC" format
// Set flags are uppercase, clear flags are lowercase
// Example: P=0x24 -> "nv-BdIzc" (Break and Interrupt set)
```

The function returns a pointer to a static buffer (not thread-safe, but fine
for single-threaded emulation).

## CHR Debug Visualization

The CHR debug renderer (`src/nes/debug_chr.h`) renders both pattern tables
as a 256x128 pixel image. This is useful for verifying CHR bank switching
and tile data:

```c
// Render to buffer
uint8_t buf[256 * 128 * 3];  // RGB888
debug_chr_render(&nes, buf);

// Or save directly as PPM
debug_chr_save_ppm(&nes, "chr_debug.ppm");
```

The output shows:
- **Left half (128x128)**: Pattern table 0 ($0000-$0FFF) -- 16x16 tiles
- **Right half (128x128)**: Pattern table 1 ($1000-$1FFF) -- 16x16 tiles

Colors are grayscale: pixel value 0=black, 1=dark gray, 2=light gray, 3=white.

## Example: Inspecting State in a Frame Hook

```c
void on_frame(uint64_t frame, uint64_t cycles) {
    CPUSnapshot cpu;
    PPUSnapshot ppu;
    debug_get_cpu_snapshot(&nes, &cpu);
    debug_get_ppu_snapshot(&nes, &ppu);

    printf("Frame %llu: PC=$%04X flags=%s VBL=%d\n",
           frame, cpu.PC, debug_format_cpu_flags(cpu.P), ppu.in_vblank);

    // Safe memory read (won't affect emulation)
    uint8_t mario_x = debug_read_cpu(&nes, 0x0086);
    printf("  Mario X position: $%02X\n", mario_x);
}

debug_hooks.on_frame = on_frame;
```

## Related Files

- `src/nes/debug.h` -- snapshot struct definitions and function declarations
- `src/nes/debug.c` -- snapshot capture and safe read implementations
- `src/nes/debug_chr.h` -- CHR pattern table renderer
- `src/nes/screenshot.h` -- framebuffer PPM output
