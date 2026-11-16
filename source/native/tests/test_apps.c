/* Host tests for the Fuel Blend math and the performance timers. */
#include "fuel_blend.h"
#include "perf_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* M240i-like pull: acceleration falls with speed (kph/s). */
static double accel(double v) { return v < 100 ? 23.5 - v * 0.05 : v < 170 ? 18.5 - (v - 100) * 0.08 : 12.9 - (v - 170) * 0.03; }

/* Exact times from fine integration. */
static void truth(double *t60, double *t100, double *tq, double *trap, double *t60_130) {
    double t = 0, v = 0, d = 0, dt = 0.0001, at60 = 0;
    *t60 = *t100 = *tq = *t60_130 = 0;
    while (v < 215) {
        double nv = v + accel(v) * dt;
        double nd = d + (v + nv) / 2 / 3.6 * dt;
        t += dt;
        if (!*t60 && nv >= 60 * PT_MPH) { *t60 = t; at60 = t; }
        if (!*t100 && nv >= 100 * PT_MPH) *t100 = t;
        if (!*tq && nd >= PT_QUARTER_M) { *tq = t; *trap = nv; }
        if (!*t60_130 && nv >= 130 * PT_MPH) *t60_130 = t - at60;
        v = nv; d = nd;
    }
}

/* Feeds the pull as the hub delivers it: integer km/h every ~50 ms with jitter,
 * after `stop_ms` stopped; launch at a random moment between two samples. */
static void feed_pull(pt_state *s, unsigned seed, double stop_ms, double lift_at_kph) {
    double t = 1000, v = 0, launch, now;
    srand(seed);
    for (now = t; now < t + stop_ms; now += 50) pt_sample(s, now, 0);
    launch = now + (rand() % 50);
    for (;; now += 45 + rand() % 11) {
        double el = (now - launch) / 1000.0;
        /* integrate the true profile up to `el` */
        double tt = 0, vv = 0, dt = 0.0005;
        if (el > 0) while (tt < el) { vv += accel(vv) * dt; tt += dt; }
        v = vv;
        if (lift_at_kph && v > lift_at_kph) { pt_sample(s, now, floor(v) - 20); return; }
        pt_sample(s, now, floor(v));
        if (v > 212) break;
    }
}

