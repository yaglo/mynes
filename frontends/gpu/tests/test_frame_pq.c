/* HDR recording conversion: the ST 2084 curve at its published points, the
 * BT.709 to BT.2020 matrix, the table lookup against the exact curve, every
 * half-float input through a whole-frame conversion to limited-range
 * Y'CbCr, the frame light statistics and the threaded bands. Prints the
 * time of one 3840x2880 conversion. */
#define _POSIX_C_SOURCE 200809L
#include "frame_pq.h"
#include "gpu_half.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"frame_pq FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

static uint16_t half_bits(float v) {
    /* Exact for the values used here: small integers, halves and quarters. */
    for (unsigned i = 0; i < 0x7c00; i++)
        if (gpu_half_to_float((uint16_t)i) == fabsf(v)) return (uint16_t)(i | (v < 0 ? 0x8000 : 0));
    return 0;
}

/* BT.2020 non-constant-luminance Y'CbCr of a PQ R'G'B' triple as BT.2100
 * limited-range 16-bit codes, in double precision: Y' 4096..60160, Cb and
 * Cr 4096..61440 around 32768. */
static void ycbcr(const double e[3], double out[3]) {
    double y = 0.2627 * e[0] + 0.6780 * e[1] + 0.0593 * e[2];
    out[0] = 4096 + 56064 * y;
    out[1] = 32768 + 57344 * (e[2] - y) / 1.8814;
    out[2] = 32768 + 57344 * (e[0] - y) / 1.4746;
}

static double luma_code(double nits) {
    double e[3] = { frame_pq_encode(nits), frame_pq_encode(nits), frame_pq_encode(nits) }, out[3];
    ycbcr(e, out);
    return out[0];
}

/* Within one 16-bit code: the matrix rows sum to 1 only within float
 * rounding, and the lookup sits 0.06 of a code from the exact curve. */
static bool near(uint16_t got, double want) { return fabs(got - want) <= 1; }

/* Sample i of plane p (0 Y', 1 Cb, 2 Cr) in a frame of n pixels. */
static uint16_t sample(const uint16_t *yuv, size_t n, int p, size_t i) { return yuv[(size_t)p * n + i]; }

static bool is_grey(const uint16_t *yuv, size_t n, size_t i, double luma) {
    return near(sample(yuv, n, 0, i), luma) && sample(yuv, n, 1, i) == 32768 && sample(yuv, n, 2, i) == 32768;
}

static void test_curve(void) {
    /* BT.2100 / BT.2408: SDR reference white at 203 nits is PQ 0.5806. */
    CHECK(fabs(frame_pq_encode(203) - 0.58069) < 1e-5);
    CHECK(fabs(frame_pq_encode(100) - 0.50808) < 1e-5);
    CHECK(fabs(frame_pq_encode(1000) - 0.75183) < 1e-5);
    CHECK(fabs(frame_pq_encode(10000) - 1) < 1e-12);
    CHECK(frame_pq_encode(0) < 1e-6);
    CHECK(frame_pq_encode(-5) == frame_pq_encode(0));
    CHECK(frame_pq_encode(20000) == frame_pq_encode(10000));
}

static void test_matrix(void) {
    float red[3] = { 1, 0, 0 }, white[3] = { 1, 1, 1 }, out[3];
    frame_pq_bt2020(red, out);
    CHECK(fabsf(out[0] - 0.6274f) < 5e-5f && fabsf(out[1] - 0.0691f) < 5e-5f && fabsf(out[2] - 0.0164f) < 5e-5f);
    frame_pq_bt2020(white, out);
    CHECK(fabsf(out[0] - 1) < 1e-6f && fabsf(out[1] - 1) < 1e-6f && fabsf(out[2] - 1) < 1e-6f);
    /* The limited-range codes at 10 bits (16-bit / 64): white 940, BT.2020
     * blue at the peak Y' 116, Cb 960, Cr 476. */
    const double peak_white[3] = { 1, 1, 1 }, blue[3] = { 0, 0, 1 };
    double c[3];
    ycbcr(peak_white, c);
    CHECK(c[0] == 60160 && c[1] == 32768 && c[2] == 32768);
    ycbcr(blue, c);
    CHECK(lround(c[0] / 64) == 116 && c[1] == 61440 && lround(c[2] / 64) == 476);
}

