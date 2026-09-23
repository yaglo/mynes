/* VHS deck kernels (vhs_tape + vhs_playback) on synthetic NES rasters.
 * Thresholds come from the golden model (analog filters evaluated exactly,
 * real RF) and the DH / JVC / IEC values in gpu-realism-validation.md; each
 * would fail on the earlier filtered-noise stage. The TV line PLL is tested
 * on its own at the end. */
#include <SDL3/SDL.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_gpu.h"

#define FS (12 * 315e6 / 88)
#define N_RASTER (VHS_LINES * VHS_SPL)
#define IRE (788.0 / (1000.0 / 140.0))      /* output chain units to IRE */
#define FFT_N 4096

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL vhs %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* ---- NES 2C02 raster at 12 fsc, line starting at H sync, 6 MHz console pole ---- */
static float nes_level(float v) { return (v - 0.312f) / 0.788f; }
static float nes_wave(int idx, int n) {
    static const float lo[4] = {0.228f, 0.312f, 0.552f, 0.880f}, hi[4] = {0.616f, 0.840f, 1.100f, 1.100f};
    int lum = (idx >> 4) & 3, hue = idx & 15;
    if (hue == 0) return nes_level(hi[lum]);
    if (hue == 13) return nes_level(lo[lum]);
    if (hue >= 14) return 0;
    return ((n + hue) % 12) < 6 ? nes_level(hi[lum]) : nes_level(lo[lum]);
}
static void nes_raster(const uint8_t *picture, float *out) {
    const float sync = -264.0f / 788.0f;
    for (int line = 0; line < VHS_LINES; line++)
        for (int x = 0; x < VHS_SPL; x++) {
            int n = line * VHS_SPL + x, dot = x / 8;
            float v = 0;
            if (dot < 25) v = sync;
            if (dot >= 29 && dot < 44) v = ((n + 8) % 12) < 6 ? 212.0f / 788.0f : -164.0f / 788.0f;
            if (line < 242 && dot >= 49 && dot < 332) v = nes_wave(0x0f, n);
            if (line < 240 && x >= 520 && x < 520 + 2048) v = nes_wave(picture[line * 256 + (x - 520) / 8], n);
            if (line >= 245 && line < 248) v = dot < 318 ? sync : 0;
            out[n] = v;
        }
    double a = exp(-2 * M_PI * 6e6 / FS), y = out[0];
    for (int n = 0; n < N_RASTER; n++) { y = a * y + (1 - a) * out[n]; out[n] = (float)y; }
}
static void picture_fill(uint8_t *p, int code) { memset(p, code, 256 * 240); }
static void picture_bars(uint8_t *p, int a, int b) {
    for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) p[y * 256 + x] = (uint8_t)((x / 32) % 2 ? b : a);
}

/* ---- rig: a signal chain holding only the two deck stages ---- */
typedef struct { SignalChain sc; VHSGpu g; } Rig;
static void static_transport(VHSParams *p) {
    p->bow_scale = 0; p->tbe_varying_ns = 0; p->line_jitter_ns = 0;
    p->skew_ab_ns = p->skew_ba_ns = 0; p->dropout_scale = 0;
}
static void clean_tape(VHSParams *p) { p->rf_cnr_dbhz = 250; p->mod_noise_hz = 0; p->chroma_noise_ire = 0; }
static VHSParams deck_params(void) { VHSParams p = {0}; vhs_params_defaults(&p); p.enabled = 1; return p; }

static bool rig_init(Rig *r, SDL_GPUDevice *gpu, const VHSParams *p) {
    if (!chain_init(&r->sc, gpu, N_RASTER, "shaders/compute")) return false;
    r->sc.samples_per_line = VHS_SPL;
    if (!vhs_gpu_init(&r->g, &r->sc, gpu) || !vhs_gpu_configure(&r->g, &r->sc, gpu, p, FS)) return false;
    vhs_gpu_set_enabled(&r->g, &r->sc, true);
    return true;
}
static void rig_free(Rig *r, SDL_GPUDevice *gpu) { vhs_gpu_destroy(&r->g, gpu); chain_destroy(&r->sc, gpu); }

