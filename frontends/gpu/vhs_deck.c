/* NTSC SP VHS deck, host side. See vhs_deck.h for the signal flow. */
#include "vhs_deck.h"
#include <complex.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FSC (315e6 / 88)
#define NES_SYNC (264.0 / 788.0)      /* NES sync depth in chain units */
#define IRE_PER_OUT (788.0 / (1000.0 / 140.0)) /* 7.14 mV/IRE over the 788 mV unit */
#define WALLACE_DB (54.6e-6 / 5.8)     /* spacing loss, dB per um per Hz at 5.8 m/s */
#define SAG_DB 1.5                     /* RF envelope sag before the switch (DH) */
#define SAG_LINES 3.0
#define BURST_NOISE_DEG 2.1            /* burst phase measurement noise per line (DH) */
#define APC_DAMPING 0.7
#define RECUR_P 0.31                   /* dropout found again on the next track (DH) */
#define RECUR_TRACKS 4
#define DROP_RATE_6DB 32.8             /* dropouts per second reaching -6 dB (DH) */
/* Events reaching -6 dB per birth: the birth plus recurrences with
 * probability 0.31 per track over four tracks, each 0.6-1 times as deep
 * (integrated over the depth distribution below). */
#define EVENTS_PER_BIRTH 1.278

typedef double complex cplx;

/* ------------------------------------------------------------------------
 * Filter design: analog zeros/poles/gain, bilinear transform per factor,
 * partial fractions H(w) = d + sum r / (1 - p w), w = 1/z.
 * ------------------------------------------------------------------------ */
typedef struct { cplx z[16], p[16]; int nz, np; cplx k; } Zpk;

static Zpk zpk_unit(void) { Zpk f; memset(&f, 0, sizeof(f)); f.k = 1; return f; }

static void zpk_cascade(Zpk *a, const Zpk *b) {
    for (int i = 0; i < b->nz; i++) a->z[a->nz++] = b->z[i];
    for (int i = 0; i < b->np; i++) a->p[a->np++] = b->p[i];
    a->k *= b->k;
}

static void butter_proto(int n, cplx *q) {
    for (int k = 0; k < n; k++) q[k] = cexp(I * M_PI * (2 * k + n + 1) / (2.0 * n));
}

static Zpk butter_lp(int n, double wc) {
    Zpk f = zpk_unit(); cplx q[8]; butter_proto(n, q);
    for (int k = 0; k < n; k++) { f.p[f.np++] = wc * q[k]; f.k *= -wc * q[k]; }
    return f;
}

static Zpk butter_hp(int n, double wc) {
    Zpk f = zpk_unit(); cplx q[8]; butter_proto(n, q);
    for (int k = 0; k < n; k++) { f.z[f.nz++] = 0; f.p[f.np++] = wc / q[k]; }
    return f;
}

/* Butterworth band-pass from the order-n prototype, edges w1 < w2. */
static Zpk butter_bp(int n, double w1, double w2) {
    Zpk f = zpk_unit(); cplx q[8]; butter_proto(n, q);
    double b = w2 - w1, w0sq = w1 * w2;
    for (int k = 0; k < n; k++) {
        cplx root = csqrt(q[k] * q[k] * b * b - 4 * w0sq);
        f.p[f.np++] = (q[k] * b + root) / 2;
        f.p[f.np++] = (q[k] * b - root) / 2;
        f.z[f.nz++] = 0;
        f.k *= b;
    }
    return f;
}

/* Third-order Bessel low-pass, -3 dB at wc (scipy besselap norm='mag'). */
static Zpk bessel3_lp(double wc) {
    static const double re[3] = {-1.04740916, -1.3226758, -1.04740916};
    static const double im[3] = {0.99926444, 0, -0.99926444};
    Zpk f = zpk_unit();
    for (int k = 0; k < 3; k++) { cplx p = wc * (re[k] + I * im[k]); f.p[f.np++] = p; f.k *= -p; }
    return f;
}

static Zpk notch(double w0, double q) {
    Zpk f = zpk_unit();
    double a = w0 / (2 * q), b = w0 * sqrt(1 - 1 / (4 * q * q));
    f.z[f.nz++] = I * w0; f.z[f.nz++] = -I * w0;
    f.p[f.np++] = -a + I * b; f.p[f.np++] = -a - I * b;
    return f;
}

/* IEC 60774-1 emphasis (1 + s tau) / (1 + s tau / (1 + X)) or its inverse. */
static Zpk emphasis(double tau, double x, bool inverse) {
    Zpk f = zpk_unit();
    cplx zero = -1 / tau, pole = -(1 + x) / tau;
    f.z[f.nz++] = inverse ? pole : zero;
    f.p[f.np++] = inverse ? zero : pole;
    f.k = inverse ? 1 / (1 + x) : 1 + x;
    return f;
}

