# Common Debugging Workflows

## 1. Debugging SMB Collision Issues

Super Mario Bros stores game state in well-known RAM locations. Use frame
hooks to monitor these during gameplay.

### Track Mario Position

```c
void on_frame(uint64_t frame, uint64_t cycles) {
    uint8_t engine = debug_read_cpu(&nes, 0x000E);  // Game engine state
    uint8_t mario_x = debug_read_cpu(&nes, 0x0086); // Mario X position
    uint8_t mario_y = debug_read_cpu(&nes, 0x00CE); // Mario Y position
    uint8_t scroll_x = debug_read_cpu(&nes, 0x071C); // Horizontal scroll

    printf("Frame %llu: engine=$%02X pos=(%02X,%02X) scroll=$%02X\n",
           frame, engine, mario_x, mario_y, scroll_x);
}

hooks_init();
debug_hooks.on_frame = on_frame;
```

### Breakpoint on Position Change

```c
void on_mem(uint16_t addr, uint8_t val, bool is_write) {
    if (is_write && addr == 0x0086) {
        CPUSnapshot snap;
        debug_get_cpu_snapshot(&nes, &snap);
        printf("Mario X written: $%02X (from PC=$%04X)\n", val, snap.PC);
    }
}

debug_hooks.on_mem_access = on_mem;
```

### Check Collision Detection

```c
void on_mem(uint16_t addr, uint8_t val, bool is_write) {
    // SMB collision result at $0490
    if (is_write && addr == 0x0490) {
        printf("Collision result: $%02X\n", val);
    }
}
```

## 2. Tracking Scroll Changes

Scroll glitches are common in games that use mid-frame scroll splits.
Monitor PPU register writes to catch them.

### Log All Scroll Writes

```c
void on_ppu_reg(uint16_t addr, uint8_t val, bool is_write) {
    if (!is_write) return;

    PPUSnapshot ppu;
    debug_get_ppu_snapshot(&nes, &ppu);

    switch (addr & 7) {
    case 0: // PPUCTRL
        printf("[%d:%d] CTRL=$%02X (NT=%d, incr=%d, NMI=%d)\n",
               ppu.scanline, ppu.dot, val,
               val & 3, (val & 4) ? 32 : 1, (val >> 7) & 1);
        break;
    case 5: // PPUSCROLL
        printf("[%d:%d] SCROLL=%s $%02X\n",
               ppu.scanline, ppu.dot, ppu.w ? "Y" : "X", val);
        break;
    case 6: // PPUADDR
        printf("[%d:%d] ADDR=%s $%02X (v=$%04X t=$%04X)\n",
               ppu.scanline, ppu.dot, ppu.w ? "lo" : "hi", val, ppu.v, ppu.t);
        break;
    }
}

debug_hooks.on_ppu_reg = on_ppu_reg;
```

### Detect Mid-Frame Scroll Splits

```c
void on_ppu_reg(uint16_t addr, uint8_t val, bool is_write) {
    if (!is_write) return;

    PPUSnapshot ppu;
    debug_get_ppu_snapshot(&nes, &ppu);

    // Flag writes to PPUSCROLL or PPUADDR during visible scanlines
    if ((addr & 7) == 5 || (addr & 7) == 6) {
        if (ppu.scanline >= 0 && ppu.scanline < 240) {
            printf("MID-FRAME scroll change at [%d:%d]!\n",
                   ppu.scanline, ppu.dot);
        }
    }
}
```

## 3. Finding Sprite Issues

### Dump Sprite 0 Status Each Scanline

```c
void on_scanline(int scanline, uint64_t frame) {
    PPUSnapshot ppu;
    debug_get_ppu_snapshot(&nes, &ppu);

    if (scanline < 240 && ppu.sprite0_hit) {
        printf("Sprite 0 hit at scanline %d (frame %llu)\n",
               scanline, frame);
    }
}

debug_hooks.on_ppu_scanline = on_scanline;
```

### Inspect OAM Contents