/* Run one frame; optional replacement table and defects; chroma_gain 0
 * leaves only the deck's luma in the output. Output in IRE. */
static void rig_run(Rig *r, SDL_GPUDevice *gpu, const float *raster, uint32_t frame,
                    const VHSLineEntry *table, const VHSDefect *defects, int defect_count,
                    float chroma_gain, double *out) {
    static float buf[N_RASTER];
    CHECK(chain_upload_input(&r->sc, gpu, raster, N_RASTER * sizeof(float)));
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(vhs_gpu_frame(&r->g, &r->sc, gpu, cmd, frame));
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    if (table) CHECK(gpu_buffer_upload(gpu, r->g.lines, table, VHS_TABLE_LINES * sizeof(VHSLineEntry)));
    if (defects) {
        CHECK(gpu_buffer_upload(gpu, r->g.defects, defects, VHS_MAX_DEFECTS * sizeof(VHSDefect)));
        r->g.deck->gpu.defect_count = (uint32_t)defect_count;
    }
    GpuVHSParams p = r->g.deck->gpu;
    p.playback_acc *= chroma_gain;
    chain_update_params(&r->sc, r->g.stage_tape, &p, sizeof(p));
    chain_update_params(&r->sc, r->g.stage_playback, &p, sizeof(p));
    r->sc.current_buf = 0;
    CHECK(chain_run(&r->sc, gpu));
    CHECK(chain_download_output(&r->sc, gpu, buf, sizeof(buf)));
    for (int n = 0; n < N_RASTER; n++) out[n] = buf[n] * IRE;
}

/* ---- spectral helpers: radix-2 FFT on one zero-padded line ---- */
static void fft(double complex *x, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double complex w = cexp((inverse ? 2 : -2) * M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            double complex u = 1;
            for (int k = 0; k < len / 2; k++, u *= w) {
                double complex a = x[i + k], b = x[i + k + len / 2] * u;
                x[i + k] = a + b; x[i + k + len / 2] = a - b;
            }
        }
    }
    if (inverse) for (int i = 0; i < n; i++) x[i] /= n;
}
/* Band-limit one line to lo <= |f| < hi (Hz) with an ideal filter. */
static void band(const double *line, double lo, double hi, double *out) {
    static double complex x[FFT_N];
    for (int i = 0; i < FFT_N; i++) x[i] = i < VHS_SPL ? line[i] : 0;
    fft(x, FFT_N, false);
    for (int i = 0; i < FFT_N; i++) {
        double f = fabs((i < FFT_N / 2 ? i : i - FFT_N) * FS / FFT_N);
        if (f < lo || f >= hi) x[i] = 0;
    }
    fft(x, FFT_N, true);
    for (int i = 0; i < VHS_SPL; i++) out[i] = creal(x[i]);
}
/* Chroma envelope of one line: product detector on the fixed carrier grid
 * and a 0.6 MHz ideal low-pass. */
static void chroma(const double *line, int line_index, double complex *out) {
    static double complex x[FFT_N];
    for (int i = 0; i < FFT_N; i++)
        x[i] = i < VHS_SPL ? 2 * line[i] * cexp(-2 * M_PI * I * ((line_index * VHS_SPL + i) % 12) / 12.0) : 0;
    fft(x, FFT_N, false);
    for (int i = 0; i < FFT_N; i++)
        if (fabs((i < FFT_N / 2 ? i : i - FFT_N) * FS / FFT_N) >= 0.6e6) x[i] = 0;
    fft(x, FFT_N, true);
    for (int i = 0; i < VHS_SPL; i++) out[i] = x[i];
}
static double corr(const double *a, const double *b, int n) {
    double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) { ma += a[i] / n; mb += b[i] / n; }
    for (int i = 0; i < n; i++) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return sab / sqrt(saa * sbb);
}

/* Region used for noise statistics: lines 20-219, samples 900-2399. */
enum { R_L0 = 20, R_L1 = 220, R_X0 = 900, R_X1 = 2400, R_W = R_X1 - R_X0, R_H = R_L1 - R_L0 };

