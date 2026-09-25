#include <stdio.h>
#include <string.h>
#include "ppu/ppu.h"

static PPU ppu;

/* Init PPU to clean test state (scanline 0, no VBL flag) */
static void test_ppu_init(void) {
    ppu_init(&ppu);
    /* ppu_init sets VBL flag for power-on compatibility with games.
     * Tests need a clean state starting at scanline 0 with no flags. */
    ppu.status = 0;
    ppu.scanline = 0;
    ppu.dot = 0;
}

static void run_dots(unsigned count) {
    while (count--) ppu_step(&ppu);
}

/* Run to specific scanline/dot */
static void run_to(int scanline, int dot) {
    while (ppu.scanline != scanline || ppu.dot != dot) {
        ppu_step(&ppu);
        if (ppu.frame > 2) break; /* Safety limit */
    }
}

/* ============================================================================
 * Tests
 * ============================================================================ */

int test_ppuctrl_write(void) {
    test_ppu_init();

    /* Write to PPUCTRL should update nametable bits in t */
    ppu_reg_write(&ppu, 0x2000, 0x03); /* Nametable = 3 */

    if ((ppu.t & 0x0C00) == 0x0C00 && ppu.ctrl == 0x03) {
        printf("TEST ppuctrl_write: PASS (t=%04X ctrl=%02X)\n", ppu.t, ppu.ctrl);
        return 1;
    } else {
        printf("TEST ppuctrl_write: FAIL (t=%04X ctrl=%02X)\n", ppu.t, ppu.ctrl);
        return 0;
    }
}

int test_ppuscroll_write(void) {
    test_ppu_init();

    /* First write: X scroll */
    ppu_reg_write(&ppu, 0x2005, 0x7D); /* X=125: coarse=15, fine=5 */

    if (ppu.w != true || ppu.x != 5 || (ppu.t & 0x1F) != 15) {
        printf("TEST ppuscroll_write: FAIL after first write (w=%d x=%d t=%04X)\n",
               ppu.w, ppu.x, ppu.t);
        return 0;
    }

    /* Second write: Y scroll */
    ppu_reg_write(&ppu, 0x2005, 0xA3); /* Y=163: coarse=20, fine=3 */

    if (ppu.w != false) {
        printf("TEST ppuscroll_write: FAIL (w not reset)\n");
        return 0;
    }

    /* t should have: fine_y=3 (bits 12-14), coarse_y=20 (bits 5-9) */
    uint16_t expected_fine_y = 3 << 12;
    uint16_t expected_coarse_y = 20 << 5;

    if ((ppu.t & 0x7000) == expected_fine_y && (ppu.t & 0x03E0) == expected_coarse_y) {
        printf("TEST ppuscroll_write: PASS (t=%04X x=%d)\n", ppu.t, ppu.x);
        return 1;
    } else {
        printf("TEST ppuscroll_write: FAIL (t=%04X expected fine_y=%04X coarse_y=%04X)\n",
               ppu.t, expected_fine_y, expected_coarse_y);
        return 0;
    }
}

int test_ppuaddr_write(void) {
    test_ppu_init();

    /* Write $2006 twice to set address $2108 */
    ppu_reg_write(&ppu, 0x2006, 0x21);
    ppu_reg_write(&ppu, 0x2006, 0x08);
    run_dots(2);
    if (ppu.v != 0 || ppu.t != 0x2108) {
        printf("TEST ppuaddr_write: FAIL (address transfer was not delayed)\n");
        return 0;
    }
    ppu_step(&ppu);

    if (ppu.v == 0x2108 && ppu.w == false) {
        printf("TEST ppuaddr_write: PASS (v=%04X)\n", ppu.v);
        return 1;
    } else {
        printf("TEST ppuaddr_write: FAIL (v=%04X w=%d)\n", ppu.v, ppu.w);
        return 0;
    }
}

int test_ppustatus_read_clears_vblank(void) {
    test_ppu_init();

    /* Set vblank flag manually */
    ppu.status = STATUS_VBLANK | STATUS_SPRITE_ZERO;
    ppu.w = true;

    /* Read status */
    uint8_t status = ppu_reg_read(&ppu, 0x2002);

    if ((status & STATUS_VBLANK) && !(ppu.status & STATUS_VBLANK) && !ppu.w) {
        printf("TEST ppustatus_read: PASS (read=%02X status=%02X w=%d)\n",
               status, ppu.status, ppu.w);
        return 1;
    } else {
        printf("TEST ppustatus_read: FAIL (read=%02X status=%02X w=%d)\n",
               status, ppu.status, ppu.w);
        return 0;
    }
}

