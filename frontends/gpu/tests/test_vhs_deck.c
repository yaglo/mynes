/* VHS deck host model: filter design, transport table and dropout events.
 * Thresholds come from the DH (Panasonic PV-7450) measurements and IEC
 * 60774-1; see docs/architecture/gpu-realism-validation.md. */
#include "vhs_deck.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

    /* Field-to-field correlation of the varying part (DH lag 1: 0.41). */
    enum { F = 600 };
    static double c[F][2 * VHS_HARMONICS];
    for (int f = 0; f < F; f++) vhs_deck_varying(d1, f, c[f]);
    double r1 = 0, r24 = 0; int n1 = 0;
    for (int f = 0; f + 24 < F; f++) {
        double s11 = 0, s22 = 0, s12 = 0, t22 = 0, t12 = 0;
        for (int j = 0; j < 2 * VHS_HARMONICS; j++) {
            s11 += c[f][j] * c[f][j]; s22 += c[f + 1][j] * c[f + 1][j]; s12 += c[f][j] * c[f + 1][j];
            t22 += c[f + 24][j] * c[f + 24][j]; t12 += c[f][j] * c[f + 24][j];
        }
        r1 += s12 / sqrt(s11 * s22); r24 += t12 / sqrt(s11 * t22); n1++;
    }
    r1 /= n1; r24 /= n1;
    printf("varying timing: field correlation lag 1 %.2f, lag 24 %.2f\n", r1, r24);
    CHECK(r1 > 0.25 && r1 < 0.5);
    CHECK(fabs(r24) < 0.2);

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
        double expected = ((f + 1) & 1) ? -80 : 1700;    /* head of the new field */
        printf("frame %u switch step %.2f ns (expected %.0f)\n", f, step, expected);
        CHECK(fabs(step - expected) < 1);
        CHECK(fabs(e->rf_db_old + 1.5f) < 1e-6 && e->rf_db < 0 && a[236].rf_db == 0);
    }
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
    dropouts();
    printf("VHS deck host model: %d failures\n", failures);
    return failures ? 1 : 0;
}
