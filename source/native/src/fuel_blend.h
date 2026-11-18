/* fuel_blend.h - how much E85 and pump gas to add for a target ethanol blend.
 *
 * Filling to full: the tank holds `capacity`; `in_tank` of it is already fuel
 * at `current` % ethanol. Pump `e85_amount` of E85 (really `e85` % ethanol)
 * first, then fill the rest with pump gas at `gas` % ethanol. Pure arithmetic,
 * no car access; header-only so the host tests and the plugin share it.
 */
#ifndef FUEL_BLEND_H
#define FUEL_BLEND_H

enum { FB_EXACT = 0, FB_TOO_LOW = 1, FB_TOO_HIGH = 2, FB_INVALID = 3 };

typedef struct {
    double e85_amount;  /* add this much E85 first */
    double gas_amount;  /* then this much pump gas (to full) */
    double result;      /* blend after the fill, % ethanol */
    int status;         /* FB_EXACT, or why the target can't be reached on this fill */
} fb_plan;

static inline fb_plan fb_solve(double capacity, double in_tank, double current, double target,
                               double e85, double gas) {
    fb_plan p;
    double room, x;
    p.e85_amount = p.gas_amount = 0;
    p.result = current;
    p.status = FB_INVALID;
    if (capacity <= 0 || in_tank < 0 || in_tank > capacity || e85 <= gas) return p;
    room = capacity - in_tank;
    /* (in_tank*current + x*e85 + (room-x)*gas) / capacity = target */
    x = (target * capacity - in_tank * current - room * gas) / (e85 - gas);
    p.status = FB_EXACT;
    if (x < 0) { x = 0; p.status = FB_TOO_LOW; }        /* already richer than target, even topping up with gas */
    if (x > room) { x = room; p.status = FB_TOO_HIGH; } /* not enough room for E85 to get there */
    p.e85_amount = x;
    p.gas_amount = room - x;
    p.result = (in_tank * current + x * e85 + (room - x) * gas) / capacity;
    return p;
}

/* Back-solves the pump E85's real ethanol content from a fill and the blend
 * measured afterwards (ethanol sensor or test kit):
 *   measured = (in_tank*before + e85_added*E + gas_added*gas) / (in_tank + e85_added + gas_added)
 * Returns FB_EXACT with *e85 set, or FB_INVALID when the fill had too little E85
 * (under min_e85) for a reliable answer or the result is implausible (outside 40..100). */
static inline int fb_calibrate(double in_tank, double before, double e85_added, double gas_added,
                               double gas, double measured, double min_e85, double *e85) {
    double total = in_tank + e85_added + gas_added, e;
    if (e85_added < min_e85 || e85_added <= 0 || total <= 0) return FB_INVALID;
    e = (measured * total - in_tank * before - gas_added * gas) / e85_added;
    if (e < 40 || e > 100) return FB_INVALID;
    *e85 = e;
    return FB_EXACT;
}

/* Ethanol sensor settling: a reading is "settled" once it has stayed within
 * FB_SETTLE_BAND for FB_SETTLE_MS. After a fill the sensor first reads the old
 * fuel still in the lines, then moves to the new blend. */
/* OBD 2F (% of tank) on this F22 does not span the tank: brim full reads
 * 83.1 %, and a fill of 9.0 gal (5.9 E85 + 3.1 gal 91) went in from a 23.4 %
 * reading. A straight line through both points puts 0 % at about 1.2 gal
 * still in the tank. Fitted from one fill (2026-11-18); refine with later ones. */
#define FB_LEVEL_FULL_PCT 83.1
#define FB_LEVEL_ZERO_PCT (-7.84)  /* where the line reaches an empty tank */
static inline double fb_level_gallons(double pct, double tank) {
    double g = tank * (pct - FB_LEVEL_ZERO_PCT) / (FB_LEVEL_FULL_PCT - FB_LEVEL_ZERO_PCT);
    return g < 0 ? 0 : g > tank ? tank : g;
}

enum { FB_SETTLE_MS = 30000 };
#define FB_SETTLE_BAND 1.5
#define FB_SETTLE_USED 0.5 /* gallons burned since the fill before trusting the sensor */

typedef struct { double low, high; unsigned long long since; int started; } fb_settle;

static inline int fb_settle_update(fb_settle *s, double value, unsigned long long now_ms) {
    if (!s->started || value < s->low - 1e-9 || value > s->high + 1e-9) {
        double low = s->started && value > s->low ? s->low : value;
        double high = s->started && value < s->high ? s->high : value;
        if (!s->started || high - low > FB_SETTLE_BAND) {
            s->low = s->high = value;
            s->since = now_ms;
            s->started = 1;
        } else {
            s->low = low;
            s->high = high;
        }
    }
    return now_ms - s->since >= FB_SETTLE_MS;
}

#endif