int main(void) {
    fb_plan p;
    pt_state s;
    double t60, t100, tq, trap, t60_130;
    unsigned seed;
    double worst60 = 0, worst100 = 0, worstq = 0, worst_roll = 0;

    /* Blend: 1/4 tank (3.425 of 13.7 gal) at E30, target E40, E85 really E80, pump gas E10. */
    p = fb_solve(13.7, 13.7 / 4, 30, 40, 80, 10);
    CHECK(p.status == FB_EXACT, "status %d", p.status);
    CHECK(fabs(p.result - 40) < 1e-9, "result %f", p.result);
    CHECK(fabs(p.e85_amount + p.gas_amount - 13.7 * 3 / 4) < 1e-9, "fills to full");
    CHECK(fabs(p.e85_amount - (40 * 13.7 - 3.425 * 30 - 10.275 * 10) / 70) < 1e-9, "e85 %f", p.e85_amount);
    /* Already above target even with pump gas only. */
    p = fb_solve(13.7, 12, 60, 30, 80, 10);
    CHECK(p.status == FB_TOO_LOW && p.e85_amount == 0 && p.result > 30, "too low %d %f", p.status, p.result);
    /* Not enough room for E85 to reach target. */
    p = fb_solve(13.7, 10, 10, 60, 80, 10);
    CHECK(p.status == FB_TOO_HIGH && fabs(p.gas_amount) < 1e-9 && p.result < 60, "too high %d %f", p.status, p.result);
    /* Empty tank, E85 at the pump = target: all E85. */
    p = fb_solve(13.7, 0, 0, 80, 80, 10);
    CHECK(p.status == FB_EXACT && fabs(p.e85_amount - 13.7) < 1e-9, "all e85 %f", p.e85_amount);
    CHECK(fb_solve(13.7, 2, 10, 30, 10, 10).status == FB_INVALID, "e85 must be richer than gas");
    /* Calibration: plan with E80 assumed, but the pump's E85 was really E72. */
    {
        double e = 0, in_tank = 13.7 / 4;
        fb_plan plan = fb_solve(13.7, in_tank, 30, 40, 80, 10);
        double measured = (in_tank * 30 + plan.e85_amount * 72 + plan.gas_amount * 10) / 13.7;
        CHECK(fb_calibrate(in_tank, 30, plan.e85_amount, plan.gas_amount, 10, measured, 1.5, &e) == FB_EXACT &&
              fabs(e - 72) < 1e-9, "calibrated %f", e);
        /* Round trip: re-planning with the calibrated value hits the target exactly. */
        plan = fb_solve(13.7, in_tank, 30, 40, e, 10);
        CHECK(fabs((in_tank * 30 + plan.e85_amount * 72 + plan.gas_amount * 10) / 13.7 - 40) < 1e-9, "round trip");
        CHECK(fb_calibrate(in_tank, 30, 1.0, 9.0, 10, 33, 1.5, &e) == FB_INVALID, "too little E85 to tell");
        CHECK(fb_calibrate(in_tank, 30, 5.0, 5.0, 10, 90, 1.5, &e) == FB_INVALID, "implausible result rejected");
    }

    /* Timers against the exact profile, 40 random launches. */
    truth(&t60, &t100, &tq, &trap, &t60_130);
    printf("truth: 0-60 %.3f  0-100 %.3f  1/4 %.3f @ %.1f mph  60-130 %.3f\n", t60, t100, tq, trap / PT_MPH, t60_130);
    for (seed = 1; seed <= 40; ++seed) {
        pt_init(&s);
        feed_pull(&s, seed, 1500, 0);
        if (fabs(s.last[PT_0_60] - t60) > worst60) worst60 = fabs(s.last[PT_0_60] - t60);
        if (fabs(s.last[PT_0_100] - t100) > worst100) worst100 = fabs(s.last[PT_0_100] - t100);
        if (fabs(s.last[PT_QUARTER] - tq) > worstq) worstq = fabs(s.last[PT_QUARTER] - tq);
        if (fabs(s.last[PT_60_130] - t60_130) > worst_roll) worst_roll = fabs(s.last[PT_60_130] - t60_130);
        CHECK(s.last[PT_0_60] > 0 && s.last[PT_0_100] > 0 && s.last[PT_QUARTER] > 0 && s.last[PT_60_130] > 0, "all recorded (seed %u)", seed);
        CHECK(fabs(s.trap_last - trap) < 2.5, "trap %f vs %f", s.trap_last, trap);
    }
    printf("worst error over 40 launches: 0-60 %.3f s, 0-100 %.3f s, 1/4 %.3f s, 60-130 %.3f s\n", worst60, worst100, worstq, worst_roll);
    CHECK(worst60 < 0.10 && worst100 < 0.10 && worstq < 0.15 && worst_roll < 0.10, "accuracy");

    /* Not armed without a stop of at least 500 ms. */
    pt_init(&s);
    feed_pull(&s, 7, 300, 0);
    CHECK(s.last[PT_0_60] == 0, "no standing run without arming");
    CHECK(s.last[PT_60_130] > 0, "rolling run still timed");

    /* Lifting before 60 mph: nothing recorded, best untouched. */
    pt_init(&s);
    feed_pull(&s, 3, 1000, 80);
    CHECK(s.last[PT_0_60] == 0 && !s.updated, "lift aborts");

    /* Best keeps the fastest; signal loss cancels the run. */
    pt_init(&s);
    feed_pull(&s, 5, 1000, 0);
    {
        double best = s.best[PT_0_60];
        pt_sample(&s, s.last_t + 50, 0);
        CHECK(s.best[PT_0_60] == best, "best kept");
        pt_init(&s);
        pt_sample(&s, 0, 0); pt_sample(&s, 600, 0); pt_sample(&s, 650, 5); pt_sample(&s, 700, 10);
        CHECK(s.phase == PT_RUNNING, "running");
        pt_sample(&s, 1500, 40); /* 800 ms gap */
        CHECK(s.phase == PT_IDLE, "signal gap cancels");
        pt_sample(&s, 1550, -1);
        CHECK(s.phase == PT_IDLE, "no data cancels");
    }
    if (!failures) puts("Apps: fuel blend math and performance timers passed.");
    return failures != 0;
}