static void levels(SDL_GPUDevice *gpu) {
    static float raster[N_RASTER]; static double out[N_RASTER]; static uint8_t pic[256 * 240];
    VHSParams p = deck_params(); static_transport(&p); clean_tape(&p);
    Rig r; CHECK(rig_init(&r, gpu, &p));
    picture_fill(pic, 0x10); nes_raster(pic, raster);
    rig_run(&r, gpu, raster, 10, NULL, NULL, 0, 1, out);
    double level = 0, burst = 0;
    for (int l = R_L0; l < R_L1; l++) for (int x = R_X0; x < R_X1; x++) level += out[l * VHS_SPL + x] / (R_W * R_H);
    static double complex z[VHS_SPL];
    for (int l = R_L0; l < R_L1; l++) {
        chroma(out + l * VHS_SPL, l, z);
        double peak = 0;
        for (int x = 150; x < 500; x++) peak = fmax(peak, cabs(z[x]));
        burst += peak / R_H;
    }
    printf("VHS levels: grey %.3f IRE, output burst %.2f IRE p-p\n", level, 2 * burst);
    CHECK(fabs(level - 80.0) < 0.3);         /* NES $10 at sync-referenced 119.4 IRE/unit */
    CHECK(fabs(2 * burst - 40) < 2);
    rig_free(&r, gpu);
}