static Zpk single_pole(double wp) {
    Zpk f = zpk_unit(); f.p[f.np++] = -wp; f.k = wp; return f;
}

/* Bilinear transform with the analog frequency fw (Hz) mapped exactly. */
static Zpk bilinear(const Zpk *a, double fs, double fw) {
    double k = fw > 0 ? 2 * M_PI * fw / tan(M_PI * fw / fs) : 2 * fs;
    Zpk d = zpk_unit(); d.k = a->k;
    for (int i = 0; i < a->nz; i++) { d.z[d.nz++] = (k + a->z[i]) / (k - a->z[i]); d.k *= k - a->z[i]; }
    for (int i = 0; i < a->np; i++) { d.p[d.np++] = (k + a->p[i]) / (k - a->p[i]); d.k /= k - a->p[i]; }
    while (d.nz < d.np) d.z[d.nz++] = -1;
    return d;
}

static cplx zpk_response(const Zpk *d, double f, double fs) {
    cplx w = cexp(-2 * M_PI * I * f / fs), h = d->k;
    for (int i = 0; i < d->nz; i++) h *= 1 - d->z[i] * w;
    for (int i = 0; i < d->np; i++) h /= 1 - d->p[i] * w;
    return h;
}

typedef struct { int first, count; bool real_input; double fs; } Slot;

/* Complex-input slots run on the analytic RF signal or the colour-under
 * envelope; the colour-under band-pass runs at 3 fsc. */
static Slot slot_of(const VHSDeck *d, int s) {
    const float *h = d->coeffs + 4 * s;
    bool complex_input = s == VHS_F_RF_REC || s == VHS_F_RF_PB || s == VHS_F_CPB;
    return (Slot){(int)h[2], (int)h[3], !complex_input, s == VHS_F_CPB ? d->fs / 4 : d->fs};
}

/* Store digital filter d in slot s. Real-input filters keep one pole of
 * each conjugate pair; the kernel takes 2 Re(r y) for those. */
static int store(float *c, int *next, int s, const Zpk *d, bool real_input) {
    cplx pz = 1, pp = 1;
    for (int i = 0; i < d->nz; i++) pz *= d->z[i];
    for (int i = 0; i < d->np; i++) pp *= d->p[i];
    cplx direct = d->nz == d->np ? d->k * pz / pp : 0;
    int first = *next;
    for (int j = 0; j < d->np; j++) {
        cplx p = d->p[j];
        bool real = fabs(cimag(p)) <= 1e-12 * cabs(p);
        if (real_input && !real && cimag(p) < 0) continue;
        cplx r = d->k;
        for (int i = 0; i < d->nz; i++) r *= 1 - d->z[i] / p;
        for (int l = 0; l < d->np; l++) if (l != j) r /= 1 - d->p[l] / p;
        if (real_input && real) { p = creal(p); r = creal(r); }
        if (*next >= VHS_MAX_SECTIONS) return -1;
        float *v = c + 4 * (VHS_FILTER_SLOTS + (*next)++);
        v[0] = (float)creal(p); v[1] = (float)cimag(p); v[2] = (float)creal(r); v[3] = (float)cimag(r);
    }
    float *h = c + 4 * s;
    h[0] = (float)creal(direct); h[1] = (float)cimag(direct);
    h[2] = (float)first; h[3] = (float)(*next - first);
    return 0;
}

static void store_one_pole(float *c, int *next, int s, double pole) {
    float *v = c + 4 * (VHS_FILTER_SLOTS + *next);
    v[0] = (float)pole; v[1] = 0; v[2] = (float)(1 - pole); v[3] = 0;
    float *h = c + 4 * s;
    h[0] = h[1] = 0; h[2] = (float)*next; h[3] = 1;
    ++*next;
}

void vhs_deck_response(const VHSDeck *d, int filter, double f, double *re, double *im) {
    const float *h = d->coeffs + 4 * filter;
    Slot slot = slot_of(d, filter), *s = &slot;
    cplx w = cexp(-2 * M_PI * I * f / s->fs), out = h[0] + I * h[1];
    for (int j = 0; j < s->count; j++) {
        const float *v = d->coeffs + 4 * (VHS_FILTER_SLOTS + s->first + j);
        cplx p = v[0] + I * v[1], r = v[2] + I * v[3];
        cplx t = r / (1 - p * w);
        if (s->real_input && v[1] != 0) t += conj(r) / (1 - conj(p) * w);
        out += t;
    }
    *re = creal(out); *im = cimag(out);
}