/* The lookup against the exact curve from 1e-9 nits to the 10000 peak. */
static void test_lookup(const FramePQ *pq) {
    double worst = 0, worst_nits = 0;
    for (int i = 0; i <= 2000000; i++) {
        double nits = i <= 1000000 ? pow(10, -9 + 13.0 * i / 1000000) : 10000.0 * (i - 1000000) / 1000000;
        double err = fabs(frame_pq_lookup(pq, (float)nits) - frame_pq_encode((float)nits));
        if (err > worst) { worst = err; worst_nits = nits; }
    }
    fprintf(stderr, "frame_pq: largest lookup error %.2e (%.3f of a 10-bit code, %.3f of a 16-bit code) at %.3g nits\n",
            worst, worst * 1023, worst * 65535, worst_nits);
    CHECK(worst < 1.0 / 1023);
    CHECK(worst < 1.0 / 65535);   /* finer than a 16-bit code */
    CHECK(frame_pq_lookup(pq, 0) < 1e-5f && frame_pq_lookup(pq, -1) == frame_pq_lookup(pq, 0));
    CHECK(frame_pq_lookup(pq, 10000) == 1 && frame_pq_lookup(pq, 1e9f) == 1);
}

/* Every half value as a grey pixel: grey stays grey through the matrix, so
 * Y' is PQ(value * 203), or black for negative and non-finite, and Cb and
 * Cr are 32768. */
static void test_every_half(const FramePQ *pq) {
    enum { N = 65536 };
    uint16_t *px = malloc(N * 4 * sizeof(uint16_t)), *out = malloc(N * 3 * sizeof(uint16_t));
    if (!px || !out) { CHECK(false); free(px); free(out); return; }
    for (unsigned i = 0; i < N; i++) {
        px[i*4+0] = px[i*4+1] = px[i*4+2] = (uint16_t)i;
        px[i*4+3] = 0x3c00;
    }
    FrameCaptureImage image = { .pixels = px, .width = 256, .height = 256, .hdr = true, .white_level = 1 };
    FramePQLight light;
    CHECK(frame_pq_convert(pq, &image, out, &light));
    double worst = 0;
    bool neutral = true;
    for (unsigned i = 0; i < N; i++) {
        float v = gpu_half_to_float((uint16_t)i);
        double expected = luma_code(isfinite(v) && v > 0 ? v * 203.0 : 0);
        worst = fmax(worst, fabs(sample(out, N, 0, i) - expected));
        neutral = neutral && sample(out, N, 1, i) == 32768 && sample(out, N, 2, i) == 32768;
    }
    CHECK(worst <= 1 && neutral);
    /* 0 and 1.0: 4096 + 56064 x 0.580689 = 36651.74. */
    CHECK(out[0] == 4096 && out[0x3c00] == 36652);
    CHECK(fabs(light.max_nits - 10000) < 1e-6);   /* 65504 * 203 clamps to the peak */
    free(px); free(out);
}

static void test_frame(const FramePQ *pq) {
    /* white, 4x white, a 50x white highlight past the peak, negative red,
     * a green outside BT.709, black; then the same on a 2.0 white level. */
    const float rgb[6][3] = { { 1, 1, 1 }, { 4, 4, 4 }, { 50, 50, 50 },
                              { -1, 0, 0 }, { -0.25f, 1, -0.0625f }, { 0, 0, 0 } };
    uint16_t px[6 * 4];
    for (int i = 0; i < 6; i++) {
        for (int c = 0; c < 3; c++) px[i*4+c] = half_bits(rgb[i][c]);
        px[i*4+3] = 0x3c00;
    }
    CHECK(px[4*4+0] == 0xb400 && px[4*4+2] == 0xac00);   /* -0.25, -0.0625 */
    FrameCaptureImage image = { .pixels = px, .width = 3, .height = 2, .hdr = true, .white_level = 1 };
    uint16_t out[6 * 3];
    FramePQLight light;
    CHECK(frame_pq_convert(pq, &image, out, &light));
    CHECK(fabs(luma_code(203) - 36651.74) < 0.01);
    CHECK(is_grey(out, 6, 0, luma_code(203)) && is_grey(out, 6, 1, luma_code(812)));
    CHECK(out[2] == 60160 && is_grey(out, 6, 2, 60160));   /* the 10000-nit peak */
    CHECK(out[3] == 4096 && is_grey(out, 6, 3, 4096));
    /* Outside BT.709 but inside BT.2020: every component stays positive. */
    float in[3] = { -0.25f, 1, -0.0625f }, wide[3];
    frame_pq_bt2020(in, wide);
    CHECK(wide[0] > 0 && wide[1] > 0 && wide[2] > 0);
    float wide_max = fmaxf(wide[0], fmaxf(wide[1], wide[2]));
    double e[3], want[3];
    for (int c = 0; c < 3; c++) e[c] = frame_pq_encode(wide[c] * 203.0);
    ycbcr(e, want);
    for (int c = 0; c < 3; c++) CHECK(near(sample(out, 6, c, 4), want[c]));
    CHECK(want[1] < 32768 - 1000 && want[2] < 32768 - 1000);   /* a green, not a grey */
    CHECK(out[5] == 4096 && is_grey(out, 6, 5, 4096));
    /* Largest component per pixel: 203, 812, 10000, 0, 203 * wide_max, 0. */
    CHECK(fabs(light.max_nits - 10000) < 1e-6);
    double mean = (203 + 812 + 10000 + 203.0 * wide_max) / 6;
    CHECK(fabs(light.mean_nits - mean) < 1e-3);
    /* The stored values are divided by the image's white level. */
    image.white_level = 2;
    CHECK(frame_pq_convert(pq, &image, out, &light));
    CHECK(is_grey(out, 6, 0, luma_code(101.5)) && is_grey(out, 6, 1, luma_code(406)));
    CHECK(fabs(light.max_nits - 25 * 203) < 1e-3);
    /* Unusable input. */
    image.hdr = false;
    CHECK(!frame_pq_convert(pq, &image, out, &light));
    image.hdr = true; image.pixels = NULL;
    CHECK(!frame_pq_convert(pq, &image, out, &light));
    CHECK(frame_pq_create(0) == NULL && frame_pq_create(-203) == NULL);
    CHECK(frame_pq_create(10001) == NULL && frame_pq_create(NAN) == NULL);
}

