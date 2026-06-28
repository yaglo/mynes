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
    ppu_reg_write(&ppu, 0x2007, 0x0F);

    /* Read from $3F10 (should mirror $3F00) */
    ppu_reg_write(&ppu, 0x2006, 0x3F);
    ppu_reg_write(&ppu, 0x2006, 0x10);
    ppu_reg_read(&ppu, 0x2007); /* Priming read */
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

/* ============================================================================
 * Main
 * ============================================================================ */

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

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