static cplx response(const VHSDeck *d, int filter, double f) {
    double re, im; vhs_deck_response(d, filter, f, &re, &im); return re + I * im;
}

/* Group delay in ns at f, from the phase slope of the stored sections. */
static double group_delay_ns(const VHSDeck *d, int filter, double f) {
    double df = 2e3;
    double a = carg(response(d, filter, f - df)), b = carg(response(d, filter, f + df));
    double dp = remainder(b - a, 2 * M_PI);
    return -dp / (2 * M_PI * 2 * df) * 1e9;
}

/* Impulse response of a real-input slot, or of the real part path of a
 * complex slot driven by a real impulse. */
static void impulse(const VHSDeck *d, int filter, double *out, int n) {
    const float *h = d->coeffs + 4 * filter;
    Slot slot = slot_of(d, filter), *s = &slot;
    for (int t = 0; t < n; t++) out[t] = t == 0 ? h[0] : 0;
    for (int j = 0; j < s->count; j++) {
        const float *v = d->coeffs + 4 * (VHS_FILTER_SLOTS + s->first + j);
        cplx p = v[0] + I * v[1], r = v[2] + I * v[3], y = 1;
        for (int t = 0; t < n; t++) {
            cplx term = r * y;
            out[t] += s->real_input && v[1] != 0 ? 2 * creal(term) : creal(term);
            y *= p;
        }
    }
}

/* Noise power gain sum |h|^2 of a filter driven by unit white noise. For
 * complex-input slots this is per component of circular noise. */
static double power_gain(const VHSDeck *d, int filter) {
    Slot slot = slot_of(d, filter), *s = &slot;
    const float *h = d->coeffs + 4 * filter;
    enum { N = 8192 };
    static cplx acc[N];
    for (int t = 0; t < N; t++) acc[t] = t == 0 ? h[0] + I * h[1] : 0;
    for (int j = 0; j < s->count; j++) {
        const float *v = d->coeffs + 4 * (VHS_FILTER_SLOTS + s->first + j);
        cplx p = v[0] + I * v[1], r = v[2] + I * v[3], y = 1;
        for (int t = 0; t < N; t++) {
            cplx term = r * y;
            acc[t] += s->real_input && v[1] != 0 ? 2 * creal(term) : term;
            y *= p;
        }
    }
    double g = 0;
    for (int t = 0; t < N; t++) g += creal(acc[t] * conj(acc[t]));
    return g;
}

/* ------------------------------------------------------------------------
 * Counter-based random draws: pure functions of (seed, field, stream, i).
 * ------------------------------------------------------------------------ */
