# PPU Architecture

## Overview

The PPU (`src/ppu/ppu.h`) emulates the NES 2C02 picture processing unit with
dot-by-dot accuracy. Each call to `ppu_step()` advances exactly one PPU dot.
The NES system calls it 3 times per CPU cycle.

## Frame Timing

| Region | Dots/Scanline | Scanlines/Frame | Pre-render | VBlank Start |
|--------|---------------|-----------------|------------|--------------|
| NTSC | 341 | 262 | 261 | 241 |
| PAL | 341 | 312 | 311 | 241 |

Scanline phases:
- **0-239**: Visible scanlines (rendering)
- **240**: Post-render (idle)
- **241**: VBlank begins (NMI fires if enabled)
- **242-260/310**: VBlank continues
- **261/311**: Pre-render scanline (next-frame setup)

## Loopy Scrolling Registers

The PPU uses two 15-bit registers for VRAM addressing, per the "loopy"
documentation on nesdev:

```
v (current VRAM address):  yyy NN YYYYY XXXXX
t (temporary address):     yyy NN YYYYY XXXXX
x (fine X scroll):         3 bits
w (write toggle):          1 bit
```

Where:
- `XXXXX` = coarse X scroll (tile column, 0-31)
- `YYYYY` = coarse Y scroll (tile row, 0-29)
- `NN` = nametable select (0-3)
- `yyy` = fine Y scroll (pixel row within tile, 0-7)

Register updates from CPU writes:
- `$2000 (CTRL)`: nametable bits -> t[11:10]
- `$2005 (SCROLL)` first write: coarse X -> t[4:0], fine X -> x
- `$2005 (SCROLL)` second write: coarse Y -> t[9:5], fine Y -> t[14:12]
- `$2006 (ADDR)` first write: high byte -> t[13:8]
- `$2006 (ADDR)` second write: low byte -> t[7:0], then v <- t

## Rendering Pipeline

### Background (dots 1-256, 321-336)

Every 8 dots, the PPU fetches a tile:

| Dot (mod 8) | Action |
|-------------|--------|
| 0 | Load shift registers from latches |
| 1 | Fetch nametable byte (tile ID) |
| 3 | Fetch attribute byte (palette select) |
| 5 | Fetch pattern table low bitplane |
| 7 | Fetch pattern table high bitplane, increment coarse X |

At dot 256, coarse Y is incremented. At dot 257, horizontal position is
copied from t to v.

### Background Pixel Selection

Each dot, the shift registers are shifted left and a 4-bit pixel is
assembled:

```c
uint8_t bit_mux = 0x8000 >> ppu->x;  // fine X selects bit position
uint8_t p0 = (bg_shift_pattern_lo & bit_mux) ? 1 : 0;
uint8_t p1 = (bg_shift_pattern_hi & bit_mux) ? 1 : 0;
uint8_t a0 = (bg_shift_attrib_lo & bit_mux) ? 1 : 0;
uint8_t a1 = (bg_shift_attrib_hi & bit_mux) ? 1 : 0;
uint8_t bg_pixel = (a1 << 3) | (a0 << 2) | (p1 << 1) | p0;
```

### Sprite Evaluation (dots 1-256 on visible scanlines)

At dot 65, the PPU scans all 64 OAM entries for sprites that overlap the
current scanline:

1. Compare each sprite's Y coordinate against the current scanline.
2. Copy matching sprites to secondary OAM (max 8 per scanline).
3. If a 9th sprite is found, set the overflow flag in `$2002`.
4. Track whether sprite 0 is among the selected sprites.

### Sprite Pattern Fetch (dot 257)

For each sprite in secondary OAM:

1. Calculate the row offset within the sprite tile.
2. Apply vertical flip if the attribute bit is set.
3. For 8x16 sprites, select the correct tile half.
4. Fetch low and high pattern bitplanes.
5. Apply horizontal flip if needed (bit reversal).

### Priority Multiplexer

Each visible dot combines the background and sprite pixels:

| BG Pixel | Sprite Pixel | Sprite Priority | Result |
|----------|-------------|-----------------|--------|
| 0 | 0 | - | Background color ($3F00) |
| 0 | != 0 | - | Sprite pixel |
| != 0 | 0 | - | BG pixel |
| != 0 | != 0 | Front (0) | Sprite pixel |
| != 0 | != 0 | Behind (1) | BG pixel |

**Sprite 0 hit** is flagged when both BG and sprite 0 have non-zero pixels
at the same dot (regardless of priority). This does not trigger on the
leftmost 8 pixels if clipping is enabled, and not at dot 255.

## VBlank and NMI

VBlank flag is set at dot 1 of scanline 241. If `$2000` bit 7 (NMI enable)
is set, the PPU asserts its NMI output line.

The NES system performs edge detection on the NMI output:

```c
if (ppu->nmi_output && !nes->prev_nmi) {
    nes->nmi_edge_detected = true;
}
```

NMI delivery is delayed until the CPU is mid-instruction (`uPC != 0`), so
it fires at the start of the instruction *after* the one during which VBlank
was set.

### VBL Suppression

Reading `$2002` during the exact dot when VBlank is being set (scanline 241,
dots 1-2) suppresses the VBlank flag and/or NMI. The PPU sets
`suppress_nmi_edge` which the NES system uses to cancel pending NMI edges.

## Open Bus Decay

The PPU data bus latch decays bit-by-bit over approximately 600ms (~36
frames). Each bit has an independent timestamp tracking when it was last
refreshed. Write-only register reads ($2000, $2001, etc.) return the decayed
bus value.

## Odd Frame Skip

On NTSC, odd frames skip the last dot of the pre-render scanline when
rendering is enabled (dot 339 is skipped, reducing the frame by 1 PPU cycle).
PAL does not do this.

## Related Files

- `src/ppu/ppu.h` -- complete PPU implementation
- `src/nes/nes.h` -- PPU/CPU interleaving and NMI edge detection
- `src/nes/debug.h` -- PPUSnapshot for side-effect-free inspection
- `src/nes/screen_dump.h` -- nametable-to-text conversion
