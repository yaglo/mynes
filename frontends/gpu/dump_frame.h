/*
 * Frame Dump — write GPU pipeline output as PPM for debugging.
 * Include this header and call dump_frame_ppm() after video_gpu_process().
 */

#ifndef DUMP_FRAME_H
#define DUMP_FRAME_H

#include <stdio.h>
#include <stdint.h>

/* Write float RGB (3 floats per pixel, [0,1]) to a PPM file. */
static inline void dump_frame_ppm(const char *path, const float *rgb,
                                   int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "dump_frame_ppm: can't open %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        float r = rgb[i*3+0], g = rgb[i*3+1], b = rgb[i*3+2];
        if (r < 0) r = 0; if (r > 1) r = 1;
        if (g < 0) g = 0; if (g > 1) g = 1;
        if (b < 0) b = 0; if (b > 1) b = 1;
        uint8_t px[3] = { (uint8_t)(r*255), (uint8_t)(g*255), (uint8_t)(b*255) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("Dumped %dx%d frame to %s\n", width, height, path);
}

#endif /* DUMP_FRAME_H */