static void time_large_frame(const FramePQ *pq) {
    enum { W = 3840, H = 2880 };
    size_t n = (size_t)W * H;
    uint16_t *px = malloc(n * 8), *out = malloc(n * 6);
    if (!px || !out) { free(px); free(out); return; }
    /* A ramp through the render's range, 0 to 3.75 in quarters, with every
     * third row darker and every third column magenta like a grille. */
    uint16_t steps[16];
    for (int s = 0; s < 16; s++) steps[s] = half_bits(s * 0.25f);
    for (size_t i = 0; i < n; i++) {
        int s = (int)(i % W * 16 / W);
        uint16_t h = steps[(i / W) % 3 ? s : s / 4];
        px[i*4+0] = h; px[i*4+1] = (uint16_t)(i % 3 ? h : 0); px[i*4+2] = h; px[i*4+3] = 0x3c00;
    }
    FrameCaptureImage image = { .pixels = px, .width = W, .height = H, .hdr = true, .white_level = 1 };
    FramePQLight light;
    double best = 1e9;
    for (int r = 0; r < 5; r++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        CHECK(frame_pq_convert(pq, &image, out, &light));
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        if (ms < best) best = ms;
    }
    fprintf(stderr, "frame_pq: 3840x2880 conversion %.1f ms (best of 5)\n", best);
    CHECK(fabs(light.max_nits - 3.75 * 203) < 1e-3);
    /* The threaded bands cover every row once: each row converted on its
     * own (one thread) gives the same codes in each plane and the same
     * light. */
    uint16_t *row = malloc((size_t)W * 6);
    double max_nits = 0, sum = 0;
    bool same = row != NULL;
    for (int y = 0; y < H && same; y++) {
        FrameCaptureImage one = { .pixels = px + (size_t)y * W * 4, .width = W, .height = 1, .hdr = true, .white_level = 1 };
        FramePQLight row_light;
        same = frame_pq_convert(pq, &one, row, &row_light);
        for (int p = 0; p < 3 && same; p++)
            same = !memcmp(row + (size_t)p * W, out + (size_t)p * n + (size_t)y * W, (size_t)W * 2);
        max_nits = fmax(max_nits, row_light.max_nits);
        sum += row_light.mean_nits;
    }
    CHECK(same);
    CHECK(max_nits == light.max_nits && fabs(sum / H - light.mean_nits) < 1e-6);
    free(row); free(px); free(out);
}

int main(void) {
    FramePQ *pq = frame_pq_create(203);
    CHECK(pq != NULL);
    if (!pq) return 1;
    test_curve();
    test_matrix();
    test_lookup(pq);
    test_every_half(pq);
    test_frame(pq);
    time_large_frame(pq);
    frame_pq_destroy(pq);
    printf("PQ conversion regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
