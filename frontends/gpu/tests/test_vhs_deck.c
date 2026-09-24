/* VHS deck host model: filter design, transport table and dropout events.
 * Thresholds come from the DH (Panasonic PV-7450) measurements and IEC
 * 60774-1; see docs/architecture/gpu-realism-validation.md. */
#include "vhs_deck.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define FS (12 * 315e6 / 88)
#define FSC (315e6 / 88)

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static double db(const VHSDeck *d, int f, double hz) {
    double re, im; vhs_deck_response(d, f, hz, &re, &im); return 20 * log10(hypot(re, im));
}

static void no_tilt(VHSParams *p) { p->tape_tilt_db_per_mhz = 0; }
static void steep_tilt(VHSParams *p) { p->tape_tilt_db_per_mhz = -4.4f; }
/* The record FM high-pass alone, for the tilt shape check. */
static VHSDeck *flat_ref(const VHSDeck *d) {
    static VHSDeck *flat;
    (void)d;
    if (!flat) {
        flat = calloc(1, sizeof(VHSDeck));
        VHSParams p = {0}; vhs_params_defaults(&p); p.enabled = 1; no_tilt(&p);
        vhs_deck_configure(flat, &p, FS);
    }
    return flat;
}

static VHSDeck *deck(void (*edit)(VHSParams *)) {
    VHSDeck *d = calloc(1, sizeof(VHSDeck));
    VHSParams p = {0};
    vhs_params_defaults(&p);
    p.enabled = 1;
    if (edit) edit(&p);
    vhs_deck_configure(d, &p, FS);
    return d;
}

