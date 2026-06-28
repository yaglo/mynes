# Mapper System

## Overview

NES cartridges use mapper chips to extend the console's 32KB PRG ROM and 8KB
CHR ROM address spaces via bank switching. The emulator implements 7 mappers
covering the majority of commercially released games.

## Mapper Interface

Each mapper implements the `MapperOps` vtable defined in
`src/nes/mappers/mapper_ops.h`:

```c
typedef struct MapperOps {
    void (*init)(Mapper *m);
    uint8_t (*cpu_read)(Mapper *m, uint16_t addr);
    void (*cpu_write)(Mapper *m, uint16_t addr, uint8_t val);
    uint8_t (*ppu_read)(Mapper *m, uint16_t addr);
    void (*ppu_write)(Mapper *m, uint16_t addr, uint8_t val);
} MapperOps;
```

The dispatcher in `src/nes/mapper.c` resolves the vtable by mapper number:

```c
static const MapperOps *mapper_ops_for(uint8_t number) {
    switch (number) {
    case 0: return &mapper0_ops;
    case 1: return &mapper1_ops;
    // ...
    }
}
```

## Implemented Mappers

### Mapper 0: NROM (`mapper_nrom.c`)

No bank switching. PRG ROM is fixed at $8000-$FFFF (16KB mirrored or 32KB).
CHR is either 8KB ROM or 8KB RAM.

**Games**: Super Mario Bros, Donkey Kong, Ice Climber

### Mapper 1: MMC1 (`mapper_mmc1.c`)

Serial shift register interface. Writes to $8000-$FFFF shift a bit into a
5-bit register; after 5 writes, the register is committed to one of 4
internal registers based on address.

- PRG modes: 32KB switching, fix-first/fix-last 16KB
- CHR modes: 8KB or 4KB switching
- Dynamic mirroring control (H, V, single-screen low/high)
- Writing bit 7 resets the shift register

**Games**: Legend of Zelda, Metroid, Mega Man 2

### Mapper 2: UxROM (`mapper_uxrom.c`)

Simple PRG bank switching. Writes to $8000-$FFFF select the 16KB bank at
$8000; the last bank is fixed at $C000. CHR RAM only.

**Games**: Contra, Castlevania, DuckTales

### Mapper 3: CNROM (`mapper_cnrom.c`)

Simple CHR bank switching. Writes to $8000-$FFFF select the 8KB CHR bank.
PRG ROM is fixed (same as NROM).

**Games**: Solomon's Key, Gradius, Arkanoid

### Mapper 4: MMC3 (`mapper_mmc3.c`)

Advanced mapper with 8 configurable bank registers, scanline counter IRQ,
and flexible PRG/CHR banking modes.

- PRG: two switchable 8KB banks + two fixed banks
- CHR: six 1KB/2KB banks with configurable layout
- Scanline counter: counts A12 transitions for mid-frame effects
- IRQ: fires after N scanlines (used for split-screen scrolling)

**Games**: Super Mario Bros 3, Kirby's Adventure, Mega Man 3-6

### Mapper 7: AxROM (`mapper_axrom.c`)

32KB PRG bank switching with single-screen mirroring control.
Writes to $8000-$FFFF select the PRG bank (bits 0-2) and nametable
(bit 4: 0=low, 1=high).

**Games**: Battletoads, Marble Madness

### Mapper 10: MMC4 (`mapper_mmc4.c`)

Similar to MMC2 (PxROM). Features latch-triggered CHR bank switching: PPU
reads of specific tile addresses ($FD/$FE) automatically switch CHR banks.

- PRG: one switchable 16KB bank at $8000, last bank fixed at $C000
- CHR: two 4KB banks, each with two selectable banks triggered by latches
- Mirroring: horizontal or vertical

**Games**: Fire Emblem, Fire Emblem Gaiden

## Shared Mapper State

All mapper-specific state lives in the `Mapper` struct (`src/nes/mapper.h`).
Common fields:

| Field | Description |
|-------|-------------|
| `prg_rom` / `prg_rom_size` | PRG ROM data and size |
| `chr_rom` / `chr_rom_size` | CHR ROM data and size |
| `chr_ram[0x2000]` | 8KB CHR RAM (when `has_chr_ram` is true) |
| `prg_ram[0x2000]` | 8KB PRG RAM at $6000-$7FFF |
| `prg_bank0` / `prg_bank1` | Active PRG bank indices |
| `chr_bank0` / `chr_bank1` | Active CHR bank indices |
| `mirroring` | Current nametable mirroring mode |

Mapper-specific fields use prefixed names (`mmc1_shift`, `mmc3_banks`, etc.).

## How to Add a New Mapper

1. Create `src/nes/mappers/mapper_XXXX.c` implementing the 5 `MapperOps`
   functions.

2. Declare the ops table in `src/nes/mappers/mapper_ops.h`:
   ```c
   extern const MapperOps mapperXX_ops;
   ```

3. Add the case to `mapper_ops_for()` in `src/nes/mapper.c`:
   ```c
   case XX: return &mapperXX_ops;
   ```

4. Add the mapper number to `mapper_supported()` in `mapper.c`.

5. Add the source file to `CMakeLists.txt` under `nes_static`.

6. If the mapper needs new state, add fields to the `Mapper` struct in
   `mapper.h` with an appropriate prefix.

7. Update the supported mapper check in `src/nes/rom.h` if needed.

## Related Files

- `src/nes/mapper.h` -- Mapper struct and function declarations
- `src/nes/mapper.c` -- Dispatcher and lifecycle
- `src/nes/mappers/mapper_ops.h` -- MapperOps vtable definition
- `src/nes/mappers/mapper_*.c` -- Per-mapper implementations
- `src/nes/rom.h` -- iNES ROM parser (mapper detection)
