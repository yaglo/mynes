#include "frame_capture.h"
#include "gpu_half.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct FrameCaptureJob {
    SDL_Thread *thread;
    FrameCaptureImage image;
    char *path;
};

bool frame_capture_write(const FrameCaptureImage *image, const char *path) {
    Uint64 start = SDL_GetTicksNS();
    int w = image->width, h = image->height;
    bool saved = false;
    uint8_t *encoded = malloc(65536), *row = malloc((size_t)w * 3);
    float *decoded = malloc(65536 * sizeof(float));
    float *linear_row = malloc((size_t)w * 3 * sizeof(float));
    char *linear_path = malloc(strlen(path) + sizeof(".linear.pfm"));
    FILE *f = NULL, *linear = NULL;
    if (image->pixels && encoded && decoded && row && linear_row && linear_path) {
        sprintf(linear_path, "%s.linear.pfm", path);
        /* Half values form a finite domain: encode each value once instead
         * of evaluating the transfer function millions of times per image. */
        int n = image->hdr ? 65536 : 256;
        for (int i = 0; i < n; i++) {
            if (image->hdr) {
                float v = gpu_half_to_float((uint16_t)i) / fmaxf(image->white_level, .001f);
                decoded[i] = isfinite(v) ? v : 0;
                v = fminf(fmaxf(decoded[i], 0), 1);
                v = v <= .0031308f ? 12.92f*v : 1.055f*powf(v, 1/2.4f)-.055f;
                encoded[i] = (uint8_t)lrintf(v*255);
            } else {
                float v = i/255.0f;
                encoded[i] = (uint8_t)i;
                decoded[i] = v <= .04045f ? v/12.92f : powf((v+.055f)/1.055f, 2.4f);
            }
        }
        f = fopen(path, "wb");
        linear = fopen(linear_path, "wb");
        if (f && linear) {
            const uint16_t endian = 1;
            const char *scale = *(const uint8_t *)&endian ? "-1.0" : "1.0";
            saved = fprintf(f, "P6\n%d %d\n255\n", w, h) > 0
                 && fprintf(linear, "PF\n%d %d\n%s\n", w, h, scale) > 0;
            /* PPM is top-down and PFM bottom-up; retain the same pixels. */
            for (int y = 0; y < h && saved; y++) {
                for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
                    int channel = image->hdr || !image->bgra ? c : 2-c;
                    size_t i = ((size_t)y*w+x)*4+channel;
                    size_t j = ((size_t)(h-1-y)*w+x)*4+channel;
                    unsigned top = image->hdr ? ((const uint16_t *)image->pixels)[i] : ((const uint8_t *)image->pixels)[i];
                    unsigned bottom = image->hdr ? ((const uint16_t *)image->pixels)[j] : ((const uint8_t *)image->pixels)[j];
                    row[x*3+c] = encoded[top];
                    linear_row[x*3+c] = decoded[bottom];
                }
                saved = fwrite(row, 3, w, f) == (size_t)w
                     && fwrite(linear_row, 3*sizeof(float), w, linear) == (size_t)w;
            }
        }
    }
    if (f && fclose(f)) saved = false;
    if (linear && fclose(linear)) saved = false;
    free(encoded); free(decoded); free(row); free(linear_row); free(linear_path);
    fprintf(stderr, "%s: %s\n", saved ? "Final CRT capture" : "Capture write failed", path);
    fprintf(stderr, "CAPTURE_WRITE ms=%.3f saved=%d\n", (SDL_GetTicksNS()-start)/1e6, saved);
    return saved;
}

static int write_job(void *user) {
    FrameCaptureJob *job = user;
    return frame_capture_write(&job->image, job->path) ? 0 : 1;
}

FrameCaptureJob *frame_capture_start(const FrameCaptureImage *image, const char *path) {
    FrameCaptureJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    size_t bytes = (size_t)image->width * image->height * (image->hdr ? 8 : 4);
    void *pixels = malloc(bytes);
    job->path = SDL_strdup(path);
    if (!pixels || !job->path) { free(pixels); SDL_free(job->path); free(job); return NULL; }
    memcpy(pixels, image->pixels, bytes);
    job->image = *image;
    job->image.pixels = pixels;
    job->thread = SDL_CreateThread(write_job, "CRT capture", job);
    if (!job->thread) { free(pixels); SDL_free(job->path); free(job); return NULL; }
    return job;
}

bool frame_capture_finish(FrameCaptureJob *job) {
    if (!job) return true;
    int status = 1;
    SDL_WaitThread(job->thread, &status);
    free((void *)job->image.pixels);
    SDL_free(job->path);
    free(job);
    return status == 0;
}