static void filters(void) {
    VHSDeck *d = deck(NULL);
    printf("record Y: %.1f dB at 1 MHz, %.1f dB at 3.4 MHz, %.1f dB at fsc\n",
           db(d, VHS_F_YREC, 1e6), db(d, VHS_F_YREC, 3.4e6), db(d, VHS_F_YREC, FSC));
    CHECK(db(d, VHS_F_YREC, FSC) < -40);            /* IEC 60774-1 6.1.1 */
    CHECK(fabs(db(d, VHS_F_YREC, 1e4)) < 0.01);
    /* Record FM high-pass alone (IEC fig. 22), then with the tape pole's
     * -2 dB/MHz across the deviation (DH carrier level). */
    VHSDeck *flat = deck(no_tilt);
    printf("record FM high-pass: %.1f dB at 629 kHz, %.1f at 1 MHz, %.2f at 3 MHz\n",
           db(flat, VHS_F_RF_REC, 629e3), db(flat, VHS_F_RF_REC, 1e6), db(flat, VHS_F_RF_REC, 3e6));
    CHECK(db(flat, VHS_F_RF_REC, 629e3) < -17);
    CHECK(db(flat, VHS_F_RF_REC, 1e6) < -10);
    CHECK(fabs(db(flat, VHS_F_RF_REC, 3e6)) < 0.5);
    free(flat);
    double ref = db(d, VHS_F_RF_REC, 3.9e6);
    printf("record RF with tape: %+.2f dB at 3.45 MHz, %+.2f at 4.45 MHz\n",
           db(d, VHS_F_RF_REC, 3.45e6) - ref, db(d, VHS_F_RF_REC, 4.45e6) - ref);
    CHECK(fabs(db(d, VHS_F_RF_REC, 3.45e6) - ref - 0.8) < 0.3);
    CHECK(fabs(db(d, VHS_F_RF_REC, 4.45e6) - ref + 1.2) < 0.3);
    /* Spacing loss is a straight line in dB; the three poles stay within
     * 1 dB of the line through 3.9 MHz over 1.4-6 MHz at -2 dB/MHz, and
     * the steepest menu setting still reaches its slope. */
    for (double fq = 1.4e6; fq <= 6.0e6; fq += 0.2e6)
        CHECK(fabs(db(d, VHS_F_RF_REC, fq) - db(flat_ref(d), VHS_F_RF_REC, fq) - (-2e-6 * (fq - 3.9e6))) < 1.0);
    VHSDeck *steep = deck(steep_tilt);
    double slope = (db(steep, VHS_F_RF_REC, 4.45e6) - db(steep, VHS_F_RF_REC, 3.45e6)) / 1.0;
    printf("tape tilt -4.4 dB/MHz: achieved %.2f dB/MHz over 3.45-4.45 MHz\n", slope);
    CHECK(fabs(slope + 4.4) < 0.45);
    free(steep);
    /* The analytic noise shaping passes the positive side and rejects the
     * image of the playback RF band. */
    printf("noise shaping: %.2f dB at 7.5 MHz, %.2f at 1.4, %.1f at 14, %.1f at -1.4, %.1f at -6 MHz\n",
           db(d, VHS_F_NOISE, 7.5e6), db(d, VHS_F_NOISE, 1.4e6), db(d, VHS_F_NOISE, 14e6), db(d, VHS_F_NOISE, -1.4e6), db(d, VHS_F_NOISE, -6e6));
    CHECK(fabs(db(d, VHS_F_NOISE, 7.5e6)) < 0.05 && db(d, VHS_F_NOISE, 1.4e6) > -1.5 && db(d, VHS_F_NOISE, 6e6) > -0.1);
    CHECK(db(d, VHS_F_NOISE, -1.4e6) < -8 && db(d, VHS_F_NOISE, -3e6) < -13 && db(d, VHS_F_NOISE, -6e6) < -22);
    /* APC burst phase noise from the chroma noise: 0.8 IRE per component
     * before the playback band-pass, halved on the burst, averaged over
     * the 10-cycle gate against a 20 IRE burst. */
    printf("APC burst phase noise %.2f deg per line\n", d->burst_noise_deg);
    CHECK(d->burst_noise_deg > 0.5 && d->burst_noise_deg < 0.85);
    /* Emphasis: X = 4 gives +14 dB at high frequency; de-emphasis inverts it. */
    CHECK(fabs(db(d, VHS_F_PRE, 1e4)) < 0.05);
    CHECK(fabs(db(d, VHS_F_PRE, 5e6) - 20 * log10(5.0)) < 0.2);
    for (double f = 1e4; f < 6e6; f *= 1.7) CHECK(fabs(db(d, VHS_F_PRE, f) + db(d, VHS_F_DE, f)) < 1e-3);
    /* Playback luma and colour-under band-pass corners. */
    CHECK(fabs(db(d, VHS_F_YPB, 3.0e6) + 3.01) < 0.1);
    CHECK(fabs(db(d, VHS_F_CPB, 0.5e6) + 3.01) < 0.05);
    CHECK(fabs(db(d, VHS_F_CPB, 0)) < 1e-3);
    CHECK(fabs(db(d, VHS_F_CREC, FSC)) < 0.1);
    CHECK(fabs(db(d, VHS_F_CREC, FSC - 0.5e6) + 3.0) < 0.3 && fabs(db(d, VHS_F_CREC, FSC + 0.5e6) + 3.0) < 0.3);
    printf("Y/C: luma path %.0f ns, chroma path %.0f ns, Y delay line %.1f samples\n",
           d->luma_delay_ns, d->chroma_delay_ns, d->gpu.y_delay);
    CHECK(d->gpu.y_delay >= d->gpu.sharp_d + 2);
    CHECK(fabs(d->gpu.in_gain - 40 / (264.0 / 788.0)) < 1e-3);
    free(d);
}

static void no_varying(VHSParams *p) { p->tbe_varying_ns = 0; p->line_jitter_ns = 0; }
static void only_jitter(VHSParams *p) {
    p->tbe_varying_ns = 0; p->bow_scale = 0; p->skew_ab_ns = p->skew_ba_ns = 0;
}
static void only_varying(VHSParams *p) {
    p->line_jitter_ns = 0; p->bow_scale = 0; p->skew_ab_ns = p->skew_ba_ns = 0;
}
static void whole_switch(VHSParams *p) { no_varying(p); p->switch_lines_before_vsync = 6; }

