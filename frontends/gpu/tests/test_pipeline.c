/*
 * test_pipeline.c -- Standalone GPU video pipeline integration test
 * ==================================================================
 *
 * Initializes SDL3 GPU (headless, no window), creates a known composite
 * waveform, runs the video GPU pipeline stage by stage, downloads
 * intermediate buffers, prints statistics, and writes PPM files.
 *
 * Build:
 *   cd build && cmake -DNES_BUILD_GPU_FRONTEND=ON .. && cmake --build . --target test_pipeline
 *
 * Run:
 *   ./bin/test_pipeline
 *
 * Output:
 *   /tmp/pipeline_input.ppm       — raw composite waveform (greyscale)
 *   /tmp/pipeline_luma.ppm        — after luma FIR
 *   /tmp/pipeline_chroma_i.ppm    — demodulated I channel
 *   /tmp/pipeline_chroma_q.ppm    — demodulated Q channel
 *   /tmp/pipeline_rgb.ppm         — final RGB output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL3/SDL.h>

#include "signal_format.h"
#include "signal_precompute.h"
#include "video_chain.h"
#include "video_gpu.h"
#include "gpu_compute.h"
#include "test_signals.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * Buffer statistics
 * ============================================================================ */

typedef struct {
    float min, max, mean;
    int nan_count, inf_count;
} BufStats;

static BufStats compute_stats(const float *buf, int count) {
    BufStats s = { .min = 1e30f, .max = -1e30f, .mean = 0.0f,
                   .nan_count = 0, .inf_count = 0 };
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        float v = buf[i];
        if (isnan(v)) { s.nan_count++; continue; }
        if (isinf(v)) { s.inf_count++; continue; }
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        sum += (double)v;
    }
    int valid = count - s.nan_count - s.inf_count;
    s.mean = (valid > 0) ? (float)(sum / (double)valid) : 0.0f;
    return s;
}

static void print_stats(const char *label, const BufStats *s) {
    printf("  %-20s  min=%.6f  max=%.6f  mean=%.6f", label, s->min, s->max, s->mean);
    if (s->nan_count > 0) printf("  NaN=%d", s->nan_count);
    if (s->inf_count > 0) printf("  Inf=%d", s->inf_count);
    printf("\n");
}

/* ============================================================================
 * PPM file writing
 * ============================================================================ */

/* Write a single-channel float buffer as a greyscale PPM (values clamped to [0,1]). */
static bool write_ppm_grey(const char *path, const float *buf,
                           int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s for writing\n", path); return false; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        float v = buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char c = (unsigned char)(v * 255.0f + 0.5f);
        unsigned char rgb[3] = { c, c, c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  Wrote %s (%dx%d)\n", path, width, height);
    return true;
}

/* Write a signed float buffer as a false-color PPM.
 * Positive = green, negative = red, zero = black. */
static bool write_ppm_signed(const char *path, const float *buf,
                             int width, int height, float scale) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s for writing\n", path); return false; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        float v = buf[i] * scale;
        unsigned char r = 0, g = 0, b = 0;
        if (v > 0.0f) {
            g = (v > 1.0f) ? 255 : (unsigned char)(v * 255.0f);
        } else {
            r = (v < -1.0f) ? 255 : (unsigned char)(-v * 255.0f);
        }
        unsigned char rgb[3] = { r, g, b };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  Wrote %s (%dx%d, scale=%.1f)\n", path, width, height, scale);
    return true;
}

