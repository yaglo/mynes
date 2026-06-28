/*
 * NES Headless ROM Runner
 *
 * Simple headless execution — runs a ROM for N frames and saves a screenshot.
 * For test automation, use test_runner instead (supports Blargg, scripting, traces).
 *
 * Usage: run_rom <rom_file> [frames]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes/rom.h"
#include "nes/nes.h"

static NES nes;
static ROM rom;

void save_screenshot(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", PPU_WIDTH, PPU_HEIGHT);
        fwrite(nes.ppu.framebuffer, 1, PPU_WIDTH * PPU_HEIGHT * 3, fp);
        fclose(fp);
        printf("Screenshot saved: %s\n", filename);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <rom_file> [frames]\n", argv[0]);
        printf("  frames: Number of frames to run (default: 300 = 5 sec)\n");
        printf("\nFor test automation, use test_runner instead.\n");
        return 1;
    }

    const char *rom_path = argv[1];
    int num_frames = (argc >= 3) ? atoi(argv[2]) : 300;
    if (num_frames <= 0) num_frames = 300;

    printf("Loading %s...\n", rom_path);

    int err = nes_rom_load(&rom, rom_path);
    if (err != ROM_OK) {
        printf("Error: %s\n", nes_rom_error_str(err));
        return 1;
    }

    nes_rom_print_info(&rom);

    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper,
                    rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size,
                    rom.mirroring);

    /* Auto-detect PAL from ROM header */
    if (rom.tv_system == NES_TV_PAL) {
        nes_set_region(&nes, NES_REGION_PAL);
        printf("Region: PAL (auto-detected)\n");
    }

    nes_reset(&nes);

    printf("Running %d frames (%.1f sec)...\n", num_frames, num_frames / 60.0);
    for (int frame = 0; frame < num_frames; frame++) {
        nes_run_frame(&nes);
    }

    /* Generate output filename */
    char output[256];
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    snprintf(output, sizeof(output), "output_%s.ppm", base);
    char *ext = strstr(output, ".nes");
    if (ext) strcpy(ext, ".ppm");

    save_screenshot(output);

    printf("CPU: PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X P=$%02X\n",
           nes.cpu.PC, nes.cpu.A, nes.cpu.X, nes.cpu.Y, nes.cpu.SP, nes.cpu.P);

    nes_rom_free(&rom);
    return 0;
}