static void transport(void) {
    static VHSLineEntry a[VHS_TABLE_LINES], b[VHS_TABLE_LINES];
    static VHSDefect da[VHS_MAX_DEFECTS], db_[VHS_MAX_DEFECTS];
    /* A pure function of (seed, frame): a fresh deck and a deck that has
     * run other frames first agree. */
    VHSDeck *d1 = deck(NULL), *d2 = deck(NULL);
    for (uint32_t f = 900; f < 905; f++) vhs_deck_frame(d2, f, b, db_);
    int na = vhs_deck_frame(d1, 1234, a, da), nb = vhs_deck_frame(d2, 1234, b, db_);
    CHECK(na == nb && memcmp(a, b, sizeof(a)) == 0 && memcmp(da, db_, sizeof(VHSDefect) * (size_t)na) == 0);
    /* The next frame's table starts with this frame's last line. */
    vhs_deck_frame(d1, 1235, b, db_);
    CHECK(a[VHS_LINES].tau == b[0].tau && a[VHS_TABLE_LINES - 1].tau == b[1].tau);

    /* Field-to-field correlation of the varying part. DH at lags 1, 12
     * and 24: 0.41, 0.35, 0.05; the resonance gives 0.40, 0.27, 0.04. */
    enum { F = 2400 };
    static double c[F][2 * VHS_HARMONICS];
    for (int f = 0; f < F; f++) vhs_deck_varying(d1, f, c[f]);
    const int lags[3] = {1, 12, 24};
    double r[3] = {0, 0, 0};
    for (int l = 0; l < 3; l++) {
        double s11 = 0, s22 = 0, s12 = 0;
        for (int f = 0; f + lags[l] < F; f++)
            for (int j = 0; j < 2 * VHS_HARMONICS; j++) {
                s11 += c[f][j] * c[f][j]; s22 += c[f + lags[l]][j] * c[f + lags[l]][j];
                s12 += c[f][j] * c[f + lags[l]][j];
            }
        r[l] = s12 / sqrt(s11 * s22);
    }
    printf("varying timing: field correlation lag 1 %.2f, lag 12 %.2f, lag 24 %.2f\n", r[0], r[1], r[2]);
    CHECK(r[0] > 0.3 && r[0] < 0.5);
    CHECK(r[1] > 0.15 && r[1] < 0.35);
    CHECK(fabs(r[2]) < 0.12);

    /* No periodic easing: the spectrum of the per-frame RMS timing change
     * over 1200 frames has no line (the old model had one at 5.56 Hz). */
    enum { N = 1200 };
    static double change[N];
    VHSLineEntry prev[VHS_TABLE_LINES];
    vhs_deck_frame(d1, 0, prev, da);
    for (int f = 1; f <= N; f++) {
        vhs_deck_frame(d1, (uint32_t)f, a, da);
        double e = 0;
        for (int l = 1; l <= VHS_LINES; l++) e += pow(a[l].tau - prev[l].tau, 2);
        change[f - 1] = sqrt(e / VHS_LINES);
        memcpy(prev, a, sizeof(prev));
    }
    /* Welch average of ten 120-frame periodograms. */
    enum { SEG = 120 };
    double power[SEG / 2] = {0}, sorted[SEG / 2 - 1];
    for (int g = 0; g < N / SEG; g++) {
        double mean = 0;
        for (int f = 0; f < SEG; f++) mean += change[g * SEG + f] / SEG;
        for (int k = 1; k < SEG / 2; k++) {
            double re = 0, im = 0;
            for (int f = 0; f < SEG; f++) {
                re += (change[g * SEG + f] - mean) * cos(2 * M_PI * k * f / SEG);
                im += (change[g * SEG + f] - mean) * sin(2 * M_PI * k * f / SEG);
            }
            power[k] += re * re + im * im;
        }
    }
    for (int k = 1; k < SEG / 2; k++) sorted[k - 1] = power[k];
    for (int i = 0; i < SEG / 2 - 1; i++) for (int j = i + 1; j < SEG / 2 - 1; j++)
        if (sorted[j] < sorted[i]) { double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    double median = sorted[(SEG / 2 - 1) / 2], peak = sorted[SEG / 2 - 2];
    printf("per-frame timing change: spectral peak %.1f x median\n", peak / median);
    CHECK(peak < 5 * median);

    /* Switch steps equal the configured skews when nothing else varies. */
    VHSDeck *s = deck(no_varying);
    for (uint32_t f = 40; f < 42; f++) {
        vhs_deck_frame(s, f, a, da);
        const VHSLineEntry *e = &a[239];                  /* raster line 238 */
        CHECK(e->x_switch == 1364);
        double step = (e->tau_new - e->tau_old) / (FS * 1e-9);
        double expected = ((f + 1) & 1) ? s->p.skew_ab_ns : s->p.skew_ba_ns;    /* head of the new field */
        printf("frame %u switch step %.2f ns (expected %.0f)\n", f, step, expected);
        CHECK(fabs(step - expected) < 1);
        CHECK(fabs(e->rf_db_old + 1.5f) < 1e-6 && e->rf_db < 0 && a[236].rf_db == 0);
    }
    /* A whole-number switch position puts the switch at the start of its
     * line: that line begins on the new head and the line before ends on
     * the old head's timing, sagged RF and noise. */
    VHSDeck *w = deck(whole_switch);
    for (uint32_t f = 40; f < 42; f++) {
        vhs_deck_frame(w, f, a, da);
        const VHSLineEntry *e = &a[240];                  /* raster line 239 */
        CHECK(a[239].x_switch < 0 && e->x_switch == 0);
        double step = (e->tau_new - e->tau_old) / (FS * 1e-9);
        double expected = ((f + 1) & 1) ? w->p.skew_ab_ns : w->p.skew_ba_ns;
        printf("frame %u switch at line start: step %.2f ns (expected %.0f)\n", f, step, expected);
        CHECK(fabs(step - expected) < 1);
        CHECK(e->tau == e->tau_old && e->rf_db == e->rf_db_old && fabs(e->rf_db_old + 1.5f) < 1e-6);
        CHECK(e->noise != e->noise_new || w->p.head_b_noise_db == 0);
        /* The next line continues from the new head's start: only the ramp
         * and the bow's first line separate them. */
        CHECK(fabs((a[241].tau - e->tau_new) / (FS * 1e-9)) < 30);
    }
    free(w);
    /* White per-line jitter: line-to-line differences have sqrt(2) x 5 ns. */
    VHSDeck *j = deck(only_jitter);
    double sum = 0; int count = 0;
    for (uint32_t f = 0; f < 60; f++) {
        vhs_deck_frame(j, f, a, da);
        for (int l = 1; l < VHS_LINES; l++) { sum += pow((a[l + 1].tau - a[l].tau) / (FS * 1e-9), 2); count++; }
    }
    double rms = sqrt(sum / count);
    printf("line-to-line jitter %.2f ns (expected %.2f)\n", rms, sqrt(2.0) * 5);
    CHECK(fabs(rms / (sqrt(2.0) * 5) - 1) < 0.1);
    free(d1); free(d2); free(s); free(j);
}

/* The DH band analysis of the varying part, applied to the table: 240
 * lines per field from scan position 10, a least-squares line removed,
 * Hann window, periodogram, RMS in 0-70 / 70-140 / 140-300 / 300-600 /
 * 600-1500 Hz at the line rate. DH: 45.4, 23.3, 10.9, 6.4, 4.1 ns. */
static void varying_bands(void) {
    enum { FIELDS = 600, N = 240, S0 = 10 };
    static VHSLineEntry a[VHS_TABLE_LINES];
    static VHSDefect da[VHS_MAX_DEFECTS];
    static double field[FIELDS][VHS_LINES];
    static bool have[FIELDS][VHS_LINES];
    VHSDeck *d = deck(only_varying);
    const double ns = 1 / (FS * 1e-9);
    memset(have, 0, sizeof(have));
    for (uint32_t f = 0; f + 1 < FIELDS; f++) {
        vhs_deck_frame(d, f, a, da);
        for (int n = 0; n < VHS_LINES; n++) {
            /* Scan position of raster line n: lines from the switch belong to
             * the next field. The switch is at line 238.5, so s = n - 238.5
             * (+262 before it); the table's line 0 is the first raster line. */
            double s = n - d->switch_line;
            int fld = (int)f + 1;
            if (s < 0) { s += VHS_LINES; fld = (int)f; }
            int k = (int)floor(s);
            if (k >= 0 && k < VHS_LINES) { field[fld][k] = a[n + 1].tau * ns; have[fld][k] = true; }
        }
    }
    static const double LO[5] = {0, 70, 140, 300, 600}, HI[5] = {70, 140, 300, 600, 1500};
    static const double DH[5] = {45.4, 23.3, 10.9, 6.4, 4.1};
    double band[5] = {0}, w[N], wsum = 0, time_ms = 0;
    for (int n = 0; n < N; n++) { w[n] = 0.5 - 0.5 * cos(2 * M_PI * n / N); wsum += w[n] * w[n]; }
    int fields = 0;
    for (int f = 1; f + 1 < FIELDS; f++) {
        double x[N];
        bool ok = true;
        for (int n = 0; n < N && ok; n++) { ok = have[f][S0 + n]; x[n] = field[f][S0 + n]; }
        if (!ok) continue;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int n = 0; n < N; n++) { sx += n; sy += x[n]; sxx += (double)n * n; sxy += n * x[n]; }
        double slope = (N * sxy - sx * sy) / (N * sxx - sx * sx), icpt = (sy - slope * sx) / N;
        for (int n = 0; n < N; n++) { x[n] -= icpt + slope * n; time_ms += x[n] * x[n] / N; }
        for (int k = 0; k <= N / 2; k++) {
            double re = 0, im = 0;
            for (int n = 0; n < N; n++) { re += x[n] * w[n] * cos(2 * M_PI * k * n / N); im -= x[n] * w[n] * sin(2 * M_PI * k * n / N); }
            double p = (re * re + im * im) / (N * wsum) * (k > 0 && k < N / 2 ? 2 : 1);
            double hz = k * (FS / VHS_SPL) / N;
            for (int b = 0; b < 5; b++) if (hz >= LO[b] && hz < HI[b]) band[b] += p;
        }
        fields++;
    }
    double quad = 0;
    for (int b = 0; b < 5; b++) { band[b] = sqrt(band[b] / fields); quad += band[b] * band[b]; }
    printf("varying timing bands: %.1f %.1f %.1f %.1f %.1f ns (DH 45.4 23.3 10.9 6.4 4.1), quadrature %.1f, detrended rms %.1f, over %d fields\n",
           band[0], band[1], band[2], band[3], band[4], sqrt(quad), sqrt(time_ms / fields), fields);
    for (int b = 0; b < 5; b++) CHECK(fabs(band[b] / DH[b] - 1) < 0.12);
    CHECK(fabs(sqrt(quad) / d->p.tbe_varying_ns - 1) < 0.05);
    free(d);
}

