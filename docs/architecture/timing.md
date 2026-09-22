# Timing

## Master Clock

The NES master clock runs at 21.477272 MHz (NTSC). Each component divides
this clock differently:

| Component | Divider | Frequency | Ratio |
|-----------|---------|-----------|-------|
| PPU | /4 | 5,369,318 Hz | 1x |
| CPU | /12 | 1,789,773 Hz | 1:3 PPU |
| APU | /12 | 1,789,773 Hz | Same as CPU |

This means for every NTSC CPU cycle, the PPU advances 3 dots. PAL divides
its master clock by 16 for the CPU and by 5 for the PPU, averaging 3.2 dots
per CPU cycle. The master-tick scheduler retains the fractional alignment.

## NTSC vs PAL

| Parameter | NTSC | PAL |
|-----------|------|-----|
| Master clock | 21,477,272 Hz | 26,601,712 Hz |
| CPU clock | 1,789,773 Hz | 1,662,607 Hz |
| CPU / PPU master dividers | 12 / 4 | 16 / 5 |
| PPU dots/line | 341 | 341 |
| Scanlines/frame | 262 | 312 |
| Pre-render line | 261 | 311 |
| Frame rate | ~60.0988 Hz | ~50.0070 Hz |
| Odd frame skip | Yes | No |
| CPU cycles/frame | ~29,781 | ~33,248 |

Region is configured via `nes_set_region()` which calls both
`ppu_set_region()` and `apu_set_region()`.

## Execution Order in nes_step()

Each call to `nes_step()` represents one CPU cycle. The execution order
within a single step is carefully chosen for timing accuracy:

```
1. Step APU; update controller strobe and CPU IRQ levels
2. Advance PPU to the configured CPU bus master tick
3. Refresh IRQ after PPU mapper activity; clock MMC5 CPU-idle detection
4. Arbitrate DMA and CPU RDY; execute the CPU microcycle and tracing
5. Advance master_tick by 12 (NTSC) or 16 (PAL)
6. Advance PPU to that tick using its regional dot divider
7. Notify legacy scanline mappers and synchronize mirroring
8. Process NMI suppression and edge transfer
9. Increment the legacy CPU-cycle counter; check frame completion
```

The default NTSC alignment corresponds to one dot before and two after the
CPU bus access. PAL uses the same master-tick mechanism without rounding its
16:5 ratio to three dots. Selected PPU register reads synchronize to their
later bus-sampling phase. `master_tick` counts master ticks; the legacy
`master_clock` field counts CPU cycles and remains for hook compatibility.

## DMA Cycle Stealing

### OAM DMA (triggered by $4014 write)

The OAM DMA halts the CPU and transfers 256 bytes to OAM:

| Phase | Cycles | Description |
|-------|--------|-------------|
| Alignment | 1-2 | Wait for even CPU cycle (extra if odd start) |
| Transfer | 512 | 256 read-write pairs |
| **Total** | **513-514** | |

During OAM DMA, the PPU and APU continue running normally. The alignment
cycle reads the CPU's last-read address, which matters for side-effect
registers.

### DMC DMA (triggered by empty sample buffer)

The DMC DMA steals fewer cycles to fetch one sample byte:

| Phase | Cycles | Description |
|-------|--------|-------------|
| Halt | 1 | Repeat last CPU read |
| Dummy | 1 | Repeat last CPU read |
| Alignment | 0-1 | Extra cycle if odd CPU cycle |
| Fetch | 1 | Actual sample read from ROM |
| **Total** | **3-4** | |

DMC DMA can interrupt OAM DMA. If both are active, DMC DMA takes priority
for the bus but OAM DMA resumes afterward.

## NMI Timing

NMI delivery involves several timing-sensitive steps:

1. **PPU sets VBlank** at scanline 241, dot 1.
2. **PPU asserts NMI output** if $2000 bit 7 is set.
3. **NES detects rising edge** (`nmi_output` goes from low to high).
4. **NMI edge is held** until CPU is mid-instruction (`uPC != 0`).
5. **CPU sees `nmi_pending`** and enters NMI handler at next instruction
   boundary.

If `nmi_output` goes low before step 4 (e.g., NMI is disabled by writing
to $2000), the pending edge is canceled.

If $2002 is read during the VBL-set window (scanline 241, dots 1-2), the
NMI is suppressed:

```c
if (ppu->scanline == 241 && ppu->dot >= 1 && ppu->dot <= 2) {
    ppu->suppress_nmi_edge = true;
}
```

## Frame Counter Timing

The APU frame counter runs on specific CPU cycle counts. The counter resets
at the end of each frame sequence:

**4-step mode**: 29,829 cycles (generates IRQ if not inhibited)
**5-step mode**: 37,280 cycles (never generates IRQ)

When $4017 is written, the frame counter resets after a 3-4 cycle delay
(depending on whether the write occurs on an even or odd CPU cycle).

## Related Files

- `src/nes/nes.h` -- `nes_step()`, DMA implementation
- `include/nes/types.h` -- clock constants
- `src/ppu/ppu.h` -- region-specific scanline counts
- `src/nes/apu.h` -- frame counter step tables, clock rates