/* Noise of `noisy` against a clean run of the same rig configuration. */
typedef struct { double rms24, rms29, share05, share23, acf_half_ns, acf_zero_ns, r_line, r_frame, column_cv, lattice; } LumaNoise;
static LumaNoise luma_noise(SDL_GPUDevice *gpu, int code, void (*edit)(VHSParams *)) {
    static float raster[N_RASTER]; static double clean[N_RASTER], out[3][N_RASTER]; static uint8_t pic[256 * 240];
    static double diff[3][R_H][R_W], line_band[VHS_SPL], column_var[R_W];
    VHSParams p = deck_params(); static_transport(&p); if (edit) edit(&p);
    VHSParams c = p; clean_tape(&c);
    picture_fill(pic, code); nes_raster(pic, raster);
    Rig rc, rn; CHECK(rig_init(&rc, gpu, &c)); CHECK(rig_init(&rn, gpu, &p));
    rig_run(&rc, gpu, raster, 10, NULL, NULL, 0, 0, clean);
    for (int f = 0; f < 3; f++) rig_run(&rn, gpu, raster, 10 + f, NULL, NULL, 0, 0, out[f]);
    LumaNoise m = {0};
    double e24 = 0, e29 = 0, p_total = 0, p05 = 0, p23 = 0;
    static double acf[VHS_SPL];
    memset(acf, 0, sizeof(acf)); memset(column_var, 0, sizeof(column_var));
    static double complex x[FFT_N];
    for (int f = 0; f < 3; f++)
        for (int l = 0; l < R_H; l++) {
            double row[VHS_SPL];
            for (int i = 0; i < VHS_SPL; i++) row[i] = out[f][(l + R_L0) * VHS_SPL + i] - clean[(l + R_L0) * VHS_SPL + i];
            band(row, 0, 2.4e6, line_band);
            for (int i = R_X0; i < R_X1; i++) e24 += line_band[i] * line_band[i];
            band(row, 0, 2.9e6, line_band);
            for (int i = R_X0; i < R_X1; i++) { e29 += line_band[i] * line_band[i]; diff[f][l][i - R_X0] = line_band[i]; }
            if (f) continue;
            /* Power spectrum (Hann) and autocorrelation of this line. */
            for (int i = 0; i < FFT_N; i++) x[i] = i < R_W ? line_band[R_X0 + i] * (0.5 - 0.5 * cos(2 * M_PI * i / R_W)) : 0;
            fft(x, FFT_N, false);
            for (int i = 1; i < FFT_N / 2; i++) {
                double fr = i * FS / FFT_N, pw = creal(x[i] * conj(x[i]));
                if (fr < 2.9e6) p_total += pw;
                if (fr < 0.5e6) p05 += pw;
                if (fr >= 2e6 && fr < 2.9e6) p23 += pw;
            }
            for (int i = 0; i < FFT_N; i++) x[i] = i < R_W ? line_band[R_X0 + i] : 0;
            fft(x, FFT_N, false);
            for (int i = 0; i < FFT_N; i++) x[i] = x[i] * conj(x[i]);
            fft(x, FFT_N, true);
            for (int i = 0; i < R_W; i++) acf[i] += creal(x[i]);
        }
    int n = 3 * R_H * R_W;
    m.rms24 = sqrt(e24 / n); m.rms29 = sqrt(e29 / n);
    m.share05 = p05 / p_total; m.share23 = p23 / p_total;
    int half = 0, zero = 0;
    while (acf[half] > 0.5 * acf[0]) half++;
    while (acf[zero] > 0) zero++;
    m.acf_half_ns = (half - 1 + (acf[half - 1] - 0.5 * acf[0]) / (acf[half - 1] - acf[half])) / FS * 1e9;
    m.acf_zero_ns = zero / FS * 1e9;
    for (int l = 0; l + 1 < R_H; l++) m.r_line += corr(diff[0][l], diff[0][l + 1], R_W) / (R_H - 1);
    m.r_frame = (corr(&diff[0][0][0], &diff[1][0][0], R_H * R_W) + corr(&diff[1][0][0], &diff[2][0][0], R_H * R_W)) / 2;
    /* Per-column variance: flat, with no lattice at the old 8.59 and 143
     * sample spacings (its spectrum stays within the random-line spread). */
    for (int f = 0; f < 3; f++) for (int l = 0; l < R_H; l++) for (int i = 0; i < R_W; i++)
        column_var[i] += diff[f][l][i] * diff[f][l][i] / (3 * R_H);
    double mean = 0, var = 0;
    for (int i = 0; i < R_W; i++) mean += column_var[i] / R_W;
    for (int i = 0; i < R_W; i++) var += pow(column_var[i] - mean, 2) / R_W;
    m.column_cv = sqrt(var) / mean;
    double power[FFT_N / 2];
    for (int i = 0; i < FFT_N; i++) x[i] = i < R_W ? column_var[i] - mean : 0;
    fft(x, FFT_N, false);
    for (int i = 0; i < FFT_N / 2; i++) power[i] = creal(x[i] * conj(x[i]));
    /* A lattice is a line: compare each spacing's bins with the median of
     * the spectrum around them. */
    double worst = 0;
    const double periods[2] = {8.59, 143.2};
    for (int k = 0; k < 2; k++) {
        int bin = (int)lround(FFT_N / periods[k]), n_near = 0;
        double near[64];
        for (int b = bin - 28; b <= bin + 28; b++) if (b > 0 && abs(b - bin) > 3) near[n_near++] = power[b];
        for (int i = 1; i < n_near; i++) { double t = near[i]; int j = i; while (j > 0 && near[j - 1] > t) { near[j] = near[j - 1]; j--; } near[j] = t; }
        for (int b = bin - 1; b <= bin + 1; b++) worst = fmax(worst, power[b] / near[n_near / 2]);
    }
    m.lattice = worst;
    rig_free(&rc, gpu); rig_free(&rn, gpu);
    return m;
}
static void no_canceller(VHSParams *p) { p->canceller_limit_ire = 0; p->sharpness = 0; }