/* Write interleaved RGB float buffer as PPM (values clamped to [0,1]). */
static bool write_ppm_rgb(const char *path, const float *rgb_buf,
                          int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s for writing\n", path); return false; }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        unsigned char rgb[3];
        for (int c = 0; c < 3; c++) {
            float v = rgb_buf[i * 3 + c];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            rgb[c] = (unsigned char)(v * 255.0f + 0.5f);
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  Wrote %s (%dx%d)\n", path, width, height);
    return true;
}

/* ============================================================================
 * CPU reference: simple composite -> Y/I/Q decode for comparison
 * ============================================================================ */

static void cpu_reference_decode(const float *waveform, int spl, int lines,
                                 float *y_out, float *i_out, float *q_out,
                                 const float *fir_y, int fir_y_n,
                                 const float *fir_c, int fir_c_n,
                                 float demod_dp) {
    int total = spl * lines;
    int half_y = fir_y_n / 2;
    int half_c = fir_c_n / 2;

    /* Luma: FIR lowpass on composite. */
    for (int i = 0; i < total; i++) {
        float sum = 0.0f;
        for (int k = 0; k < fir_y_n; k++) {
            int idx = i + k - half_y;
            /* Clamp to row boundaries. */
            int row = i / spl;
            int row_start = row * spl;
            int row_end = row_start + spl - 1;
            if (idx < row_start) idx = row_start;
            if (idx > row_end) idx = row_end;
            sum += waveform[idx] * fir_y[k];
        }
        y_out[i] = sum;
    }

    /* Chroma demod: multiply by carrier, then FIR lowpass. */
    float *raw_i = (float *)calloc((size_t)total, sizeof(float));
    float *raw_q = (float *)calloc((size_t)total, sizeof(float));
    if (!raw_i || !raw_q) { free(raw_i); free(raw_q); return; }

    for (int i = 0; i < total; i++) {
        float phase = demod_dp * (float)i;
        raw_i[i] = waveform[i] * cosf(phase);
        raw_q[i] = waveform[i] * sinf(phase);
    }

    /* FIR lowpass on I and Q. */
    for (int i = 0; i < total; i++) {
        float si = 0.0f, sq = 0.0f;
        for (int k = 0; k < fir_c_n; k++) {
            int idx = i + k - half_c;
            int row = i / spl;
            int row_start = row * spl;
            int row_end = row_start + spl - 1;
            if (idx < row_start) idx = row_start;
            if (idx > row_end) idx = row_end;
            si += raw_i[idx] * fir_c[k];
            sq += raw_q[idx] * fir_c[k];
        }
        i_out[i] = si;
        q_out[i] = sq;
    }

    free(raw_i);
    free(raw_q);
}

/* ============================================================================
 * Comparison: GPU vs CPU
 * ============================================================================ */

static void compare_buffers(const char *label, const float *gpu_buf,
                            const float *cpu_buf, int count) {
    double max_diff = 0.0;
    double sum_diff = 0.0;
    int mismatches_over_01 = 0;
    int mismatches_over_001 = 0;

    for (int i = 0; i < count; i++) {
        double d = fabs((double)gpu_buf[i] - (double)cpu_buf[i]);
        if (d > max_diff) max_diff = d;
        sum_diff += d;
        if (d > 0.1) mismatches_over_01++;
        if (d > 0.01) mismatches_over_001++;
    }

    double mean_diff = sum_diff / (double)count;
    printf("  %-20s  max_diff=%.6f  mean_diff=%.6f  |d|>0.01: %d  |d|>0.1: %d\n",
           label, max_diff, mean_diff, mismatches_over_001, mismatches_over_01);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("=== GPU Pipeline Test ===\n\n");

    /* ---- 1. SDL init (no window) ---- */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("[1] SDL initialized (headless)\n");

    /* ---- 2. Create GPU device ---- */
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL,
        true,   /* debug mode */
        NULL    /* no specific device preference */
    );
    if (!gpu) {
        fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    printf("[2] GPU device created (driver: %s)\n",
           SDL_GetGPUDeviceDriver(gpu));

    /* ---- 3. Signal precompute (NTSC) ---- */
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    printf("[3] Signal precompute: spl=%d, fir_y=%d taps, fir_c=%d taps\n",
           sp.samples_per_line, sp.fir_y_n, sp.fir_c_n);

    /* ---- 4. Video chain config (composite connection) ---- */
    VideoChain vchain;
    video_chain_init_preset(&vchain, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE,
                            SIGNAL_REGION_NTSC);
    printf("[4] Video chain: composite, NTSC, spl=%d, lines=%d\n",
           vchain.signal_fmt.samples_per_line, vchain.signal_fmt.lines);

    /* ---- 5. Init GPU video chain ---- */
    /* Use path relative to the binary's expected shader location. */
    const char *shader_dir = "shaders/compute";
    VideoGPUChain vgc;
    if (!video_gpu_init(&vgc, gpu, &vchain, shader_dir,
                        sp.fir_y, sp.fir_y_n,
                        sp.fir_c, sp.fir_c_n,
                        sp.fir_q, sp.fir_q_n)) {
        fprintf(stderr, "video_gpu_init failed\n");
        SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
        return 1;
    }

    /* Set color matrix from precompute. */
    video_gpu_set_color_matrix(&vgc, sp.color_matrix, sp.color_bias);

    /* Set demod parameters. */
    float demod_dp = 2.0f * (float)M_PI / 12.0f;
    video_gpu_set_demod(&vgc, 0.0f, demod_dp);

    printf("[5] GPU video chain initialized (%d stages)\n",
           chain_get_num_stages(&vgc.sig_chain));

    /* ---- 6. Generate test waveform ---- */
    int spl = vchain.signal_fmt.samples_per_line;
    int lines = vchain.signal_fmt.lines;
    int total_samples = spl * lines;
    float *waveform = (float *)calloc((size_t)total_samples, sizeof(float));
    if (!waveform) { fprintf(stderr, "malloc failed\n"); return 1; }

    float sc_dp = 2.0f * (float)M_PI / 12.0f;  /* subcarrier phase per sample */
    test_signal_color_bars(waveform, spl, lines, sc_dp);
    printf("[6] Test waveform generated: color bars, %d x %d samples\n",
           spl, lines);

    /* Input stats. */
    BufStats input_stats = compute_stats(waveform, total_samples);
    print_stats("Input waveform", &input_stats);

    /* ---- 7. Run GPU pipeline ---- */
    printf("\n[7] Running GPU pipeline...\n");

    /* Allocate output buffer. */
    float *rgb_out = (float *)calloc((size_t)total_samples * 3, sizeof(float));
    if (!rgb_out) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Process the full pipeline. */
    if (!video_gpu_process(&vgc, gpu, waveform, rgb_out)) {
        fprintf(stderr, "video_gpu_process failed\n");
        /* Continue to report what we can. */
    } else {
        printf("  GPU pipeline completed successfully.\n");
    }

    /* ---- 8. Download intermediate buffers ---- */
    printf("\n[8] Downloading intermediate buffers...\n");

    /* Allocate intermediate download buffers. */
    float *luma_buf = (float *)calloc((size_t)total_samples, sizeof(float));
    float *chroma_i_buf = (float *)calloc((size_t)total_samples, sizeof(float));
    float *chroma_q_buf = (float *)calloc((size_t)total_samples, sizeof(float));
    float *composite_buf = (float *)calloc((size_t)total_samples, sizeof(float));

    if (!luma_buf || !chroma_i_buf || !chroma_q_buf || !composite_buf) {
        fprintf(stderr, "malloc failed for intermediate buffers\n");
        goto cleanup;
    }

    /* Download composite (after RC stages, before luma FIR) from buf[0].
     * After chain_run, buf[0] still has the RC-filtered composite.
     * The luma FIR output is in buf[current_buf]. */
    {
        SignalChain *sc = &vgc.sig_chain;
        Uint32 bytes = (Uint32)(total_samples * sizeof(float));

        /* buf[0] = composite (post-RC). */
        if (gpu_buffer_download(gpu, sc->buf[0], composite_buf, bytes)) {
            BufStats s = compute_stats(composite_buf, total_samples);
            print_stats("Composite (post-RC)", &s);
        } else {
            printf("  WARNING: Failed to download composite buffer\n");
        }

        /* buf[current_buf] = luma (post-FIR). */
        if (gpu_buffer_download(gpu, sc->buf[sc->current_buf], luma_buf, bytes)) {
            BufStats s = compute_stats(luma_buf, total_samples);
            print_stats("Luma Y (post-FIR)", &s);
        } else {
            printf("  WARNING: Failed to download luma buffer\n");
        }

        /* aux[2] = filtered I (post-FIR). */
        if (gpu_buffer_download(gpu, sc->aux[2], chroma_i_buf, bytes)) {
            BufStats s = compute_stats(chroma_i_buf, total_samples);
            print_stats("Chroma I (post-FIR)", &s);
        } else {
            printf("  WARNING: Failed to download I buffer\n");
        }

        /* aux[3] = filtered Q (post-FIR). */
        if (gpu_buffer_download(gpu, sc->aux[3], chroma_q_buf, bytes)) {
            BufStats s = compute_stats(chroma_q_buf, total_samples);
            print_stats("Chroma Q (post-FIR)", &s);
        } else {
            printf("  WARNING: Failed to download Q buffer\n");
        }
    }

    /* RGB output stats. */
    {
        int rgb_count = total_samples * 3;
        BufStats rgb_stats = compute_stats(rgb_out, rgb_count);
        print_stats("RGB output", &rgb_stats);

        /* Per-channel stats. */
        float *r_buf = (float *)calloc((size_t)total_samples, sizeof(float));
        float *g_buf = (float *)calloc((size_t)total_samples, sizeof(float));
        float *b_buf = (float *)calloc((size_t)total_samples, sizeof(float));
        if (r_buf && g_buf && b_buf) {
            for (int i = 0; i < total_samples; i++) {
                r_buf[i] = rgb_out[i * 3 + 0];
                g_buf[i] = rgb_out[i * 3 + 1];
                b_buf[i] = rgb_out[i * 3 + 2];
            }
            BufStats rs = compute_stats(r_buf, total_samples);
            BufStats gs = compute_stats(g_buf, total_samples);
            BufStats bs = compute_stats(b_buf, total_samples);
            print_stats("  R channel", &rs);
            print_stats("  G channel", &gs);
            print_stats("  B channel", &bs);
        }
        free(r_buf); free(g_buf); free(b_buf);
    }

    /* ---- 9. CPU reference decode ---- */
    printf("\n[9] CPU reference decode...\n");
    {
        float *cpu_y = (float *)calloc((size_t)total_samples, sizeof(float));
        float *cpu_i = (float *)calloc((size_t)total_samples, sizeof(float));
        float *cpu_q = (float *)calloc((size_t)total_samples, sizeof(float));
        if (cpu_y && cpu_i && cpu_q) {
            cpu_reference_decode(waveform, spl, lines,
                                cpu_y, cpu_i, cpu_q,
                                sp.fir_y, sp.fir_y_n,
                                sp.fir_c, sp.fir_c_n,
                                demod_dp);

            BufStats cy = compute_stats(cpu_y, total_samples);
            BufStats ci = compute_stats(cpu_i, total_samples);
            BufStats cq = compute_stats(cpu_q, total_samples);
            print_stats("CPU Y", &cy);
            print_stats("CPU I", &ci);
            print_stats("CPU Q", &cq);

            printf("\n  GPU vs CPU comparison:\n");
            compare_buffers("Y (luma)", luma_buf, cpu_y, total_samples);
            compare_buffers("I (chroma)", chroma_i_buf, cpu_i, total_samples);
            compare_buffers("Q (chroma)", chroma_q_buf, cpu_q, total_samples);
        }
        free(cpu_y); free(cpu_i); free(cpu_q);
    }

    /* ---- 10. Write PPM files ---- */
    printf("\n[10] Writing PPM files...\n");
    write_ppm_grey("/tmp/pipeline_input.ppm", waveform, spl, lines);
    write_ppm_grey("/tmp/pipeline_composite.ppm", composite_buf, spl, lines);
    write_ppm_grey("/tmp/pipeline_luma.ppm", luma_buf, spl, lines);
    write_ppm_signed("/tmp/pipeline_chroma_i.ppm", chroma_i_buf, spl, lines, 4.0f);
    write_ppm_signed("/tmp/pipeline_chroma_q.ppm", chroma_q_buf, spl, lines, 4.0f);
    write_ppm_rgb("/tmp/pipeline_rgb.ppm", rgb_out, spl, lines);

    /* ---- 11. Chain timing report ---- */
    printf("\n[11] Chain timing:\n");
    for (int i = 0; i < chain_get_num_stages(&vgc.sig_chain); i++) {
        const char *name = chain_get_stage_name(&vgc.sig_chain, i);
        bool enabled = chain_get_stage_enabled(&vgc.sig_chain, i);
        double us = chain_get_stage_timing(&vgc.sig_chain, i);
        printf("  [%d] %-20s  %s  %.1f us\n",
               i, name, enabled ? "ON " : "OFF", us);
    }
    printf("  Total: %.1f us\n", chain_get_total_timing(&vgc.sig_chain));

    printf("\n=== DONE ===\n");

cleanup:
    free(waveform);
    free(rgb_out);
    free(luma_buf);
    free(chroma_i_buf);
    free(chroma_q_buf);
    free(composite_buf);

    video_gpu_destroy(&vgc, gpu);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();

    return 0;
}