int test_ppudata_write_increment(void) {
    test_ppu_init();

    /* Set address to $2000 */
    ppu_reg_write(&ppu, 0x2006, 0x20);
    ppu_reg_write(&ppu, 0x2006, 0x00);
    run_dots(3);

    /* Write with increment=1 (default) */
    ppu_reg_write(&ppu, 0x2007, 0x42);

    if (ppu.v != 0x2001) {
        printf("TEST ppudata_write: FAIL (v=%04X after +1 write)\n", ppu.v);
        return 0;
    }

    /* Set increment=32 mode */
    ppu_reg_write(&ppu, 0x2000, CTRL_INCREMENT);
    ppu_reg_write(&ppu, 0x2006, 0x20);
    ppu_reg_write(&ppu, 0x2006, 0x00);
    run_dots(3);
    ppu_reg_write(&ppu, 0x2007, 0x55);

    if (ppu.v == 0x2020) {
        printf("TEST ppudata_write: PASS (v=%04X after +32 write)\n", ppu.v);
        return 1;
    } else {
        printf("TEST ppudata_write: FAIL (v=%04X expected 2020)\n", ppu.v);
        return 0;
    }
}

int test_vblank_timing(void) {
    test_ppu_init();

    /* Run to scanline 241, dot 0 */
    run_to(241, 0);

    if (ppu.status & STATUS_VBLANK) {
        printf("TEST vblank_timing: FAIL (vblank set too early at %d:%d)\n",
               ppu.scanline, ppu.dot);
        return 0;
    }

    /* Step once: execute dot 0, advance to dot 1 */
    ppu_step(&ppu);

    if (ppu.status & STATUS_VBLANK) {
        printf("TEST vblank_timing: FAIL (vblank set at dot 0)\n");
        return 0;
    }

    /* Step again: execute dot 1 (vblank sets here), advance to dot 2 */
    ppu_step(&ppu);

    if (!(ppu.status & STATUS_VBLANK)) {
        printf("TEST vblank_timing: FAIL (vblank not set after dot 1, scanline=%d dot=%d)\n",
               ppu.scanline, ppu.dot);
        return 0;
    }

    printf("TEST vblank_timing: PASS (vblank set at scanline 241 after dot 1)\n");
    return 1;
}

int test_vblank_clear_on_prerender(void) {
    test_ppu_init();

    /* Set flags */
    ppu.status = STATUS_VBLANK | STATUS_SPRITE_ZERO | STATUS_OVERFLOW;

    /* Run to dot 0 of pre-render scanline */
    run_to(261, 0);

    /* Flags should still be set */
    if (!(ppu.status & STATUS_VBLANK)) {
        printf("TEST vblank_clear: FAIL (vblank already cleared at dot 0)\n");
        return 0;
    }

    /* Step to execute dot 0, advance to dot 1 */
    ppu_step(&ppu);

    /* Step to execute dot 1 (flags clear here) */
    ppu_step(&ppu);

    if (ppu.status & (STATUS_VBLANK | STATUS_SPRITE_ZERO | STATUS_OVERFLOW)) {
        printf("TEST vblank_clear: FAIL (flags not cleared: status=%02X)\n", ppu.status);
        return 0;
    }

    printf("TEST vblank_clear: PASS (status=%02X at pre-render)\n", ppu.status);
    return 1;
}

int test_nmi_output(void) {
    test_ppu_init();

    /* Enable NMI in PPUCTRL */
    ppu_reg_write(&ppu, 0x2000, CTRL_NMI_ENABLE);

    /* NMI should not be active yet */
    if (ppu.nmi_output) {
        printf("TEST nmi_output: FAIL (NMI active before vblank)\n");
        return 0;
    }

    /* Run to just before vblank fires (241:0), then execute dot 1 */
    run_to(241, 0);
    ppu_step(&ppu);  /* Execute dot 0 */
    ppu_step(&ppu);  /* Execute dot 1 - vblank and NMI fire here */

    if (!ppu.nmi_output) {
        printf("TEST nmi_output: FAIL (NMI not active at vblank, scanline=%d dot=%d)\n",
               ppu.scanline, ppu.dot);
        return 0;
    }

    printf("TEST nmi_output: PASS (NMI active at vblank)\n");
    return 1;
}

