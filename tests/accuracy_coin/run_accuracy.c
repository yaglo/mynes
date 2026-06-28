/*
 * AccuracyCoin Test Runner
 *
 * Loads AccuracyCoin.nes and runs all tests automatically.
 * Press Start at menu to run all tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes/rom.h"
#include "nes/nes.h"

/* CPU/PPU clock alignment override via NES_ALIGN env var (0, 1, or 2).
 * Default 2 (CPU bus access after 2 PPU dots) maximizes pass rate. */
static uint8_t parse_align_env(void) {
    const char *s = getenv("NES_ALIGN");
    if (!s) return 2;
    int v = atoi(s);
    if (v < 0 || v > 2) return 2;
    return (uint8_t)v;
}

static NES nes;
static ROM rom;

int main(int argc, char *argv[]) {
    const char *rom_path = "tests/accuracy_coin/AccuracyCoin.nes";
    int page = 1;

    if (argc > 1) {
        rom_path = argv[1];
    }
    if (argc > 2) {
        page = atoi(argv[2]);
        if (page < 1) page = 1;
        if (page > 20) page = 20;
    }

    printf("Loading %s...\n", rom_path);
    printf("Target page: %d\n", page);

    int err = nes_rom_load(&rom, rom_path);
    if (err != ROM_OK) {
        printf("Error loading ROM: %s\n", nes_rom_error_str(err));
        return 1;
    }

    nes_rom_print_info(&rom);

    /* Debug: Check CHR ROM content */
    printf("\nCHR ROM first 32 bytes (pattern table):\n");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", rom.chr_rom[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    /* Initialize NES */
    nes_init(&nes);
    /* Allow finer-grained CPU phase offset via NES_CPU_PHASE env var.
     * If unset, fall back to coarse NES_ALIGN (0/1/2). */
    {
        const char *p = getenv("NES_CPU_PHASE");
        if (p) {
            int v = atoi(p);
            if (v < 0) v = 0;
            if (v > 11) v = 11;
            nes_set_cpu_phase_offset(&nes, (uint8_t)v);
            printf("CPU phase offset (master ticks): %d\n", v);
        } else {
            nes_set_cpu_align(&nes, parse_align_env());
        }
    }
    nes_load_prg(&nes, rom.prg_rom, rom.prg_size);
    nes_load_chr(&nes, rom.chr_rom, rom.chr_size);
    nes.ppu.mirroring = rom.mirroring;
    printf("CPU/PPU align: %d\n", nes.cpu_align);

    nes_reset(&nes);

    printf("Running emulation...\n");
    printf("  - Waiting for menu (60 frames)...\n");

    /* Run for 90 frames to let the ROM initialize and show menu */
    for (int frame = 0; frame < 90; frame++) {
        nes_run_frame(&nes);
    }

    /* Helper to press/release a button and wait */
    #define PRESS_BUTTON(btn, frames) do { \
        nes_set_controller(&nes, 0, btn); \
        for (int _f = 0; _f < (frames); _f++) nes_run_frame(&nes); \
        nes_set_controller(&nes, 0, 0); \
        for (int _f = 0; _f < 5; _f++) nes_run_frame(&nes); \
    } while(0)

    /* Navigate to target page */
    if (page > 1 && page <= 10) {
        /* Go RIGHT for pages 2-10 */
        int presses = page - 1;
        printf("  - Navigating to page %d (%d RIGHT presses)...\n", page, presses);
        for (int p = 0; p < presses; p++) {
            PRESS_BUTTON(BTN_RIGHT, 10);
            for (int f = 0; f < 20; f++) nes_run_frame(&nes);
        }
    } else if (page > 10) {
        /* Go LEFT for pages 11-20 (wrap around) */
        int presses = 21 - page;
        printf("  - Navigating to page %d (%d LEFT presses)...\n", page, presses);
        for (int p = 0; p < presses; p++) {
            PRESS_BUTTON(BTN_LEFT, 10);
            for (int f = 0; f < 20; f++) nes_run_frame(&nes);
        }
    }

    /* Debug: Check PPU state */
    printf("\nPPU State after navigation:\n");
    printf("  PPUCTRL: %02X  PPUMASK: %02X\n", nes.ppu.ctrl, nes.ppu.mask);
    printf("  v: %04X  t: %04X\n", nes.ppu.v, nes.ppu.t);
    printf("  Palette: ");
    for (int i = 0; i < 16; i++) printf("%02X ", nes.ppu.palette[i]);
    printf("\n");
    printf("  Nametable $2000 rows 0-7:\n");
    for (int row = 0; row < 8; row++) {
        printf("  Row %d: ", row);
        for (int col = 0; col < 32; col++) {
            uint8_t tile = nes.ppu.vram[0x2000 + row * 32 + col];
            printf("%02X ", tile);
        }
        printf("\n");
    }
    printf("\n");
    printf("  OAM first 32 bytes (8 sprites):\n  ");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", nes.ppu.oam[i]);
        if ((i + 1) % 4 == 0) printf(" ");
    }
    printf("\n");

    /* Save menu screenshot */
    {
        FILE *fp = fopen("accuracy_menu.ppm", "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", PPU_WIDTH, PPU_HEIGHT);
            fwrite(nes.ppu.framebuffer, 1, PPU_WIDTH * PPU_HEIGHT * 3, fp);
            fclose(fp);
            printf("  - Menu saved to accuracy_menu.ppm\n");
        }
    }

    int total_frames = 180;  /* 120 initial + 40 navigation + 10 button press */

    /* Run tests on selected page */
    printf("  - Running tests (page %d)...\n", page);

    /* Page 15 (Controller Clocking / Power On State) requires individual test navigation */
    if (page == 15) {
        /* Page 15 has 5 DRAW items that need individual selection */
        const int num_tests = 5;
        const char *test_names[] = {"PPU RESET FLAG", "CPU RAM", "CPU REGISTERS", "PPU RAM", "PALETTE RAM"};
        for (int t = 0; t < num_tests; t++) {
            printf("  - Running test %d/%d: %s\n", t + 1, num_tests, test_names[t]);
            /* Navigate down to test item */
            for (int d = 0; d <= t; d++) {
                PRESS_BUTTON(BTN_DOWN, 5);
            }
            /* Select and run test */
            PRESS_BUTTON(BTN_A, 10);
            for (int f = 0; f < 60; f++) nes_run_frame(&nes);
            total_frames += 60;
            /* Navigate back up */
            for (int u = 0; u < 10; u++) {
                PRESS_BUTTON(BTN_UP, 5);
            }
        }
    } else {
        PRESS_BUTTON(BTN_A, 10);
    }

    /* Wait for tests to complete - tuned for reasonable speed */
    int wait_frames;
    if (page == 17) {
        wait_frames = 600;  /* VBlank/NMI timing tests need longer wait */
    } else if (page >= 18) {
        wait_frames = 400;  /* Sprite tests */
    } else if (page >= 8 && page <= 14) {
        wait_frames = 300;  /* Interrupt, APU, and DMA tests */
    } else if (page == 15) {
        wait_frames = 60;   /* Page 15 handled above */
    } else if (page == 16) {
        wait_frames = 400;  /* PPU behavior tests may need longer */
    } else {
        wait_frames = 200;  /* Basic CPU tests */
    }
    for (int frame = 0; frame < wait_frames || frame < 2000; frame++) {
        nes_run_frame(&nes);
        total_frames++;

        // Debug: Print progress every 100 frames
        if (frame % 100 == 0) {
            printf("  Frame %d: PC=%04X SP=%02X P=%02X\n", frame, nes.cpu.PC, nes.cpu.SP, nes.cpu.P);
        }
    }

    /* Print results */
    printf("\n=== Page %d Test Results ===\n", page);
    int pass_count = 0, fail_count = 0;
    for (int row = 5; row < 26; row += 2) {
        uint8_t first_tile = nes.ppu.vram[0x2000 + row * 32 + 1];
        if (first_tile == 0x24) continue;

        /* Check result type based on tile at position 1-4 */
        char result[5] = {0};
        for (int i = 0; i < 4; i++) {
            uint8_t t = nes.ppu.vram[0x2000 + row * 32 + 1 + i];
            if (t >= 0x0A && t <= 0x23) result[i] = 'A' + t - 0x0A;
            else if (t <= 0x09) result[i] = '0' + t;
            else result[i] = ' ';
        }

        bool is_pass = (result[0] == 'P' && result[1] == 'A' && result[2] == 'S' && result[3] == 'S');

        /* Check if this is an informational item (not a real test)
         * These start with "PRIN" (PRINT), "DRAW", dots, or other patterns */
        bool is_info = (result[0] == 'P' && result[1] == 'R' && result[2] == 'I' && result[3] == 'N') ||
                       (result[0] == 'D' && result[1] == 'R' && result[2] == 'A' && result[3] == 'W') ||
                       (result[0] == '.' && result[1] == '.' && result[2] == '.' && result[3] == '.');

        if (is_info) {
            printf("  INFO ");
        } else {
            if (is_pass) pass_count++; else fail_count++;
            printf("  %s ", is_pass ? "PASS" : "FAIL");
        }
        for (int col = 7; col < 30; col++) {
            uint8_t tile = nes.ppu.vram[0x2000 + row * 32 + col];
            if (tile <= 0x09) printf("%c", '0' + tile);
            else if (tile >= 0x0A && tile <= 0x23) printf("%c", 'A' + tile - 0x0A);
            else if (tile == 0x24) printf(" ");
            else printf(".");
        }
        printf("\n");
    }
    printf("\n=== Summary: %d PASS, %d FAIL ===\n", pass_count, fail_count);

    #undef PRESS_BUTTON

    printf("\n=== Emulation Complete ===\n");
    printf("Ran %d frames (%d seconds at 60fps)\n", total_frames, total_frames / 60);

    /* Debug: Check final PPU state */
    printf("\nFinal PPU State:\n");
    printf("  PPUCTRL: %02X  PPUMASK: %02X\n", nes.ppu.ctrl, nes.ppu.mask);
    printf("  Nametable $2000 rows 0-29:\n");
    for (int row = 0; row < 30; row++) {
        printf("  Row %2d: ", row);
        for (int col = 0; col < 32; col++) {
            uint8_t tile = nes.ppu.vram[0x2000 + row * 32 + col];
            if (tile <= 0x09) {
                /* Digit 0-9 */
                printf("%c", '0' + tile);
            } else if (tile >= 0x0A && tile <= 0x23) {
                /* Letter A-Z */
                printf("%c", 'A' + tile - 0x0A);
            } else if (tile == 0x24) {
                printf(" ");
            } else {
                printf(".");
            }
        }
        printf("\n");
    }
    /* Dump row 7 raw to see corruption */
    printf("\n  Row 7 raw hex: ");
    for (int col = 0; col < 32; col++) {
        printf("%02X ", nes.ppu.vram[0x2000 + 7 * 32 + col]);
    }
    printf("\n");
    /* Also dump row 18 raw */
    printf("  Row 18 raw hex: ");
    for (int col = 0; col < 32; col++) {
        printf("%02X ", nes.ppu.vram[0x2000 + 18 * 32 + col]);
    }
    printf("\n");
    printf("\n  Active sprites (Y < 240):\n");
    int sprite_count = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t y = nes.ppu.oam[i * 4];
        if (y < 240) {
            printf("    Sprite %2d: Y=%3d X=%3d Tile=%02X Attr=%02X\n",
                   i, y, nes.ppu.oam[i*4+3], nes.ppu.oam[i*4+1], nes.ppu.oam[i*4+2]);
            sprite_count++;
            if (sprite_count >= 16) break;
        }
    }
    if (sprite_count == 0) printf("    (none)\n");

    /* Dump some memory locations that might contain results */
    printf("\nRAM dump (possible test results):\n");
    printf("$0000-$001F: ");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", nes.ram[i]);
    }
    printf("\n");

    printf("$0020-$003F: ");
    for (int i = 0x20; i < 0x40; i++) {
        printf("%02X ", nes.ram[i]);
    }
    printf("\n");

    printf("$0040-$005F: ");
    for (int i = 0x40; i < 0x60; i++) {
        printf("%02X ", nes.ram[i]);
    }
    printf("\n");

    printf("$0060-$007F: ");
    for (int i = 0x60; i < 0x80; i++) {
        printf("%02X ", nes.ram[i]);
    }
    printf("\n");

    printf("$0500-$051F: ");
    for (int i = 0x500; i < 0x520 && i < 0x800; i++) {
        printf("%02X ", nes.ram[i]);
    }
    printf("\n");

    /* Save framebuffer as PPM image */
    const char *output_path = "accuracy_result.ppm";
    FILE *fp = fopen(output_path, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", PPU_WIDTH, PPU_HEIGHT);
        fwrite(nes.ppu.framebuffer, 1, PPU_WIDTH * PPU_HEIGHT * 3, fp);
        fclose(fp);
        printf("\nFramebuffer saved to: %s\n", output_path);
        printf("View with: open %s (macOS) or any image viewer\n", output_path);
    }

    nes_rom_free(&rom);

    return 0;
}