static void luma_noise_tests(SDL_GPUDevice *gpu) {
    LumaNoise black = luma_noise(gpu, 0x0f, no_canceller), grey = luma_noise(gpu, 0x10, no_canceller),
              white = luma_noise(gpu, 0x30, no_canceller), deflt = luma_noise(gpu, 0x10, NULL);
    printf("VHS luma noise, canceller and sharpness off: black %.3f, white %.3f IRE (0-2.4 MHz), ratio %.3f\n",
           black.rms24, white.rms24, white.rms24 / black.rms24);
    printf("  grey: <0.5 MHz %.1f%%, 2-2.9 MHz %.1f%%, ACF 0.5 at %.0f ns, zero at %.0f ns, r(line) %+.3f, r(frame) %+.4f, column CV %.3f, lattice %.1f x median\n",
           100 * grey.share05, 100 * grey.share23, grey.acf_half_ns, grey.acf_zero_ns, grey.r_line, grey.r_frame, grey.column_cv, grey.lattice);
    printf("  defaults: %.3f IRE (0-2.9 MHz), <0.5 MHz %.1f%%\n", deflt.rms29, 100 * deflt.share05);
    CHECK(fabs(black.rms24 - 0.87) < 0.1);           /* golden 0.87 before the canceller */
    CHECK(fabs(white.rms24 / black.rms24 - 1.26) < 0.1);
    CHECK(grey.share05 <= 0.10 && grey.share23 >= 0.40);
    CHECK(grey.acf_half_ns > 70 && grey.acf_half_ns < 120 && grey.acf_zero_ns < 250);
    CHECK(fabs(grey.r_line) < 0.05);
    CHECK(fabs(grey.r_frame) < 0.02);
    CHECK(grey.column_cv < 0.10);
    CHECK(grey.lattice < 10);
    CHECK(deflt.rms29 > 0.45 && deflt.rms29 < 0.65);  /* after the canceller, US4698696 values */
    CHECK(deflt.share05 > 0.20 && deflt.share05 < 0.30);
}

static void chroma_noise(SDL_GPUDevice *gpu) {
    enum { FRAMES = 7 };
    static float raster[N_RASTER]; static double clean[N_RASTER], out[N_RASTER]; static uint8_t pic[256 * 240];
    static double re[FRAMES][R_H][R_W];
    static double complex zc[VHS_SPL], zn[VHS_SPL];
    VHSParams p = deck_params(); static_transport(&p);
    VHSParams c = p; clean_tape(&c);
    picture_fill(pic, 0x10); nes_raster(pic, raster);
    Rig rc, rn; CHECK(rig_init(&rc, gpu, &c)); CHECK(rig_init(&rn, gpu, &p));
    rig_run(&rc, gpu, raster, 10, NULL, NULL, 0, 1, clean);
    double energy = 0;
    for (int f = 0; f < FRAMES; f++) {
        rig_run(&rn, gpu, raster, 20 + f, NULL, NULL, 0, 1, out);
        for (int l = 0; l < R_H; l++) {
            chroma(clean + (l + R_L0) * VHS_SPL, l + R_L0, zc);
            chroma(out + (l + R_L0) * VHS_SPL, l + R_L0, zn);
            for (int i = 0; i < R_W; i++) {
                double complex d = zn[R_X0 + i] - zc[R_X0 + i];
                re[f][l][i] = creal(d);
                energy += creal(d * conj(d)) / 2;
            }
        }
    }
    double rms = sqrt(energy / (FRAMES * R_H * R_W)), r_line = 0, r_frame = 0;
    for (int l = 0; l + 1 < R_H; l++) r_line += corr(re[0][l], re[0][l + 1], R_W) / (R_H - 1);
    for (int f = 0; f + 1 < FRAMES; f++) r_frame += corr(&re[f][0][0], &re[f + 1][0][0], R_H * R_W) / (FRAMES - 1);
    printf("VHS chroma noise: %.3f IRE per component, r(line) %+.3f, r(frame) %+.4f\n", rms, r_line, r_frame);
    CHECK(rms > 0.50 && rms < 0.62);         /* 0.8 IRE on tape (DH), then the 1H comb */
    CHECK(r_line > 0.40 && r_line < 0.55);
    CHECK(fabs(r_frame) < 0.03);
    rig_free(&rc, gpu); rig_free(&rn, gpu);
}

/* Timing moves luma and chroma content together; the carrier does not
 * move, so the hue stays put (the old stage rotated it 1.29 deg per ns). */