int test_frame_complete(void) {
    test_ppu_init();

    /* Run until frame complete flag is set */
    int dots = 0;
    while (!ppu.frame_complete && dots < DOTS_PER_SCANLINE * SCANLINES_PER_FRAME + 100) {
        ppu_step(&ppu);
        dots++;
    }

    if (ppu.frame_complete) {
        printf("TEST frame_complete: PASS (after %d dots)\n", dots);
        return 1;
    } else {
        printf("TEST frame_complete: FAIL (not set after %d dots)\n", dots);
        return 0;
    }
}

int test_oam_write(void) {
    test_ppu_init();

    /* Set OAM address */
    ppu_reg_write(&ppu, 0x2003, 0x10);

    /* Write OAM data */
    ppu_reg_write(&ppu, 0x2004, 0x42);
    ppu_reg_write(&ppu, 0x2004, 0x55);

    if (ppu.oam[0x10] == 0x42 && ppu.oam[0x11] == 0x55 && ppu.oam_addr == 0x12) {
        printf("TEST oam_write: PASS (oam[10]=%02X oam[11]=%02X addr=%02X)\n",
               ppu.oam[0x10], ppu.oam[0x11], ppu.oam_addr);
        return 1;
    } else {
        printf("TEST oam_write: FAIL (oam[10]=%02X oam[11]=%02X addr=%02X)\n",
               ppu.oam[0x10], ppu.oam[0x11], ppu.oam_addr);
        return 0;
    }
}

int test_palette_mirror(void) {
    test_ppu_init();

    /* Write to $3F00 (universal background) */
    ppu_reg_write(&ppu, 0x2006, 0x3F);
    ppu_reg_write(&ppu, 0x2006, 0x00);
    run_dots(3);
    ppu_reg_write(&ppu, 0x2007, 0x0F);

    /* Read from $3F10 (should mirror $3F00) */
    ppu_reg_write(&ppu, 0x2006, 0x3F);
    ppu_reg_write(&ppu, 0x2006, 0x10);
    run_dots(3);
    ppu_reg_read(&ppu, 0x2007); /* Priming read */
    run_dots(4);
    uint8_t val = ppu_reg_read(&ppu, 0x2007);

    /* Actually check directly */
    if (ppu.palette[0] == 0x0F && ppu_read(&ppu, 0x3F10) == 0x0F) {
        printf("TEST palette_mirror: PASS (palette[0]=%02X)\n", ppu.palette[0]);
        return 1;
    } else {
        printf("TEST palette_mirror: FAIL (palette[0]=%02X read=%02X)\n",
               ppu.palette[0], val);
        return 0;
    }
}

int test_coarse_x_increment(void) {
    test_ppu_init();

    /* Set v to coarse_x = 31 (at nametable boundary) */
    ppu.v = 0x001F;

    ppu_inc_x(&ppu);

    /* Should wrap to 0 and switch nametable */
    if ((ppu.v & 0x001F) == 0 && (ppu.v & 0x0400)) {
        printf("TEST coarse_x_inc: PASS (v=%04X, wrapped with nametable switch)\n", ppu.v);
        return 1;
    } else {
        printf("TEST coarse_x_inc: FAIL (v=%04X)\n", ppu.v);
        return 0;
    }
}

int test_fine_y_increment(void) {
    test_ppu_init();

    /* Set v to fine_y = 7, coarse_y = 29 (at edge) */
    ppu.v = 0x7000 | (29 << 5); /* fine_y=7, coarse_y=29 */

    ppu_inc_y(&ppu);

    /* Should reset fine_y=0, coarse_y=0, and switch vertical nametable */
    if (FINE_Y(ppu.v) == 0 && COARSE_Y(ppu.v) == 0 && (ppu.v & 0x0800)) {
        printf("TEST fine_y_inc: PASS (v=%04X, wrapped with nametable switch)\n", ppu.v);
        return 1;
    } else {
        printf("TEST fine_y_inc: FAIL (v=%04X fine_y=%d coarse_y=%d)\n",
               ppu.v, FINE_Y(ppu.v), COARSE_Y(ppu.v));
        return 0;
    }
}

