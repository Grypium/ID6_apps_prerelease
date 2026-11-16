/* perf_timer.h - acceleration timers from timestamped speed samples.
 *
 * Standing runs (from a stop): 0-60 mph, 0-100 mph and the quarter mile
 * (time and trap speed). Rolling run: 60-130 mph. Crossing times are
 * interpolated between samples; the start of a standing run is extrapolated
 * back from the first two moving samples (no rollout). Distance is integrated
 * from speed, so the quarter mile is an estimate. Header-only so the host tests
 * and the plugin share it. Speeds in km/h, times in ms.
 */
#ifndef PERF_TIMER_H
#define PERF_TIMER_H

#include <stdint.h>

enum { PT_0_60, PT_0_100, PT_QUARTER, PT_60_130, PT_COUNT };
enum { PT_IDLE, PT_ARMED, PT_LAUNCH, PT_RUNNING };

#define PT_MPH 1.609344
#define PT_QUARTER_M 402.336

typedef struct {
    int phase;
    double last_t, last_v;     /* previous sample */
    double stopped_since;      /* first 0 km/h sample of the current stop */
    double zero_t;             /* last 0 km/h sample before launch */
    double first_t, first_v;   /* first moving sample */
    double t0, distance, peak;
    int have_sample;
    /* rolling 60-130 */
    double roll_t0, roll_peak;
    int rolling;
    /* results: time in s (0 = none), trap speed for the quarter in km/h */
    double last[PT_COUNT], best[PT_COUNT], trap_last, trap_best;
    int done[PT_COUNT];        /* this run's targets reached */
    int new_best;              /* set when a result beats the best; caller clears */
    int updated;               /* set when any result was recorded; caller clears */
} pt_state;

static inline void pt_init(pt_state *s) {
    unsigned i;
    char *p = (char *)s;
    for (i = 0; i < sizeof *s; ++i) p[i] = 0;
}

static inline double pt_cross(double t1, double v1, double t2, double v2, double v) {
    if (v2 == v1) return t2;
    return t1 + (v - v1) / (v2 - v1) * (t2 - t1);
}

static inline void pt_record(pt_state *s, int which, double seconds) {
    s->last[which] = seconds;
    s->updated = 1;
    if (!s->best[which] || seconds < s->best[which]) { s->best[which] = seconds; s->new_best = 1; }
}

/* Running time of the current standing run, in s (0 when none). */
static inline double pt_elapsed(const pt_state *s, double now) {
    return s->phase == PT_RUNNING && now > s->t0 ? (now - s->t0) / 1000.0 : 0;
}

/* Feed one sample. A gap over 600 ms or a negative speed (no data) cancels runs. */
static inline void pt_sample(pt_state *s, double t, double v) {
    static const double standing_kph[2] = {60 * PT_MPH, 100 * PT_MPH};
    double lt = s->last_t, lv = s->last_v;
    if (v < 0 || (s->have_sample && t - lt > 600)) {
        s->phase = PT_IDLE;
        s->rolling = 0;
        s->have_sample = v >= 0;
        s->last_t = t; s->last_v = v < 0 ? 0 : v;
        if (v == 0) s->stopped_since = t;
        return;
    }
    if (!s->have_sample) {
        s->have_sample = 1;
        s->last_t = t; s->last_v = v;
        if (v == 0) s->stopped_since = t;
        return;
    }
    if (t <= lt) return;

    /* standing runs */
    switch (s->phase) {
    case PT_IDLE:
        if (v == 0) {
            if (lv != 0) s->stopped_since = t;
            if (t - s->stopped_since >= 500) s->phase = PT_ARMED;
        }
        break;
    case PT_ARMED:
        if (v > 0) {
            s->zero_t = lt;
            s->first_t = t; s->first_v = v;
            s->phase = PT_LAUNCH;
        }
        break;
    case PT_LAUNCH: {
        double a, t0;
        unsigned i;
        if (v <= 0) { s->phase = PT_IDLE; s->stopped_since = t; break; }
        a = (v - s->first_v) / (t - s->first_t);
        t0 = a > 0 ? s->first_t - s->first_v / a : (s->zero_t + s->first_t) / 2;
        if (t0 < s->zero_t) t0 = s->zero_t;
        if (t0 > s->first_t) t0 = s->first_t;
        s->t0 = t0;
        s->distance = s->first_v / 3.6 * (s->first_t - t0) / 2000.0 +
                      (s->first_v + v) / 3.6 * (t - s->first_t) / 2000.0;
        s->peak = v;
        for (i = 0; i < PT_COUNT; ++i) s->done[i] = 0;
        s->phase = PT_RUNNING;
        break;
    }
    case PT_RUNNING: {
        double d = (lv + v) / 3.6 * (t - lt) / 2000.0;
        unsigned i;
        if (v < s->peak - 8 || v == 0) { /* lifted or stopped: the run ends */
            s->phase = PT_IDLE;
            if (v == 0) s->stopped_since = t;
            break;
        }
        for (i = 0; i < 2; ++i)
            if (!s->done[i] && lv < standing_kph[i] && v >= standing_kph[i]) {
                s->done[i] = 1;
                pt_record(s, (int)i, (pt_cross(lt, lv, t, v, standing_kph[i]) - s->t0) / 1000.0);
            }
        if (!s->done[PT_QUARTER] && s->distance + d >= PT_QUARTER_M) {
            double frac = d > 0 ? (PT_QUARTER_M - s->distance) / d : 1;
            double tq = lt + frac * (t - lt);
            s->done[PT_QUARTER] = 1;
            s->trap_last = lv + frac * (v - lv);
            if (!s->best[PT_QUARTER] || (tq - s->t0) / 1000.0 < s->best[PT_QUARTER]) s->trap_best = s->trap_last;
            pt_record(s, PT_QUARTER, (tq - s->t0) / 1000.0);
        }
        s->distance += d;
        if (v > s->peak) s->peak = v;
        if (s->done[PT_0_100] && s->done[PT_QUARTER]) s->phase = PT_IDLE;
        break;
    }
    }

    /* rolling 60-130 */
    if (!s->rolling && lv < 60 * PT_MPH && v >= 60 * PT_MPH) {
        s->rolling = 1;
        s->roll_t0 = pt_cross(lt, lv, t, v, 60 * PT_MPH);
        s->roll_peak = v;
    } else if (s->rolling) {
        if (v < s->roll_peak - 5 || v < 60 * PT_MPH) s->rolling = 0;
        else {
            if (lv < 130 * PT_MPH && v >= 130 * PT_MPH) {
                pt_record(s, PT_60_130, (pt_cross(lt, lv, t, v, 130 * PT_MPH) - s->roll_t0) / 1000.0);
                s->rolling = 0;
            }
            if (v > s->roll_peak) s->roll_peak = v;
        }
    }
    s->last_t = t;
    s->last_v = v;
}

#endif