static void timebase_hue(SDL_GPUDevice *gpu) {
    static float raster[N_RASTER]; static double a[N_RASTER], b[N_RASTER]; static uint8_t pic[256 * 240];
    static VHSLineEntry zero[VHS_TABLE_LINES], moved[VHS_TABLE_LINES];
    static double complex za[VHS_SPL], zb[VHS_SPL];
    double tau_ns[VHS_TABLE_LINES];
    uint32_t state = 12345;
    for (int i = 0; i < VHS_TABLE_LINES; i++) {
        double u1 = ((state = state * 1664525u + 1013904223u) >> 8) / 16777216.0 + 1e-9;
        double u2 = ((state = state * 1664525u + 1013904223u) >> 8) / 16777216.0;
        tau_ns[i] = 30 * sqrt(-2 * log(u1)) * cos(2 * M_PI * u2) + 150 * sin(2 * M_PI * (i - 1) / 262.0);
        zero[i] = (VHSLineEntry){.x_switch = -1, .noise = 1, .noise_new = 1};
        moved[i] = zero[i];
        moved[i].tau = (float)(tau_ns[i] * 1e-9 * FS);
    }
    VHSParams p = deck_params(); static_transport(&p); clean_tape(&p);
    Rig r; CHECK(rig_init(&r, gpu, &p));
    picture_fill(pic, 0x12); nes_raster(pic, raster);
    rig_run(&r, gpu, raster, 10, zero, NULL, 0, 1, a);
    rig_run(&r, gpu, raster, 10, moved, NULL, 0, 1, b);
    double worst = 0;
    for (int l = R_L0; l < R_L1; l++) {
        chroma(a + l * VHS_SPL, l, za); chroma(b + l * VHS_SPL, l, zb);
        double complex sa = 0, sb = 0;
        for (int x = R_X0; x < R_X1; x++) { sa += za[x]; sb += zb[x]; }
        worst = fmax(worst, fabs(carg(sb / sa)) * 180 / M_PI);
    }
    /* Luma edges of 32-pixel bars move by exactly tau. */
    picture_bars(pic, 0x0f, 0x30); nes_raster(pic, raster);
    rig_run(&r, gpu, raster, 10, zero, NULL, 0, 0, a);
    rig_run(&r, gpu, raster, 10, moved, NULL, 0, 0, b);
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int l = R_L0; l < R_L1; l++) {
        double pos[2] = {0, 0}; int count[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            const double *y = (k ? b : a) + l * VHS_SPL;
            for (int x = R_X0; x < R_X1; x++)
                if (y[x] < 59.7 && y[x + 1] >= 59.7) { pos[k] += x + (59.7 - y[x]) / (y[x + 1] - y[x]); count[k]++; }
        }
        if (!count[0] || count[0] != count[1]) continue;
        double t = (tau_ns[l + 1] + (tau_ns[l + 2] - tau_ns[l + 1]) * (R_X0 + R_X1) / 2.0 / VHS_SPL);
        double d = (pos[1] - pos[0]) / count[0] / FS * 1e9;
        sx += t; sy += d; sxx += t * t; sxy += t * d; n++;
    }
    double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    printf("VHS timebase: worst per-line hue change %.4f deg, edge displacement slope %.4f\n", worst, slope);
    CHECK(worst < 0.1);
    CHECK(fabs(slope - 1) < 0.02);
    rig_free(&r, gpu);
}

/* IEC pre-emphasis clipped at 160% white before FM: bright NES edges
 * smear to the right; small steps do not. */