int test_rendering_basic(void) {
    test_ppu_init();

    /* Enable background rendering */
    ppu_reg_write(&ppu, 0x2001, MASK_BG_ENABLE);

    /* Set up a simple nametable (tile 0 everywhere) */
    /* Pattern table: tile 0 = solid color */
    for (int i = 0; i < 8; i++) {
        ppu.vram[i] = 0xFF;     /* Low plane = all 1s */
        ppu.vram[i + 8] = 0xFF; /* High plane = all 1s -> color 3 */
    }

    /* Palette: color 3 in palette 0 = color index 0x15 */
    ppu.palette[3] = 0x15;

    /* Run one full frame */
    while (!ppu.frame_complete) {
        ppu_step(&ppu);
    }

    /* Check that framebuffer has been filled */
    int non_black = 0;
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT * 3; i++) {
        if (ppu.framebuffer[i] != 0) non_black++;
    }

    if (non_black > 0) {
        printf("TEST rendering_basic: PASS (%d non-black bytes in framebuffer)\n", non_black);
        return 1;
    } else {
        printf("TEST rendering_basic: FAIL (framebuffer is all black)\n");
        return 0;
    }
}

int test_hybrid_fetch_address(void) {
    test_ppu_init();
    ppu_write(&ppu, 0x2C19, 0x11);
    ppu_write(&ppu, 0x2F19, 0xCA);
    ppu.mask = MASK_BG_ENABLE;
    ppu.v = 0x2C19;
    ppu.dot = 1;
    ppu_step(&ppu); /* ALE captures $19. */
    ppu.v = 0x2F00; /* High pins change before /RD; the octal latch does not. */
    ppu_step(&ppu);
    bool pass = ppu.bg_next_tile_id == 0xCA;
    printf("TEST hybrid_fetch_address: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_ppudata_read_sequencer(void) {
    test_ppu_init();
    ppu.v = 0x2108;
    ppu.read_buffer = 0x42;
    ppu_write(&ppu, 0x2108, 0xAB);
    uint8_t value = ppu_reg_read(&ppu, 0x2007);
    run_dots(3);
    bool pass = value == 0x42 && ppu.read_buffer == 0x42 && ppu.v == 0x2108;
    ppu_step(&ppu);
    pass &= ppu.read_buffer == 0xAB && ppu.v == 0x2109;
    printf("TEST ppudata_read_sequencer: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_secondary_oam_restart(void) {
    test_ppu_init();
    memset(ppu.oam, 0xFF, sizeof(ppu.oam));
    ppu.oam[0] = 5;
    ppu.oam[1] = 0xC5;
    ppu.mask = MASK_SPRITE_ENABLE;
    ppu.scanline = 5;
    ppu.dot = 1;
    run_dots(14);
    ppu.mask = 0;
    run_dots(18); /* Skipping part of OAM clear must not offset evaluation. */
    ppu.mask = MASK_SPRITE_ENABLE;
    run_dots(40);
    bool pass = ppu.oam_secondary[0] == 5 && ppu.oam_secondary[1] == 0xC5;
    printf("TEST secondary_oam_restart: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_oam_output_latch(void) {
    test_ppu_init();
    ppu.mask = MASK_SPRITE_ENABLE;
    ppu.scanline = 5;
    ppu.dot = 65;
    ppu.oam[0] = 4;
    ppu.oam_latch = 0xFF;
    ppu_step(&ppu); /* Primary OAM feeds evaluation before the CPU output. */
    bool pass = ppu.oam_latch == 4 && ppu_reg_read(&ppu, 0x2004) == 0xFF;
    ppu_step(&ppu);
    pass &= ppu_reg_read(&ppu, 0x2004) == 4;
    printf("TEST oam_output_latch: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_nmi_disable_gates_output(void) {
    test_ppu_init();
    ppu.nmi_occurred = true;
    ppu_reg_write(&ppu, 0x2000, CTRL_NMI_ENABLE);
    run_dots(2);
    bool pass = ppu.nmi_output && ppu.nmi_edge_pending;
    ppu_reg_write(&ppu, 0x2000, 0);
    pass &= !ppu.nmi_output && !ppu.nmi_edge_pending;
    run_dots(3);
    pass &= !ppu.nmi_output;
    printf("TEST nmi_disable_gates_output: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* Check the sprite clock boundary independently of pixel color: reaching X=0
 * must not shift early, and a pending enable uses the old latch for shifting.
 * Include forced blanking and pre-render, where counters clock without shifts. */
int test_sprite_clock_order(void) {
    static const uint8_t positions[8] = {0, 1, 2, 255, 0, 7, 1, 128};
    bool pass = true;
    for (int count = 0; count <= 8; count++) {
        for (int flags = 0; flags < 16; flags++) {
            bool active = (flags & 1) != 0;
            bool pending = (flags & 2) != 0;
            bool rendering = (flags & 4) != 0;
            bool prerender = (flags & 8) != 0;
            test_ppu_init();
            ppu.scanline = prerender ? ppu.prerender_line : 5;
            ppu.dot = 10;
            ppu.mask = rendering ? MASK_BG_ENABLE : 0;
            ppu.sprites_on_line = count;
            ppu.sprite_counters_active = active;
            ppu.sprite_counters_pending = pending;
            memcpy(ppu.sprite_positions, positions, sizeof(positions));
            memset(ppu.sprite_patterns_lo, 0xA5, 8);
            memset(ppu.sprite_patterns_hi, 0xC3, 8);
            ppu_step(&ppu);
            bool counting = false;
            for (int i = 0; i < 8; i++) {
                bool clock = i < count && (active || pending);
                bool shift = i < count && rendering && !prerender &&
                             (!active || positions[i] == 0);
                uint8_t expected_x = positions[i] - (clock && positions[i] != 0);
                pass &= ppu.sprite_positions[i] == expected_x;
                pass &= ppu.sprite_patterns_lo[i] == (shift ? 0x4A : 0xA5);
                pass &= ppu.sprite_patterns_hi[i] == (shift ? 0x86 : 0xC3);
                counting |= clock && positions[i] != 0;
            }
            pass &= ppu.sprite_counters_active == counting;
            pass &= !ppu.sprite_counters_pending;
        }
    }
    test_ppu_init();
    ppu.scanline = ppu.prerender_line;
    ppu.dot = 340;
    ppu.sprite_counters_pending = true;
    ppu.sprites_on_line = 1;
    ppu.sprite_positions[0] = 1;
    ppu.sprite_patterns_lo[0] = 0x80;
    ppu_step(&ppu);
    pass &= ppu.sprite_counters_active && !ppu.sprite_counters_pending;
    pass &= ppu.sprite_positions[0] == 1 && ppu.sprite_patterns_lo[0] == 0x80;
    printf("TEST sprite_clock_order: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static int test_nametable_mirroring_all_addresses(void) {
    static const unsigned banks[5][4] = {
        {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 3}
    };
    bool pass = true;
    for (unsigned mode = 0; mode < 5; mode++) {
        test_ppu_init();
        ppu.mirroring = mode;
        for (unsigned addr = 0x2000; addr < 0x3F00; addr++) {
            unsigned offset = (addr - 0x2000) % 4096;
            unsigned physical = 0x2000 + banks[mode][offset / 1024] * 1024 + offset % 1024;
            ppu.vram[physical] = 0xA5;
            pass &= ppu_read(&ppu, addr) == 0xA5;
            ppu_write(&ppu, addr, 0x5A);
            pass &= ppu.vram[physical] == 0x5A;
            ppu.vram[physical] = 0;
        }
    }
    printf("TEST nametable_mirroring_all_addresses: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

static int test_pixel_palette_priority(void) {
    bool pass = true;
    test_ppu_init();
    ppu.scanline = 0;
    ppu.sprites_on_line = 1;
    ppu.sprite_zero_being_rendered = true;
    for (unsigned bg = 0; bg < 4; bg++)
    for (unsigned sp = 0; sp < 4; sp++)
    for (unsigned bgpal = 0; bgpal < 4; bgpal++)
    for (unsigned sppal = 0; sppal < 4; sppal++)
    for (unsigned flags = 0; flags < 32; flags++) {
        bool behind = flags & 1;
        bool grey = flags & 2;
        unsigned emphasis = flags >> 2;
        ppu.dot = 11;
        ppu.mask = MASK_BG_ENABLE | MASK_SPRITE_ENABLE | (grey ? MASK_GREYSCALE : 0) | (emphasis << 5);
        ppu.bg_shift_pattern_lo = (bg & 1) ? 0x8000 : 0;
        ppu.bg_shift_pattern_hi = (bg & 2) ? 0x8000 : 0;
        ppu.bg_shift_attrib_lo = (bgpal & 1) ? 0x8000 : 0;
        ppu.bg_shift_attrib_hi = (bgpal & 2) ? 0x8000 : 0;
        ppu.sprite_patterns_lo[0] = (sp & 1) ? 0x80 : 0;
        ppu.sprite_patterns_hi[0] = (sp & 2) ? 0x80 : 0;
        ppu.sprite_attributes[0] = sppal | (behind ? SPRITE_PRIORITY : 0);
        /* Distinct high-bit palette values also test the six-bit output mask. */
        for (unsigned i = 0; i < 32; i++) ppu.palette[i] = 0xC0 | (i + 17);
        unsigned address = 0;
        if (bg) address = bgpal * 4 + bg;
        if (sp && (!bg || !behind)) address = 16 + sppal * 4 + sp;
        unsigned color = (address + 17) & (grey ? 0x30 : 0x3F);
        for (unsigned edge = 0; edge < 2; edge++) {
            ppu.dot = edge ? 256 : 11;
            ppu.sprite_flags_pending = 0;
            ppu_render_pixel(&ppu);
            unsigned x = ppu.dot - 1;
            pass &= ppu.index_framebuffer[x] == (color | (emphasis << 6));
            pass &= memcmp(&ppu.framebuffer[x * 3], ppu_palette_2c02[color], 3) == 0;
            pass &= !!(ppu.sprite_flags_pending & STATUS_SPRITE_ZERO) == (bg && sp && !edge);
        }
    }
    printf("TEST pixel_palette_priority: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

static bool row_is(int y, unsigned entry) {
    for (int x = 0; x < PPU_WIDTH; x++) {
        int i = y * PPU_WIDTH + x;
        if (ppu.index_framebuffer[i] != entry ||
            memcmp(&ppu.framebuffer[i * 3], ppu_palette_2c02[entry & 0x3F], 3) != 0)
            return false;
    }
    return true;
}

static int test_forced_blank_backdrop(void) {
    test_ppu_init();
    memset(ppu.framebuffer, 0xFF, sizeof(ppu.framebuffer));
    memset(ppu.index_framebuffer, 0xFF, sizeof(ppu.index_framebuffer));
    ppu.mask = 0;
    ppu.palette[0] = 0x0F;
    ppu.palette[5] = 0x21;
    /* Outside palette space the backdrop is $3F00... */
    ppu.v = 0x2000;
    run_to(1, 0);
    bool pass = row_is(0, 0x0F);
    /* ...inside it, the entry v points at. */
    ppu.v = 0x3F05;
    run_to(2, 0);
    pass &= row_is(1, 0x21);
    /* $3F14 mirrors $3F04, through greyscale and with emphasis, the same
     * entry the frontend takes for the border. */
    ppu.palette[4] = 0x2A;
    ppu.mask = MASK_GREYSCALE | 0xA0;
    ppu.v = 0x3F14;
    run_to(3, 0);
    pass &= row_is(2, 0x20 | 0x140) && ppu_backdrop_entry(&ppu) == (0x20 | 0x140);
    printf("TEST forced_blank_backdrop: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main(void) {
    printf("=== NES PPU Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++; passed += test_ppuctrl_write();
    total++; passed += test_ppuscroll_write();
    total++; passed += test_ppuaddr_write();
    total++; passed += test_ppustatus_read_clears_vblank();
    total++; passed += test_ppudata_write_increment();
    total++; passed += test_vblank_timing();
    total++; passed += test_vblank_clear_on_prerender();
    total++; passed += test_nmi_output();
    total++; passed += test_frame_complete();
    total++; passed += test_oam_write();
    total++; passed += test_palette_mirror();
    total++; passed += test_coarse_x_increment();
    total++; passed += test_fine_y_increment();
    total++; passed += test_rendering_basic();
    total++; passed += test_hybrid_fetch_address();
    total++; passed += test_ppudata_read_sequencer();
    total++; passed += test_secondary_oam_restart();
    total++; passed += test_oam_output_latch();
    total++; passed += test_nmi_disable_gates_output();
    total++; passed += test_sprite_clock_order();
    total++; passed += test_nametable_mirroring_all_addresses();
    total++; passed += test_pixel_palette_priority();
    total++; passed += test_forced_blank_backdrop();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