static uint64_t mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint64_t key(uint32_t seed, int64_t field, uint32_t stream, uint64_t i) {
    return mix64(mix64(mix64(((uint64_t)seed << 32) ^ stream) ^ (uint64_t)field) ^ i);
}
static double uniform(uint64_t k) { return ((mix64(k) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
static double gauss(uint64_t k) {
    double u1 = uniform(k), u2 = uniform(k ^ 0x5851f42d4c957f2dull);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}
enum { S_FRESH = 1, S_SLOW, S_JITTER, S_BURST, S_JUMP, S_DROP_N, S_DROP };

/* ------------------------------------------------------------------------
 * Transport timing (DH, Panasonic PV-7450 capture).
 * ------------------------------------------------------------------------ */
/* Per-head timing bows: 10 scan harmonics fitted to the measured profiles
 * with the chord removed, so each is zero at the switch. RMS 77 / 39 ns. */
static const double BOW_COS[2][10] = {
    {-43.779, 5.409, -45.502, -8.953, -0.643, 1.584, 0.570, 0.898, -0.416, 0.113},
    {-34.886, -4.328, -34.251, -4.629, 0.734, 1.949, -0.075, 0.820, -0.674, 0.147}};
static const double BOW_SIN[2][10] = {
    {-47.129, 54.061, 47.608, 17.114, 5.702, 4.529, 0.902, 0.286, 0.218, -0.216},
    {-12.946, 13.214, 16.317, 2.471, -0.908, 2.153, 0.275, 0.256, 0.249, -0.158}};

/* sum over m = 1..n of a[m-1] cos(m t) + b[m-1] sin(m t), by rotation. */
static double harmonics(const double *a, const double *b, int stride, int n, double t) {
    double c1 = cos(t), s1 = sin(t), c = c1, s = s1, v = 0;
    for (int m = 1; m <= n; m++) {
        v += a[(m - 1) * stride] * c + b[(m - 1) * stride] * s;
        double cn = c * c1 - s * s1; s = s * c1 + c * s1; c = cn;
    }
    return v;
}

static double bow(const VHSDeck *d, int head, double s) {
    double chord = 0;
    for (int m = 0; m < 10; m++) chord += BOW_COS[head][m];
    double t = 2 * M_PI * s / VHS_LINES;
    return (harmonics(BOW_COS[head], BOW_SIN[head], 1, 10, t) - chord) * d->p.bow_scale;
}

static const double *slow_draws(VHSDeck *d, int64_t field) {
    int slot = (int)(((field % VHS_SLOW_RING) + VHS_SLOW_RING) % VHS_SLOW_RING);
    if (d->slow_tag[slot] != field) {
        for (int j = 0; j < 2 * VHS_HARMONICS; j++)
            d->slow_g[slot][j] = gauss(key((uint32_t)d->p.deck_seed, field, S_SLOW, (uint64_t)j));
        d->slow_tag[slot] = field;
    }
    return d->slow_g[slot];
}

void vhs_deck_varying(VHSDeck *d, int64_t field, double coeff[2 * VHS_HARMONICS]) {
    double sf = fmin(1, fmax(0, d->p.tbe_slow_fraction)), w = sqrt(1 - d->rho * d->rho);
    double slow[2 * VHS_HARMONICS] = {0}, weight = w;
    for (int i = 0; i < VHS_SLOW_TERMS; i++, weight *= d->rho) {
        const double *g = slow_draws(d, field - i);
        for (int j = 0; j < 2 * VHS_HARMONICS; j++) slow[j] += weight * g[j];
    }
    for (int j = 0; j < 2 * VHS_HARMONICS; j++) {
        double fresh = gauss(key((uint32_t)d->p.deck_seed, field, S_FRESH, (uint64_t)j));
        coeff[j] = (sqrt(1 - sf) * fresh + sqrt(sf) * slow[j]) * d->sig[j / 2 + 1] / sqrt(2.0);
    }
}

static double varying(const double *c, double s) {
    return harmonics(c, c + 1, 2, VHS_HARMONICS, 2 * M_PI * s / VHS_LINES);
}

/* Scan position (lines since the switch) and field of raster line n. */
static void scan_of(const VHSDeck *d, int64_t frame, double n, int64_t *field, double *s) {
    if (n >= d->switch_line) { *field = frame + 1; *s = n - d->switch_line; }
    else { *field = frame; *s = n - d->switch_line + VHS_LINES; }
}

typedef struct { int64_t field; double c[2 * VHS_HARMONICS]; } FieldCoeff;

static const double *field_coeff(const FieldCoeff *cache, int n, int64_t field) {
    for (int i = 0; i < n; i++) if (cache[i].field == field) return cache[i].c;
    return cache[0].c;
}

static double tau_at(VHSDeck *d, const double *c, int64_t field, double s, int line, bool jitter) {
    double t = d->offset[field & 1] + d->ramp * s + bow(d, (int)(field & 1), s) + varying(c, s) - d->centre;
    if (jitter) t += d->p.line_jitter_ns * gauss(key((uint32_t)d->p.deck_seed, field, S_JITTER, (uint64_t)line));
    return t;
}

/* ------------------------------------------------------------------------
 * Dropouts (DH, 4 s of capture): depth distribution, depth-dependent
 * length, recurrence on the next track 1.5 H later.
 * ------------------------------------------------------------------------ */
static const double DEPTH_DB[] = {6, 10, 15, 20, 30, 40, 45};
static const double DEPTH_RATE[] = {32.8, 13.3, 8.0, 5.5, 4.25, 1.5, 0};

static double depth_from_uniform(double u) {
    double r = u * DEPTH_RATE[0];
    for (int i = 0; i < 6; i++) {
        double a = DEPTH_RATE[i], b = DEPTH_RATE[i + 1];
        if (r <= a && r >= b) {
            double t = b > 0 ? log(a / r) / log(a / b) : (a - r) / a;
            return DEPTH_DB[i] + t * (DEPTH_DB[i + 1] - DEPTH_DB[i]);
        }
    }
    return DEPTH_DB[6];
}

static double length_us(double depth, double g) {
    double median, sigma;
    if (depth < 10) { median = 2.1; sigma = 1.46; }
    else if (depth < 15) { median = 15.5; sigma = 0.49; }
    else if (depth < 20) { median = 42; sigma = 0.41; }
    else if (depth < 30) { median = 43; sigma = 0.18; }
    else { median = 70; sigma = 0.37; }
    double duration = fmin(400, fmax(0.2, median * exp(sigma * g)));
    /* The medians are widths at -6 dB; add the part of each 1 us taper
     * that is shallower than -6 dB. */
    double w6 = fmin(1, 6 / depth);
    return duration + 2 * acos(1 - 2 * w6) / M_PI;
}

static double field_seconds(const VHSDeck *d) { return VHS_LINES * VHS_SPL / d->fs; }

int vhs_deck_field_events(const VHSDeck *d, int64_t field, VHSTapeEvent *out, int max) {
    int count = 0;
    uint32_t seed = (uint32_t)d->p.deck_seed;
    /* The measured rates count every event, recurrences included, so
     * births are the -6 dB rate over the events each birth produces. */
    double lambda = DROP_RATE_6DB / EVENTS_PER_BIRTH * fmax(0, d->p.dropout_scale) * field_seconds(d);
    double scan = (double)VHS_LINES * VHS_SPL;
    for (int back = 0; back < RECUR_TRACKS; back++) {
        int64_t born = field - back;
        double u = uniform(key(seed, born, S_DROP_N, 0)), p = exp(-lambda), cum = p;
        int n = 0;
        while (u > cum && n < 64) { n++; p *= lambda / n; cum += p; }
        for (int b = 0; b < n; b++) {
            uint64_t base = (uint64_t)b * 16;
            double depth = depth_from_uniform(uniform(key(seed, born, S_DROP, base + 1)));
            double start = uniform(key(seed, born, S_DROP, base)) * scan;
            double len = length_us(depth, gauss(key(seed, born, S_DROP, base + 2))) * 1e-6 * d->fs;
            bool alive = true;
            for (int k = 1; k <= back && alive; k++) {
                alive = uniform(key(seed, born, S_DROP, base + 2 + 2 * k)) < RECUR_P;
                depth *= 0.6 + 0.4 * uniform(key(seed, born, S_DROP, base + 3 + 2 * k));
                start += 1.5 * VHS_SPL;
            }
            if (!alive || start >= scan || count >= max) continue;
            out[count++] = (VHSTapeEvent){field, start, len, depth, back};
        }
    }
    return count;
}

/* ------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------ */
void vhs_deck_configure(VHSDeck *d, const VHSParams *p, double fs) {
    memset(d, 0, sizeof(*d));
    d->p = *p;
    d->fs = fs;
    VHSParams *v = &d->p;
    if (v->fm_white_hz <= v->fm_sync_hz + 1e4f) { v->fm_sync_hz = 3.4e6f; v->fm_white_hz = 4.4e6f; }
    if (v->tbe_slow_tau_ms <= 0) v->tbe_slow_tau_ms = 400;
    if (v->apc_loop_hz <= 0) v->apc_loop_hz = 1000;
    for (int i = 0; i < VHS_SLOW_RING; i++) d->slow_tag[i] = INT64_MIN;

    const double fs3 = fs / 4, w = 2 * M_PI;
    float *c = d->coeffs;
    int next = 0;
    Zpk f, g;
    /* Record luma: Bessel low-pass with a colour trap for > 40 dB at fsc. */
    f = bessel3_lp(w * 3.4e6); f = bilinear(&f, fs, 3.4e6);
    g = notch(w * FSC, 2.0); g = bilinear(&g, fs, FSC); zpk_cascade(&f, &g);
    store(c, &next, VHS_F_YREC, &f, true);
    const double tau = 1.3e-6, x = 4.0, corner = (1 + x) / (w * tau);
    f = emphasis(tau, x, false); f = bilinear(&f, fs, corner);
    store(c, &next, VHS_F_PRE, &f, true);
    f = emphasis(tau, x, true); f = bilinear(&f, fs, corner);
    store(c, &next, VHS_F_DE, &f, true);
    /* Record chroma band-pass, edges prewarped individually. */
    double e1 = 2 * fs * tan(M_PI * (FSC - 0.5e6) / fs), e2 = 2 * fs * tan(M_PI * (FSC + 0.5e6) / fs);
    f = butter_bp(2, e1, e2); f = bilinear(&f, fs, 0);
    store(c, &next, VHS_F_CREC, &f, true);
    f = butter_lp(2, w * 0.4e6); f = bilinear(&f, fs, 0.4e6);
    store(c, &next, VHS_F_MOD, &f, true);
    /* Record FM high-pass (IEC fig. 22) and the head/tape response, a
     * single pole placed for the measured slope at 3.9 MHz. */
    f = butter_hp(3, w * 1.6e6); f = bilinear(&f, fs, 1.6e6);
    double tilt = fabs(v->tape_tilt_db_per_mhz) * 1e-6, f0 = 3.9e6;
    if (tilt > 1e-9) {
        double fp2 = 20 / log(10.0) * f0 / tilt - f0 * f0;
        double fp = sqrt(fmax(fp2, 1e10));
        g = single_pole(w * fp); g = bilinear(&g, fs, f0);
        g.k /= cabs(zpk_response(&g, f0, fs));
        zpk_cascade(&f, &g);
    }
    store(c, &next, VHS_F_RF_REC, &f, false);
    f = butter_hp(2, w * 1.4e6); f = bilinear(&f, fs, 1.4e6);
    g = butter_lp(2, w * 6.0e6); g = bilinear(&g, fs, 6.0e6); zpk_cascade(&f, &g);
    store(c, &next, VHS_F_RF_PB, &f, false);
    store_one_pole(c, &next, VHS_F_ENV, exp(-1 / (0.35e-6 * fs)));
    f = bessel3_lp(w * 3.0e6); f = bilinear(&f, fs, 3.0e6);
    store(c, &next, VHS_F_YPB, &f, true);
    f = butter_lp(2, w * 0.5e6); f = bilinear(&f, fs3, 0.5e6);
    store(c, &next, VHS_F_CPB, &f, false);
    store_one_pole(c, &next, VHS_F_CANC, exp(-w * fmax(1e4, v->canceller_split_hz) / fs));

    /* DOC click: the playback luma filter's impulse response. */
    double taps[VHS_CLICK_TAPS];
    impulse(d, VHS_F_YPB, taps, VHS_CLICK_TAPS);
    for (int t = 0; t < VHS_CLICK_TAPS; t++)
        c[4 * (VHS_FILTER_SLOTS + VHS_MAX_SECTIONS) + t] = (float)taps[t];

    /* Y/C registration: the deck's luma delay line matches the chroma path.
     * The canceller passes large edges undelayed; emphasis cancels. */
    double dl = group_delay_ns(d, VHS_F_YREC, 2e4) + group_delay_ns(d, VHS_F_RF_REC, f0) +
                group_delay_ns(d, VHS_F_RF_PB, f0) + group_delay_ns(d, VHS_F_YPB, 2e4);
    double dc = group_delay_ns(d, VHS_F_CREC, FSC) + 2.5e9 / fs + group_delay_ns(d, VHS_F_CPB, 2e4);
    d->luma_delay_ns = dl; d->chroma_delay_ns = dc;
    double sharp_d = round(233e-9 * fs);
    double y_delay = (dc - dl + v->yc_delay_ns) * 1e-9 * fs;
    double min_delay = sharp_d + 2, c_delay = 0;
    if (y_delay < min_delay) { c_delay = min_delay - y_delay; y_delay = min_delay; }
    /* The deck's own filters and delay line hold the output about 1 us
     * behind its input. A fixed delay is invisible to the TV, which locks
     * to the delayed sync; the table removes it so the sync stays where
     * the receiver's slicer looks for it. */
    d->path_samples = dl * 1e-9 * fs + y_delay;

    /* Record ACC: the kernels measure the burst fundamental over dots 30-42
     * of the composite, and the deck holds the burst envelope after its
     * chroma band-pass at 20 IRE. Scale one to the other on the NES burst
     * (dots 29-44), which is too short for the band-pass to settle, and
     * set the playback ACC so the output burst peaks at 20 IRE (40 p-p). */
    double burst_norm, playback_acc;
    {
        enum { LEN = 640, LEN3 = LEN / 4 };
        double sq[LEN], out[LEN], imp[LEN], imp3[LEN3];
        for (int n = 0; n < LEN; n++) sq[n] = n >= 232 && n < 352 ? (((n + 8) % 12) < 6 ? 1 : -1) : 0;
        impulse(d, VHS_F_CREC, imp, LEN);
        impulse(d, VHS_F_CPB, imp3, LEN3);
        for (int n = 0; n < LEN; n++) { out[n] = 0; for (int k = 0; k <= n; k++) out[n] += imp[k] * sq[n - k]; }
        cplx raw = 0, env[LEN3], pb[LEN3];
        for (int n = 240; n < 336; n++) raw += sq[n] * cexp(-2 * M_PI * I * (n % 12) / 12.0);
        for (int m = 0; m < LEN3; m++) {
            env[m] = 0;
            for (int k = 0; k < 6 && 4 * m - k >= 0; k++)
                env[m] += 2 * out[4 * m - k] * cexp(-2 * M_PI * I * ((4 * m - k) % 12) / 12.0) / 6.0;
        }
        for (int m = 0; m < LEN3; m++) { pb[m] = 0; for (int k = 0; k <= m; k++) pb[m] += imp3[k] * env[m - k]; }
        double peak = 0, peak_pb = 0;
        for (int m = 0; m < LEN3; m++) { peak = fmax(peak, cabs(env[m])); peak_pb = fmax(peak_pb, cabs(pb[m])); }
        burst_norm = (2.0 / 96.0 * cabs(raw)) / peak;
        playback_acc = peak / peak_pb;
    }
    double cn0 = pow(10, v->rf_cnr_dbhz / 10);
    double env = cabs(response(d, VHS_F_RF_REC, f0) * response(d, VHS_F_RF_PB, f0));
    double doc_on = pow(10, v->doc_threshold_db / 20);
    d->gpu = (GpuVHSParams){
        .doc = v->doc ? 1u : 0u, .total = VHS_LINES * VHS_SPL,
        .in_gain = (float)(40 / NES_SYNC), .dark_clip = -v->dark_clip_pct, .white_clip = v->white_clip_pct,
        .f_sync = v->fm_sync_hz, .hz_per_pct = (v->fm_white_hz - v->fm_sync_hz) / 100,
        .sample_rate = (float)fs,
        .mod_noise_sigma = (float)(fmax(0, v->mod_noise_hz) / sqrt(power_gain(d, VHS_F_MOD))),
        .rf_noise_sigma = (float)sqrt(fs / (2 * cn0)),
        .env_norm = (float)(1 / env), .doc_on = (float)doc_on,
        .doc_off = (float)(doc_on * pow(10, 1.94 / 20)),
        .chroma_noise_sigma = (float)(fmax(0, v->chroma_noise_ire) / sqrt(power_gain(d, VHS_F_CPB))),
        .burst_target = 20, .burst_norm = (float)burst_norm,
        .spacing_db = (float)WALLACE_DB, .colour_under_hz = (float)(40 * fs / VHS_SPL),
        .canceller_limit = fmaxf(0, v->canceller_limit_ire), .sharpness = v->sharpness,
        .detail_limit = fmaxf(0, v->detail_limit_ire),
        .y_delay = (float)y_delay, .c_delay = (float)(c_delay / 4),
        .out_scale = (float)(1 / IRE_PER_OUT), .playback_acc = (float)playback_acc,
        .click_scale = (float)(fs * 140 / (2 * M_PI * (v->fm_white_hz - v->fm_sync_hz))),
        .sharp_d = (float)sharp_d};

    /* Transport. Per-harmonic RMS of the varying part from the DH bands. */
    for (int m = 1; m <= VHS_HARMONICS; m++)
        d->sig[m] = m == 1 ? 45.4 : m == 2 ? 23.3 : m <= 4 ? 10.9 / sqrt(2.0) :
                    m <= 9 ? 6.4 / sqrt(5.0) : 4.1 / sqrt(13.0);
    double total = 0;
    for (int m = 1; m <= VHS_HARMONICS; m++) total += d->sig[m] * d->sig[m];
    for (int m = 1; m <= VHS_HARMONICS; m++) d->sig[m] *= fmax(0, v->tbe_varying_ns) / sqrt(total);
    d->rho = exp(-field_seconds(d) * 1000 / v->tbe_slow_tau_ms);
    d->ramp = -(v->skew_ba_ns + v->skew_ab_ns) / (2.0 * VHS_LINES);
    d->offset[0] = v->skew_ba_ns;
    d->offset[1] = v->skew_ba_ns + d->ramp * VHS_LINES + v->skew_ab_ns;
    d->centre = (d->offset[0] + d->offset[1]) / 2 + d->ramp * VHS_LINES / 2;
    d->switch_line = 245 - fmin(20, fmax(0, v->switch_lines_before_vsync));
}

/* ------------------------------------------------------------------------
 * Per-frame table
 * ------------------------------------------------------------------------ */
int vhs_deck_frame(VHSDeck *d, uint32_t frame_u, VHSLineEntry table[VHS_TABLE_LINES],
                   VHSDefect defects[VHS_MAX_DEFECTS]) {
    const int64_t frame = frame_u;
    const double to_samples = 1e-9 * d->fs, line_rate = d->fs / VHS_SPL;
    const uint32_t seed = (uint32_t)d->p.deck_seed;
    const double noise_b = pow(10, d->p.head_b_noise_db / 20);
    FieldCoeff cache[3];
    for (int i = 0; i < 3; i++) { cache[i].field = frame - 1 + i; vhs_deck_varying(d, frame - 1 + i, cache[i].c); }
    const int sw = (int)floor(d->switch_line);
    const double sw_x = (d->switch_line - sw) * VHS_SPL;

    /* Timing at every line start of the previous and this frame, plus the
     * two neighbouring lines the kernels read. */
    double tau[2 * VHS_LINES + 1];
    for (int i = 0; i <= 2 * VHS_LINES; i++) {
        int64_t g = frame - 1 + i / VHS_LINES, field; double s; int n = i % VHS_LINES;
        scan_of(d, g, n, &field, &s);
        tau[i] = tau_at(d, field_coeff(cache, 3, field), field, s, n, true);
    }
    /* Colour APC/AFC: second-order loop on the colour-under phase of the
     * timing, driven by a burst phase measurement with 2.1 deg of noise.
     * Started one frame earlier so it needs no state. */
    double wn = 2 * M_PI * d->p.apc_loop_hz / line_rate, kp = 2 * APC_DAMPING * wn, ki = wn * wn;
    double deg = 360 * d->gpu.colour_under_hz * 1e-9, th = tau[0] * deg, per = 0, residual[2 * VHS_LINES];
    for (int i = 0; i < 2 * VHS_LINES; i++) {
        double in = tau[i] * deg;
        int64_t g = frame - 1 + i / VHS_LINES;
        double meas = in + BURST_NOISE_DEG * gauss(key(seed, g, S_BURST, (uint64_t)(i % VHS_LINES)));
        residual[i] = in - th;
        double e = meas - th;
        per += ki * e; th += kp * e + per;
    }
    for (int i = 0; i < VHS_TABLE_LINES; i++) {
        int k = i + VHS_LINES - 1;                    /* index into tau[] */
        int n = (i - 1 + VHS_LINES) % VHS_LINES;
        int64_t g = frame + (i == 0 ? -1 : i == VHS_TABLE_LINES - 1 ? 1 : 0), field; double s;
        double t_ns = tau[k];
        scan_of(d, g, n, &field, &s);
        VHSLineEntry *e = &table[i];
        memset(e, 0, sizeof(*e));
        e->tau = (float)(t_ns * to_samples - d->path_samples);
        e->x_switch = -1;
        e->rf_db = s > VHS_LINES - SAG_LINES ? (float)(-SAG_DB * (s - (VHS_LINES - SAG_LINES)) / SAG_LINES) : 0;
        e->noise = (float)(field & 1 ? noise_b : 1);
        e->head = (float)(field & 1);
        e->chroma_phase = (float)(residual[k < 2 * VHS_LINES ? k : 2 * VHS_LINES - 1] * M_PI / 180);
        if (n == sw && sw_x > 0) {
            /* Old head to the switch point, new head after it. */
            int64_t fold = g, fnew = g + 1;
            const double *c_old = field_coeff(cache, 3, fold), *c_new = field_coeff(cache, 3, fnew);
            e->x_switch = (float)sw_x;
            e->tau_old = (float)(tau_at(d, c_old, fold, VHS_LINES, sw, false) * to_samples - d->path_samples);
            e->tau_new = (float)(tau_at(d, c_new, fnew, 0, sw, false) * to_samples - d->path_samples);
            e->rf_db_old = (float)-SAG_DB;
            e->noise_new = (float)(fnew & 1 ? noise_b : 1);
            e->rf_phase_jump = (float)((2 * uniform(key(seed, fnew, S_JUMP, 0)) - 1) * M_PI);
        }
    }

    /* Dropouts of the two head scans in this frame. */
    int count = 0;
    const double scan_start = d->switch_line * VHS_SPL, frame_len = (double)VHS_LINES * VHS_SPL;
    for (int k = 0; k < 2; k++) {
        int64_t field = frame + k;
        VHSTapeEvent ev[64];
        int n = vhs_deck_field_events(d, field, ev, 64);
        double from = k ? scan_start : -1e9, to = k ? 1e9 : scan_start;
        for (int i = 0; i < n && count < VHS_MAX_DEFECTS; i++) {
            double r = ev[i].scan_start + scan_start - (k ? 0 : frame_len);
            double lo = fmax(from, -1024), hi = fmin(to, frame_len + 1024);
            if (r + ev[i].length < lo || r > hi) continue;
            defects[count++] = (VHSDefect){(float)r, (float)ev[i].length, (float)(ev[i].depth_db / 36.7),
                (float)(1e-6 * d->fs), (float)from, (float)to, 0, 0};
        }
    }
    d->gpu.frame = frame_u;
    d->gpu.defect_count = (uint32_t)count;
    return count;
}