static void emphasis_clip(SDL_GPUDevice *gpu) {
    static float raster[N_RASTER]; static double out[N_RASTER], y[VHS_SPL]; static uint8_t pic[256 * 240];
    struct { int lo, hi; float clip; float sharp; } cases[] = {{0x0f, 0x30, 160, 0.2f}, {0x0f, 0x00, 160, 0.2f},
                                                            {0x0f, 0x30, 200, 0.2f}, {0x0f, 0x30, 160, 0}};
    double rise[4][5], fall[4][5], pre[4];
    for (int c = 0; c < 4; c++) {
        VHSParams p = deck_params(); static_transport(&p); clean_tape(&p);
        p.white_clip_pct = cases[c].clip; p.sharpness = cases[c].sharp;
        Rig r; CHECK(rig_init(&r, gpu, &p));
        picture_bars(pic, cases[c].lo, cases[c].hi); nes_raster(pic, raster);
        rig_run(&r, gpu, raster, 10, NULL, NULL, 0, 0, out);
        band(out + 100 * VHS_SPL, 0, 2.9e6, y);
        const double *raw = out + 100 * VHS_SPL;
        double lo = 0, top = 0, hi = 0, lo2 = 0;
        for (int x = 600; x < 680; x++) lo += y[x] / 80;
        for (int x = 700; x < 1100; x++) top = fmax(top, y[x]);
        int i = 700; while (y[i] < lo + 0.5 * (top - lo)) i++;
        for (int x = i + 220; x < i + 250; x++) hi += y[x] / 30;
        i = 700; while (y[i] < lo + 0.5 * (hi - lo)) i++;
        int j = i + 128; while (y[j] > hi - 0.5 * (hi - lo)) j++;
        for (int x = j + 220; x < j + 250; x++) lo2 += y[x] / 30;
        const double us[5] = {0.25, 0.5, 1, 2, 3};
        for (int k = 0; k < 5; k++) {
            int d = (int)lround(us[k] * 1e-6 * FS);
            rise[c][k] = hi - y[i + d]; fall[c][k] = y[j + d] - lo2;
        }
        /* Before the edge: raw output, nothing ahead of the analog filters. */
        pre[c] = 0;
        for (int x = i - 13 - 80; x <= i - 13; x++) pre[c] = fmax(pre[c], fabs(raw[x] - lo));
        rig_free(&r, gpu);
    }
    printf("VHS emphasis clip: $0F-$30 rise shortfall %.1f/%.1f/%.1f/%.1f/%.1f IRE at 0.25/0.5/1/2/3 us, fall excess %.1f at 1 us\n",
           rise[0][0], rise[0][1], rise[0][2], rise[0][3], rise[0][4], fall[0][2]);
    printf("  $0F-$00 %.1f at 1 us; white clip 200%% %.1f at 0.5 us; sharpness 0: %.2f IRE ahead of the edge\n",
           rise[1][2], rise[2][1], pre[3]);
    CHECK(rise[0][2] > 17 && rise[0][2] < 27);      /* golden 22.0 */
    CHECK(rise[0][4] > 2 && rise[0][4] < 7);         /* golden 4.5 */
    CHECK(fall[0][2] > 11 && fall[0][2] < 19);      /* golden 14.9 */
    for (int k = 2; k < 5; k++) CHECK(fabs(rise[1][k]) < 4 && fabs(fall[1][k]) < 4);  /* golden 1.6 */
    CHECK(rise[2][1] < 20);                           /* golden 15.8 */
    CHECK(pre[3] < 1);
}

/* Dropout compensator (JVC 1H loop) and the pulse-count discriminator
 * without it: a 1 um, 40 us defect is -37 dB at 3.9 MHz. */
static void dropouts(SDL_GPUDevice *gpu) {
    static float raster[N_RASTER]; static double out[N_RASTER], clean[N_RASTER]; static uint8_t pic[256 * 240];
    static VHSDefect defects[VHS_MAX_DEFECTS];
    for (int y = 0; y < 240; y++) memset(pic + y * 256, (y % 8) < 4 ? 0x00 : 0x10, 256);
    nes_raster(pic, raster);
    const struct { int line; double us, um; } spots[3] = {{100, 40, 1.0}, {150, 10, 0.25}, {170, 25, 0.45}};
    memset(defects, 0, sizeof(defects));
    for (int i = 0; i < 3; i++)
        defects[i] = (VHSDefect){(float)(spots[i].line * VHS_SPL + 1000), (float)(spots[i].us * 1e-6 * FS),
                                 (float)spots[i].um, (float)(1e-6 * FS), -1e9f, 1e9f, 0, 0};
    double above[2] = {0}, excess[2] = {0}, shallow = 0;
    for (int doc = 1; doc >= 0; doc--) {
        VHSParams p = deck_params(); static_transport(&p); p.doc = doc;
        Rig r; CHECK(rig_init(&r, gpu, &p));
        static VHSDefect none[VHS_MAX_DEFECTS];
        rig_run(&r, gpu, raster, 10, NULL, none, 0, 1, clean);
        rig_run(&r, gpu, raster, 10, NULL, defects, 3, 1, out);
        int x0 = 1000 + 100, x1 = 1000 + (int)(40e-6 * FS) - 60;
        for (int x = x0; x < x1; x++) {
            above[doc] += (out[100 * VHS_SPL + x] - out[99 * VHS_SPL + x - 2]) / (x1 - x0);
            excess[doc] += (out[100 * VHS_SPL + x] - clean[100 * VHS_SPL + x]) / (x1 - x0);
        }
        if (!doc) {
            double e = 0; int n = 0;
            for (int x = 1100; x < 1000 + (int)(10e-6 * FS) - 60; x++, n++) e += pow(out[150 * VHS_SPL + x] - clean[150 * VHS_SPL + x], 2);
            shallow = sqrt(e / n);
        }
        rig_free(&r, gpu);
    }
    printf("VHS dropouts: DOC on, output minus the line above %+.2f IRE; DOC off, white streak %+.1f IRE; 0.25 um defect %.2f IRE RMS\n",
           above[1], excess[0], shallow);
    CHECK(fabs(above[1]) < 2);
    CHECK(excess[0] > 30);                            /* golden +54 */
    CHECK(shallow > 1.5 && shallow < 4);              /* golden 2.4 */
}

