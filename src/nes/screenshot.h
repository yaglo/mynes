/*
 * Screenshot Utility
 *
 * Save PPU framebuffer as PPM image. Supports end-of-frame and mid-frame capture.
 */

#ifndef NES_SCREENSHOT_H
#define NES_SCREENSHOT_H

#include <stdio.h>
#include <stdint.h>

struct PPU;

/* Save the current framebuffer as a PPM (P6) image.
 * Returns true on success. */
static inline int screenshot_save_ppm(const struct PPU *ppu, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return 0;
    fprintf(fp, "P6\n%d %d\n255\n", 256, 240);
    fwrite(ppu->framebuffer, 1, 256 * 240 * 3, fp);
    fclose(fp);
    return 1;
}

#endif /* NES_SCREENSHOT_H */
