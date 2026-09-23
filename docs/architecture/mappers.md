# Mapper System

## Overview

NES cartridges use mapper chips to extend the console's 32KB PRG ROM and 8KB
CHR ROM address spaces via bank switching. The dispatcher supports mapper
numbers 0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 34, 66, 69, 71, 206 and 227. Support
depth varies; in particular, MMC5 still has unimplemented features listed
below, and [Known Gaps](#known-gaps) collects what is known not to work.

A ROM with any other mapper number is refused by the loader with an error
message that names the number ("Unsupported mapper 24"), so it can be
reported as-is.

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
    void (*ppu_address)(Mapper *m, uint16_t addr);
    void (*ppu_bus_read)(Mapper *m, uint16_t addr);
    void (*cpu_clock)(Mapper *m);
    void (*scanline)(Mapper *m);
} MapperOps;
```

The dispatcher in `src/nes/mapper.c` resolves the vtable by mapper number.
Its switch is the single list of implemented mappers: `mapper_supported()`
returns whether the lookup finds an entry, and the ROM loader in
`src/nes/rom.h` gates on `mapper_supported()`, so the two cannot disagree
(`rom_tests` checks this for all 256 numbers).

```c
static const MapperOps *mapper_ops_lookup(uint8_t number) {
    switch (number) {
    case 0: return &mapper0_ops;
    case 1: return &mapper1_ops;
    // ...
    default: return NULL;
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
- TVROM (Rad Racer II): the header's four-screen bit gives four separate
  nametables, and $A000 writes are ignored because the board does not wire
  the MMC3's mirroring output

**Games**: Super Mario Bros 3, Kirby's Adventure, Mega Man 3-6

### Mapper 5: MMC5 (`mapper_mmc5.c`)

PRG/CHR banking, fetch-dependent CHR set selection, independently selected
CIRAM/ExRAM/fill nametable quadrants, multiply registers and scanline IRQ.
The IRQ observes actual PPU reads and CPU-idle gaps rather than counting
frontend scanline callbacks. Tests cover 8×8/8×16 CHR selection, all CHR bank
sizes, fill/ExRAM routing, and odd/even frame IRQ boundaries. Castlevania III
USA/PAL replays progress through title and gameplay.

Extended attributes, vertical split, PCM and full banked PRG-RAM behavior
above $8000 remain incomplete. Working Castlevania III is not proof of complete
MMC5 compatibility.

### Mapper 7: AxROM (`mapper_axrom.c`)

32KB PRG bank switching with single-screen mirroring control.
Writes to $8000-$FFFF select the PRG bank (bits 0-2) and nametable
(bit 4: 0=low, 1=high).

**Games**: Battletoads, Marble Madness

### Mapper 9: MMC2 (`mapper_mmc2.c`)

PxROM. Shares the MMC4 CHR latch state: one switchable 8KB PRG bank at
$8000-$9FFF with the last three 8KB banks fixed at $A000-$FFFF; two 4KB CHR
windows, each with a $FD and an $FE bank ($B000/$C000 for $0000, $D000/$E000
for $1000). Latch 0 only reacts to the exact fetches $0FD8 and $0FE8, latch 1
to the rows $1FD8-$1FDF and $1FE8-$1FEF; the triggering fetch still returns
the old bank. $F000 bit 0 selects vertical (0) or horizontal (1) mirroring.
No MMC2 board has four-screen VRAM, but the Punch-Out!! (U) dump sets the
header's four-screen bit (flags 6 = $99), so the first $F000 write replaces
the four-screen layout the loader starts with.

**Games**: Punch-Out!!, Mike Tyson's Punch-Out!!

### Mapper 10: MMC4 (`mapper_mmc4.c`)

Similar to MMC2 (PxROM). Features latch-triggered CHR bank switching: PPU
reads of specific tile addresses ($FD/$FE) automatically switch CHR banks.

- PRG: one switchable 16KB bank at $8000, last bank fixed at $C000
- CHR: two 4KB banks, each with two selectable banks triggered by latches
- Mirroring: horizontal or vertical

**Games**: Fire Emblem, Fire Emblem Gaiden

### Mapper 11: Color Dreams (`mapper_colordreams.c`)

One latch at $8000-$FFFF: bits 0-1 select the 32KB PRG bank, bits 4-7 the
8KB CHR bank. The board has no write protection, so the latch receives the
written value ANDed with the ROM byte at that address (bus conflict).
Mirroring is hard-wired.

**Games**: Crystal Mines, Bible Adventures

### Mapper 34: BNROM / NINA-001 (`mapper_bnrom.c`)

Two boards under one number, told apart by the CHR type. With CHR RAM it is
BNROM: writes to $8000-$FFFF select the 32KB PRG bank. With CHR ROM it is
NINA-001: $7FFD selects the 32KB PRG bank, $7FFE and $7FFF the 4KB CHR banks
at $0000 and $1000; the registers sit inside PRG RAM ($6000-$7FFF) and read
back as RAM.

**Games**: Deadly Towers (BNROM), Impossible Mission II (NINA-001)

### Mapper 66: GxROM (`mapper_gxrom.c`)

One latch at $8000-$FFFF: bits 4-5 select the 32KB PRG bank, bits 0-1 the
8KB CHR bank. Mirroring is hard-wired.

**Games**: Super Mario Bros. / Duck Hunt, Dragon Power

### Mapper 69: FME-7 (`mapper_fme7.c`)

Command/parameter registers select 1KB CHR and 8KB PRG banks, mirroring and
the IRQ counter. Sunsoft 5B expansion audio is not implemented.

### Mapper 71: Camerica/Codemasters (`mapper_camerica.c`)

UxROM-like: writes to $C000-$FFFF select the 16KB PRG bank at $8000, the
last bank is fixed at $C000, CHR is 8KB RAM. Writes to $8000-$9FFF set
single-screen mirroring from bit 4 (0 = low, 1 = high). Only the BF9097
board (Fire Hawk) has that latch, but no other mapper 71 game writes there,
so it is decoded unconditionally. $A000-$BFFF is not decoded.

**Games**: Micro Machines, Bee 52, Fire Hawk

### Mapper 206: Namco 108 / DxROM (`mapper_namco108.c`)

The MMC3's predecessor, implemented separately rather than aliased: the
$8000/$8001 bank-select/bank-data pair with a fixed layout. R0/R1 are 2KB
CHR banks at $0000/$0800, R2-R5 1KB banks at $1000-$1C00, R6/R7 8KB PRG
banks at $8000/$A000, with the last two PRG banks fixed. CHR values are six
bits wide, the mode bits of the bank-select value are ignored, and there is
no IRQ counter, mirroring control or PRG RAM. Only A0 is decoded within
$8000-$FFFF, so the pair repeats across the whole range. The `mmc3_banks`
registers are reused as state. The DRROM board (Gauntlet) carries four-screen
VRAM, which the header's four-screen bit selects.

**Games**: Gauntlet, Pac-Mania, Karnov, Dragon Spirit

### Mapper 227: multicart (`mapper_227.c`)

Address-latched PRG banking and mirroring: the CPU write address selects
the bank/mode and the written value is ignored. Includes 1200-in-1 layouts.

## Known Gaps

- **MMC5**: extended attributes, vertical split, PCM and banked PRG RAM
  above $8000 (see the mapper 5 section).
- **FME-7**: Sunsoft 5B expansion audio is silent.
- **Mapper 34**: iNES 1.0 headers cannot say which board a ROM is; the CHR
  type heuristic is wrong for the rare BNROM dump that carries CHR ROM, and
  submapper hints in NES 2.0 headers are not read.
- **Mapper 71**: the mirroring latch is decoded for every mapper 71 ROM. A
  homebrew or hack that writes to $8000-$9FFF for another purpose would
  switch to single-screen mirroring.
- **Bus conflicts** are emulated only where the track called for them
  (mapper 11). UxROM, CNROM, AxROM, BNROM and GxROM boards also have them on
  real hardware; a program that relies on the conflict for its result will
  differ.
- No mapper handles NES 2.0 submapper numbers or PRG/CHR RAM sizes; every
  board gets 8KB PRG RAM at $6000 (except mappers 206 and 227) and 8KB CHR RAM when
  the header lists no CHR ROM.

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
Related boards share them rather than adding one field per mapper: MMC2 uses
the `mmc4_*` latch state and Namco 108 the `mmc3_bank_select`/`mmc3_banks`
registers, and the discrete-logic boards (11, 34, 66, 71) only need the
generic `prg_bank*`/`chr_bank*` fields.

## How to Add a New Mapper

1. Create `src/nes/mappers/mapper_XXXX.c` implementing the 5 `MapperOps`
   functions.

2. Declare the ops table in `src/nes/mappers/mapper_ops.h`:
   ```c
   extern const MapperOps mapperXX_ops;
   ```

3. Add the case to `mapper_ops_lookup()` in `src/nes/mapper.c`:
   ```c
   case XX: return &mapperXX_ops;
   ```
   `mapper_supported()` and the ROM loader's gate follow from that switch;
   nothing else needs updating for the number to be accepted.

4. Add the source file to `CMakeLists.txt` under `nes_static`.

5. If the mapper needs new state, prefer an existing generic field
   (`prg_bank*`, `chr_bank*`, `mmc3_banks`, the `mmc4_*` latches) before
   adding one to the `Mapper` struct in `mapper.h`.

6. Add `tests/nes/test_mapperXX.c` in the style of the existing mapper tests
   (synthetic PRG/CHR filled with bank numbers, write the registers, check
   `mapper_cpu_read`, `mapper_ppu_read` and `mapper_get_mirroring`) and
   register it with `add_executable`/`add_test` in `CMakeLists.txt`.

7. Document the board and anything known not to work in this file.

## Related Files

- `src/nes/mapper.h` -- Mapper struct and function declarations
- `src/nes/mapper.c` -- Dispatcher and lifecycle
- `src/nes/mappers/mapper_ops.h` -- MapperOps vtable definition
- `src/nes/mappers/mapper_*.c` -- Per-mapper implementations
- `src/nes/rom.h` -- iNES ROM parser (mapper detection, gated on
  `mapper_supported()`)
- `tests/nes/test_mapper*.c` -- Per-mapper register/banking tests