/* Every deck control at each end of its menu range: the table and the
 * APC residual stay finite and bounded. */
static void sweep(void) {
    static VHSLineEntry a[VHS_TABLE_LINES];
    static VHSDefect da[VHS_MAX_DEFECTS];
    typedef struct { const char *name; size_t offset; float lo, hi; } Range;
#define R(field, lo, hi) {#field, offsetof(VHSParams, field), lo, hi}
    static const Range ranges[] = {
        R(white_clip_pct, 110, 250), R(dark_clip_pct, 0, 100), R(fm_sync_hz, 3000000, 4000000),
        R(fm_white_hz, 4000000, 5400000), R(rf_cnr_dbhz, 80, 110), R(tape_tilt_db_per_mhz, -4.4f, 0),
        R(mod_noise_hz, 0, 10000), R(head_b_noise_db, -3, 3), R(chroma_noise_ire, 0, 5),
        R(dropout_scale, 0, 20), R(doc_threshold_db, -30, -3), R(canceller_split_hz, 100000, 2000000),
        R(canceller_limit_ire, 0, 15), R(sharpness, 0, 1), R(detail_limit_ire, 0, 50),
        R(apc_loop_hz, 100, 2000), R(yc_delay_ns, -500, 500), R(bow_scale, 0, 5),
        R(tbe_varying_ns, 0, 500), R(tbe_slow_fraction, 0, 1), R(tbe_slow_tau_ms, 20, 5000),
        R(tbe_slow_period_ms, 0, 5000), R(line_jitter_ns, 0, 100), R(switch_lines_before_vsync, 0, 20),
        R(skew_ba_ns, -5000, 5000), R(skew_ab_ns, -5000, 5000)};
#undef R
    int bad = 0;
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        for (int end = 0; end < 2; end++) {
            VHSDeck *d = calloc(1, sizeof(VHSDeck));
            VHSParams p = {0}; vhs_params_defaults(&p); p.enabled = 1;
            *(float *)((char *)&p + ranges[i].offset) = end ? ranges[i].hi : ranges[i].lo;
            vhs_deck_configure(d, &p, FS);
            bool ok = true;
            for (int k = 0; k < (int)(sizeof(d->coeffs) / sizeof(d->coeffs[0])); k++) ok = ok && isfinite(d->coeffs[k]);
            const float *gp = (const float *)&d->gpu;
            for (size_t k = 4; k < sizeof(GpuVHSParams) / sizeof(float); k++) ok = ok && isfinite(gp[k]);
            for (uint32_t f = 0; f < 3 && ok; f++) {
                vhs_deck_frame(d, 300 + f, a, da);
                for (int l = 0; l < VHS_TABLE_LINES; l++) {
                    const float *e = (const float *)&a[l];
                    for (int k = 0; k < 12; k++) ok = ok && isfinite(e[k]);
                    ok = ok && fabsf(a[l].tau) < 2 * VHS_SPL && fabsf(a[l].chroma_phase) < 100;
                }
            }
            if (!ok) { fprintf(stderr, "sweep: %s at %g gives a non-finite or unbounded table\n", ranges[i].name, end ? ranges[i].hi : ranges[i].lo); bad++; }
            free(d);
        }
    printf("menu sweep: %d settings out of range\n", bad);
    CHECK(bad == 0);
}