```c
void dump_oam(const NES *nes) {
    printf("OAM Contents (first 8 sprites):\n");
    printf("  #   Y  Tile Attr   X\n");
    for (int i = 0; i < 8; i++) {
        uint8_t y    = debug_read_oam(nes, i * 4 + 0);
        uint8_t tile = debug_read_oam(nes, i * 4 + 1);
        uint8_t attr = debug_read_oam(nes, i * 4 + 2);
        uint8_t x    = debug_read_oam(nes, i * 4 + 3);

        char flags[5] = "----";
        if (attr & 0x40) flags[0] = 'H';  // Horizontal flip
        if (attr & 0x80) flags[1] = 'V';  // Vertical flip
        if (attr & 0x20) flags[2] = 'B';  // Behind background
        flags[3] = '0' + (attr & 0x03);   // Palette

        printf("  %d: Y=%3d T=$%02X %s X=%3d\n", i, y, tile, flags, x);
    }
}
```

### Check Sprite Overflow

```c
void on_scanline(int scanline, uint64_t frame) {
    PPUSnapshot ppu;
    debug_get_ppu_snapshot(&nes, &ppu);
    if (ppu.sprite_overflow) {
        printf("Sprite overflow at scanline %d\n", scanline);
    }
}
```

## 4. Diagnosing Mapper Issues

### Log Bank Switches

```c
void on_mem(uint16_t addr, uint8_t val, bool is_write) {
    if (!is_write) return;

    // MMC1: writes to $8000-$FFFF
    if (addr >= 0x8000) {
        printf("Mapper write: $%04X <- $%02X", addr, val);
        if (val & 0x80) printf(" (RESET)");
        printf("\n");
    }
}

debug_hooks.on_mem_access = on_mem;
```

### Verify CHR Banking

```c
void on_frame(uint64_t frame, uint64_t cycles) {
    if (frame % 60 == 0) {  // Every second
        debug_chr_save_ppm(&nes, "chr_debug.ppm");
        printf("CHR debug saved at frame %llu\n", frame);
    }
}
```

## 5. Automated Test Debugging

### Run with Full Traces

```bash
# Trace everything and capture to file
./bin/test_runner rom.nes --blargg --trace all > trace.log 2>&1
echo "Exit code: $?"

# Trace just CPU with memory writes to PRG RAM
./bin/test_runner rom.nes --blargg --trace cpu,memw --trace-mem 6000-7FFF
```

### Compare Against Nestest Log

```bash
# Generate CPU trace
./bin/test_runner nestest.nes --trace cpudetail --frames 100 > my_trace.log

# Compare (first 8000 lines covers the official opcodes)
diff <(head -8000 my_trace.log) <(head -8000 nestest_golden.log)
```

### Screen Dump for Visual Verification

```bash
# Dump nametable as ASCII text
./bin/test_runner rom.nes --blargg --dump-on-exit --frames 600

# With custom character map
./bin/test_runner rom.nes --dump-on-exit --charmap data/charmaps/smb.map
```

## 6. Performance Debugging

### Measure Cycles Per Frame

```c
static uint64_t last_cycles = 0;

void on_frame(uint64_t frame, uint64_t cycles) {
    if (last_cycles > 0) {
        uint64_t delta = cycles - last_cycles;
        printf("Frame %llu: %llu cycles\n", frame, delta);
    }
    last_cycles = cycles;
}
```

### Track IRQ/NMI Frequency

```c
static int nmi_count = 0;
static int irq_count = 0;

void on_irq(bool is_nmi, uint16_t vec, uint16_t ret) {
    if (is_nmi) nmi_count++;
    else irq_count++;
}

void on_frame(uint64_t frame, uint64_t cycles) {
    printf("Frame %llu: %d NMIs, %d IRQs\n", frame, nmi_count, irq_count);
    nmi_count = irq_count = 0;
}
```

## Related Files

- `src/nes/hooks.h` -- hook installation
- `src/nes/debug.h` -- snapshots and safe reads
- `src/nes/trace.h` -- trace categories
- `src/nes/debug_chr.h` -- CHR visualization
- `tools/test_runner.c` -- headless test execution