/* TV H-PLL, 250 Hz, damping 0.7, V-blank gain 2.5: a 1 us step at the
 * head switch (line 238.5) leaves -72 ns at the top and settles by line 40. */
static void line_pll(SDL_GPUDevice *gpu) {
    enum { LINES = 262, FRAMES = 3 };
    const int width = 2728, spp = 8;
    SignalChain sc; CHECK(chain_init(&sc, gpu, LINES * width, "shaders/compute"));
    double line_rate = FS / width, wn = 2 * M_PI * 250 / line_rate;
    GpuReceiverPLLParams pp = {LINES, (uint32_t)width, spp, 0, 0, (float)(1.4 * wn), (float)(wn * wn), 2.5f, 1, 21, 0, 0};
    int loop = chain_add_stage(&sc, "Line PLL", CHAIN_KERNEL_RECEIVER_PLL, &pp, sizeof(pp), 1, 1);
    ChainStage *s = &sc.stages[loop]; s->io_typed = true;
    s->ro_count = 1; s->ro[0] = CBR_AUX0; s->rw_count = 1; s->rw[0] = CBR_AUX1;
    static float meas[LINES * 4], ref[(LINES + 2) * 4];
    memset(ref, 0, sizeof(ref));
    CHECK(gpu_buffer_upload(gpu, sc.aux[1], ref, sizeof(ref)));
    double step = 1e-6 * FS, residual[LINES];
    for (int f = 0; f < FRAMES; f++) {
        for (int l = 0; l < LINES; l++) {
            bool after = f > 1 || (f == 1 && l >= 239);
            bool vertical = l >= 245 && l < 248;
            meas[l * 4] = 0; meas[l * 4 + 1] = 0;
            meas[l * 4 + 2] = vertical ? -1 : 0.3f;
            meas[l * 4 + 3] = vertical ? 0 : (float)(after ? step : 0);
        }
        CHECK(gpu_buffer_upload(gpu, sc.aux[0], meas, sizeof(meas)));
        CHECK(chain_run(&sc, gpu));
        CHECK(gpu_buffer_download(gpu, sc.aux[1], ref, sizeof(ref)));
        if (f == 2) for (int l = 0; l < LINES; l++) residual[l] = (step - ref[l * 4 + 3]) / FS * 1e9;
    }
    printf("TV line PLL after a 1 us head-switch step: line 0 %+.1f ns, line 20 %+.1f, line 40 %+.1f\n",
           residual[0], residual[20], residual[40]);
    CHECK(fabs(residual[0] + 72) < 10);
    double late = 0;
    for (int l = 40; l < 230; l++) late = fmax(late, fabs(residual[l]));
    CHECK(late < 5);
    chain_destroy(&sc, gpu);
}

int test_vhs_fidelity(SDL_GPUDevice *gpu) {
    failures = 0;
    levels(gpu);
    luma_noise_tests(gpu);
    chroma_noise(gpu);
    timebase_hue(gpu);
    emphasis_clip(gpu);
    dropouts(gpu);
    line_pll(gpu);
    return failures;
}