/* Event rates count every event, recurrences included, like the capture. */
static void dropouts(void) {
    VHSDeck *d = deck(NULL);
    enum { FIELDS = 30000 };
    double seconds = (double)FIELDS * VHS_LINES * VHS_SPL / FS;
    int deep = 0, any = 0, births = 0, followed = 0;
    static VHSTapeEvent now[64], next[64];
    int n_next = vhs_deck_field_events(d, 0, next, 64);
    for (int64_t f = 0; f < FIELDS; f++) {
        int n = n_next;
        memcpy(now, next, sizeof(now));
        n_next = vhs_deck_field_events(d, f + 1, next, 64);
        for (int i = 0; i < n; i++) {
            any += now[i].depth_db >= 6;
            deep += now[i].depth_db >= 20;
            if (now[i].track != 0) continue;
            births++;
            for (int k = 0; k < n_next; k++)
                if (next[k].track == 1 && fabs(next[k].scan_start - now[i].scan_start - 1.5 * VHS_SPL) < 0.5) {
                    followed++;
                    break;
                }
        }
    }
    printf("dropouts: %.1f/s at -6 dB, %.2f/s at -20 dB, recurrence %.3f\n",
           any / seconds, deep / seconds, (double)followed / births);
    CHECK(fabs(any / seconds / 32.8 - 1) < 0.1);
    CHECK(fabs(deep / seconds / 5.5 - 1) < 0.1);
    CHECK(fabs((double)followed / births - 0.31) < 0.05);
    free(d);
}

int main(void) {
    filters();
    transport();
    varying_bands();
    sweep();
    dropouts();
    printf("VHS deck host model: %d failures\n", failures);
    return failures ? 1 : 0;
}
