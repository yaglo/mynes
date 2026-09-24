/*
 * CHR Tile Debug Visualization
 *
 * Renders CHR pattern tables (tile data) as a 256x128 RGB image.
 * Left half: pattern table 0 ($0000-$0FFF) — 128x128 (16x16 tiles)
 * Right half: pattern table 1 ($1000-$1FFF) — 128x128 (16x16 tiles)
 *
 * Each tile is 8x8 pixels. Colors use a grayscale palette:
 *   0 = black, 1 = dark gray, 2 = light gray, 3 = white
 */

#ifndef NES_DEBUG_CHR_H
#define NES_DEBUG_CHR_H

#include <stdint.h>
#include <stdio.h>

struct NES;

/* Grayscale palette for 2-bit pixel values */
static const uint8_t chr_debug_palette[4] = {0x00, 0x55, 0xAA, 0xFF};

/*
 * Render both pattern tables to an RGB888 buffer.
 * buf must be at least 256 * 128 * 3 bytes.
 * Reads CHR data via the mapper (side-effect-free for ROM-based CHR).
 */
static inline void debug_chr_render(const struct NES *nes, uint8_t *buf) {
    /* Access PPU/mapper through the NES struct.
     * We read CHR directly from the mapper's CHR ROM/RAM. */
    for (int table = 0; table < 2; table++) {
        uint16_t base = table * 0x1000;
        int x_off = table * 128;

        for (int tile_row = 0; tile_row < 16; tile_row++) {
            for (int tile_col = 0; tile_col < 16; tile_col++) {
                uint16_t tile_addr = base + (tile_row * 16 + tile_col) * 16;

                for (int row = 0; row < 8; row++) {
                    /* Read two bitplanes */
                    uint8_t lo = 0, hi = 0;
                    /* Use mapper for CHR reads if available */
                    if (nes->mapper_loaded && nes->mapper.has_chr_ram) {
                        lo = nes->mapper.chr_ram[tile_addr + row];
                        hi = nes->mapper.chr_ram[tile_addr + row + 8];
                    } else if (nes->mapper_loaded) {
                        lo = nes->mapper.chr_rom ?
                             nes->mapper.chr_rom[(tile_addr + row) % nes->mapper.chr_rom_size] : 0;
                        hi = nes->mapper.chr_rom ?
                             nes->mapper.chr_rom[(tile_addr + row + 8) % nes->mapper.chr_rom_size] : 0;
                    } else if (nes->chr_rom && nes->chr_rom_size > 0) {
                        lo = nes->chr_rom[(tile_addr + row) % nes->chr_rom_size];
                        hi = nes->chr_rom[(tile_addr + row + 8) % nes->chr_rom_size];
                    } else {
                        lo = nes->ppu.vram[tile_addr + row];
                        hi = nes->ppu.vram[tile_addr + row + 8];
                    }

                    for (int col = 0; col < 8; col++) {
                        int bit = 7 - col;
                        uint8_t pixel = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                        uint8_t gray = chr_debug_palette[pixel];

                        int px = x_off + tile_col * 8 + col;
                        int py = tile_row * 8 + row;
                        int idx = (py * 256 + px) * 3;
                        buf[idx + 0] = gray;
                        buf[idx + 1] = gray;
                        buf[idx + 2] = gray;
                    }
                }
            }
        }
    }
}

/* Save CHR debug view as PPM */
static inline int debug_chr_save_ppm(const struct NES *nes, const char *filename) {
    uint8_t buf[256 * 128 * 3];
    debug_chr_render(nes, buf);

    FILE *fp = fopen(filename, "wb");
    if (!fp) return 0;
    fprintf(fp, "P6\n256 128\n255\n");
    fwrite(buf, 1, sizeof(buf), fp);
    fclose(fp);
    return 1;
}

#endif /* NES_DEBUG_CHR_H */
