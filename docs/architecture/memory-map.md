# Memory Map

## CPU Address Space ($0000-$FFFF)

| Range | Size | Description |
|-------|------|-------------|
| $0000-$07FF | 2 KB | Internal RAM |
| $0800-$1FFF | 6 KB | Mirrors of $0000-$07FF (repeats 3x) |
| $2000-$2007 | 8 bytes | PPU registers |
| $2008-$3FFF | ~8 KB | Mirrors of $2000-$2007 (repeats every 8 bytes) |
| $4000-$4013 | 20 bytes | APU registers |
| $4014 | 1 byte | OAM DMA (write triggers 513-514 cycle transfer) |
| $4015 | 1 byte | APU status (read/write) |
| $4016 | 1 byte | Controller 1 data / strobe (read/write) |
| $4017 | 1 byte | Controller 2 data (read) / Frame counter (write) |
| $4018-$401F | 8 bytes | Normally disabled APU test mode |
| $4020-$5FFF | ~8 KB | Cartridge expansion area |
| $6000-$7FFF | 8 KB | Cartridge PRG RAM (battery-backed on some games) |
| $8000-$FFFF | 32 KB | Cartridge PRG ROM (bank-switched by mapper) |

### Special Addresses

| Address | Description |
|---------|-------------|
| $FFFA-$FFFB | NMI vector (little-endian) |
| $FFFC-$FFFD | Reset vector (little-endian) |
| $FFFE-$FFFF | IRQ/BRK vector (little-endian) |

### Memory Access Implementation

Reads and writes go through `nes_cpu_read()` / `nes_cpu_write()` in
`src/nes/nes.h`. The routing logic:

```c
static inline uint8_t nes_cpu_read(CPU *cpu, uint16_t addr) {
    NES *nes = (NES *)cpu->user_data;
    if (addr < 0x2000)       return nes->ram[addr & 0x07FF];
    else if (addr < 0x4000)  return ppu_reg_read(nes->ppu, addr);
    else if (addr < 0x4018)  /* APU/IO routing */
    else if (addr < 0x6000)  return nes->open_bus;
    else                     return mapper_cpu_read(nes->mapper, addr);
}
```

## PPU Address Space ($0000-$3FFF)

| Range | Size | Description |
|-------|------|-------------|
| $0000-$0FFF | 4 KB | Pattern table 0 (CHR ROM/RAM, via mapper) |
| $1000-$1FFF | 4 KB | Pattern table 1 (CHR ROM/RAM, via mapper) |
| $2000-$23FF | 1 KB | Nametable 0 |
| $2400-$27FF | 1 KB | Nametable 1 |
| $2800-$2BFF | 1 KB | Nametable 2 (mirror depends on mirroring mode) |
| $2C00-$2FFF | 1 KB | Nametable 3 (mirror depends on mirroring mode) |
| $3000-$3EFF | ~4 KB | Mirror of $2000-$2EFF |
| $3F00-$3F1F | 32 bytes | Palette RAM |
| $3F20-$3FFF | 224 bytes | Mirrors of palette |

### Nametable Mirroring

The NES has only 2 KB of VRAM (two physical nametables). The mirroring mode
determines how the four logical nametables map to physical memory:

| Mode | $2000 | $2400 | $2800 | $2C00 |
|------|-------|-------|-------|-------|
| Horizontal | NT0 | NT0 | NT1 | NT1 |
| Vertical | NT0 | NT1 | NT0 | NT1 |
| Single-low | NT0 | NT0 | NT0 | NT0 |
| Single-high | NT1 | NT1 | NT1 | NT1 |

Mirroring is controlled by the cartridge (hardwired or mapper-controlled).
The PPU applies mirroring in `ppu_read()` / `ppu_write()` using a switch
on `ppu->mirroring`.

### Palette Mirroring

Palette addresses $3F10, $3F14, $3F18, $3F1C mirror $3F00, $3F04, $3F08,
$3F0C respectively. This means the background color at index 0 of each
sprite palette is actually the universal background color:

```c
if ((addr & 0x13) == 0x10) addr &= ~0x10;  // Mirror sprite BG to universal BG
```

## Open Bus Behavior

The NES data bus retains the last value driven on it. When no device responds
to a read, the CPU sees this "open bus" value.

The emulator tracks this in `nes->open_bus`:

- All writes update `open_bus`.
- RAM and ROM reads update `open_bus`.
- APU/IO reads ($4000-$4017) do NOT update `open_bus` -- the value mixes
  with the existing bus value via bit masking.
- Controller reads return 5 data bits OR'd with 3 bits of open bus:
  ```c
  val = (val & 0x1F) | (nes->open_bus & 0xE0);
  ```
- Expansion area reads ($4020-$5FFF) return `open_bus` unchanged.

### PPU Open Bus

The PPU has its own data bus latch with per-bit decay (~600ms / ~36 frames):

- Write-only registers ($2000, $2001, $2003, $2005, $2006) return the
  decayed bus value on read.
- $2002 (PPUSTATUS) returns bits 7-5 from status, bits 4-0 from bus.
- $2004 (OAMDATA) refreshes the bus.
- $2007 (PPUDATA) refreshes the bus.

## OAM DMA ($4014)

Writing a page number to $4014 triggers a 256-byte transfer from CPU address
space to PPU OAM. The transfer:

1. Halts the CPU (RDY line low).
2. Takes 1-2 alignment cycles (2 if starting on odd CPU cycle).
3. Performs 256 read-write pairs (512 cycles).
4. Total: 513 or 514 cycles.
5. PPU and APU continue running during DMA.

The source address is `(page << 8) | byte_index`. The destination is
`(OAMADDR + byte_index) & 0xFF`, preserving the starting OAMADDR.

## Related Files

- `src/nes/nes.h` -- `nes_cpu_read()`, `nes_cpu_write()`, `nes_dma_step()`
- `src/ppu/ppu.h` -- `ppu_read()`, `ppu_write()`, `ppu_reg_read()`, `ppu_reg_write()`
- `src/nes/mapper.h` -- `mapper_cpu_read()`, `mapper_cpu_write()`
- `include/nes/types.h` -- memory size constants
