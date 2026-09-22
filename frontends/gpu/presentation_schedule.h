/* Shared with the bundled SDL Metal backend; times are monotonic seconds. */
#ifndef MYNES_PRESENTATION_SCHEDULE_H
#define MYNES_PRESENTATION_SCHEDULE_H
#include <stdint.h>

static inline double mynes_presentation_target(double previous, double now,
                                              double interval, int64_t advance,
                                              int reset) {
    double target = previous + interval * (double)advance;
    /* A short stall can consume the entire GPU lead without crossing the old
     * three-frame stall threshold. Do not keep issuing expired deadlines, or
     * depend on a settings change to restore the lead. Keep half an interval
     * for GPU work; rebase to the same two-interval lead used at startup.
     * Bound the other direction too, e.g. after source-clock catch-up. */
    if (reset || previous <= 0 || advance <= 0 || advance > 3 ||
        target < now + interval * 0.5 || target > now + interval * 4)
        return now + interval * 2;
    return target;
}
#endif
