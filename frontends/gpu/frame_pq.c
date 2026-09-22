/* HDR recording conversion: see frame_pq.h. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE   /* _SC_NPROCESSORS_ONLN */
#include "frame_pq.h"
#include "gpu_half.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The PQ table is indexed by the float bits of Y = nits / 10000: the
 * exponent and the top PQ_STEPS mantissa bits pick a knot, the remaining
 * mantissa bits interpolate linearly to the next. Knots are spaced 1/128 of
 * an octave from 2^-48 to 1, where the curve is smooth enough that the
 * interpolation stays within 1e-6 of it (0.06 of a 16-bit code). Below
 * 2^-48 (3.5e-11 nits) the signal is PQ(2^-48), which rounds to code 0. */
#define PQ_OCTAVES 48
#define PQ_STEPS   7
#define PQ_KNOTS   ((PQ_OCTAVES << PQ_STEPS) + 2)

struct FramePQ {
    float half[65536];   /* half-float bits to linear value, non-finite as 0 */
    float pq[PQ_KNOTS];  /* signal at each knot, 0..1 */
    float white_nits;
};

void frame_pq_bt2020(const float in[3], float out[3]) {
    out[0] = 0.6274039f * in[0] + 0.3292830f * in[1] + 0.0433131f * in[2];
    out[1] = 0.0690973f * in[0] + 0.9195404f * in[1] + 0.0113623f * in[2];
    out[2] = 0.0163914f * in[0] + 0.0880133f * in[1] + 0.8955953f * in[2];
}

double frame_pq_encode(double nits) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128;
    const double c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    double y = nits > 0 ? fmin(nits / FRAME_PQ_PEAK_NITS, 1) : 0;
    double p = pow(y, m1);
    return pow((c1 + c2 * p) / (1 + c3 * p), m2);
}

static const uint32_t pq_base = (uint32_t)(127 - PQ_OCTAVES) << 23;   /* bits of 2^-48 */

/* y in 0..1. */
static inline float pq_signal(const float *table, float y) {
    uint32_t bits;
    memcpy(&bits, &y, sizeof(bits));
    if (bits <= pq_base) return table[0];
    uint32_t offset = bits - pq_base;
    uint32_t i = offset >> (23 - PQ_STEPS);
    float f = (float)(offset & ((1u << (23 - PQ_STEPS)) - 1)) * (1.0f / (1u << (23 - PQ_STEPS)));
    return table[i] + (table[i + 1] - table[i]) * f;
}

float frame_pq_lookup(const FramePQ *pq, float nits) {
    float y = nits > 0 ? fminf(nits / (float)FRAME_PQ_PEAK_NITS, 1) : 0;
    return pq_signal(pq->pq, y);
}

FramePQ *frame_pq_create(double white_nits) {
    if (!(white_nits > 0 && white_nits <= FRAME_PQ_PEAK_NITS)) return NULL;
    FramePQ *pq = malloc(sizeof(*pq));
    if (!pq) return NULL;
    pq->white_nits = (float)white_nits;
    for (int i = 0; i < 65536; i++) {
        float v = gpu_half_to_float((uint16_t)i);
        pq->half[i] = isfinite(v) ? v : 0;
    }
    for (int i = 0; i < PQ_KNOTS; i++) {
        uint32_t bits = pq_base + ((uint32_t)i << (23 - PQ_STEPS));
        float y;
        memcpy(&y, &bits, sizeof(y));
        pq->pq[i] = (float)frame_pq_encode(fmin(y, 1) * FRAME_PQ_PEAK_NITS);
    }
    return pq;
}

void frame_pq_destroy(FramePQ *pq) { free(pq); }

/* One 3840x2880 frame takes 27 ms on an idle M5 performance core and
 * 44-52 ms with other work running, so a large frame is split into bands
 * of rows handed out from a shared counter to up to eight threads; faster
 * and slower cores then finish together. */
#define BAND_ROWS    16
#define MAX_THREADS  8
#define MIN_THREADED (1 << 19)   /* pixels; smaller frames stay on one thread */

typedef struct {
    const FramePQ  *pq;
    const uint16_t *pixels;   /* RGBA half */
    uint16_t       *out;      /* RGB */
    int             width, height;
    float           scale;    /* stored value to Y = nits / 10000 */
    atomic_int      next_row;
} Job;

typedef struct {
    Job      *job;
    pthread_t thread;
    float     max_y;
    double    sum_y;
} Worker;

static void convert_pixels(Worker *w, size_t first, size_t count) {
    const float *half = w->job->pq->half, *table = w->job->pq->pq;
    const uint16_t *px = w->job->pixels + first * 4;
    uint16_t *out = w->job->out + first * 3;
    float scale = w->job->scale, max_y = w->max_y;
    double sum_y = 0;
    for (size_t i = 0; i < count; i++, px += 4, out += 3) {
        float in[3] = { half[px[0]], half[px[1]], half[px[2]] }, c[3];
        frame_pq_bt2020(in, c);
        float pixel_max = 0;
        for (int k = 0; k < 3; k++) {
            /* Negative after the matrix is outside BT.2020 too; `>` also
             * turns a NaN from out-of-range inputs into black. */
            float y = c[k] > 0 ? fminf(c[k] * scale, 1) : 0;
            pixel_max = fmaxf(pixel_max, y);
            out[k] = (uint16_t)(pq_signal(table, y) * 65535.0f + 0.5f);
        }
        max_y = fmaxf(max_y, pixel_max);
        sum_y += pixel_max;
    }
    w->max_y = max_y;
    w->sum_y += sum_y;
}

static void *convert_bands(void *user) {
    Worker *w = user;
    Job *job = w->job;
    for (;;) {
        int y = atomic_fetch_add(&job->next_row, BAND_ROWS);
        if (y >= job->height) break;
        int rows = job->height - y < BAND_ROWS ? job->height - y : BAND_ROWS;
        convert_pixels(w, (size_t)y * job->width, (size_t)rows * job->width);
    }
    return NULL;
}

bool frame_pq_convert(const FramePQ *pq, const FrameCaptureImage *image,
                      uint16_t *rgb48, FramePQLight *light) {
    if (!pq || !image->pixels || !image->hdr || !rgb48 || image->width <= 0 || image->height <= 0)
        return false;
    size_t count = (size_t)image->width * image->height;
    float white = image->white_level > 0 ? image->white_level : 1;
    Job job = { .pq = pq, .pixels = image->pixels, .out = rgb48,
                .width = image->width, .height = image->height,
                .scale = pq->white_nits / white / (float)FRAME_PQ_PEAK_NITS };
    atomic_init(&job.next_row, 0);
    Worker workers[MAX_THREADS] = {{0}};
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = count < MIN_THREADED || cpus < 2 ? 1 : cpus < MAX_THREADS ? (int)cpus : MAX_THREADS;
    int started = 1;
    for (int i = 0; i < MAX_THREADS; i++) workers[i].job = &job;
    /* A thread that cannot start leaves its bands to the others. */
    for (; started < threads; started++)
        if (pthread_create(&workers[started].thread, NULL, convert_bands, &workers[started])) break;
    convert_bands(&workers[0]);
    float max_y = 0;
    double sum_y = 0;
    for (int i = 0; i < started; i++) {
        if (i) pthread_join(workers[i].thread, NULL);
        max_y = fmaxf(max_y, workers[i].max_y);
        sum_y += workers[i].sum_y;
    }
    if (light) {
        light->max_nits = max_y * FRAME_PQ_PEAK_NITS;
        light->mean_nits = sum_y / (double)count * FRAME_PQ_PEAK_NITS;
    }
    return true;
}
