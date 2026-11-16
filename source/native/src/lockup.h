/* lockup.h - gear and torque converter state for ShiftDeck.
 *
 * Converter slip = engine rpm - transmission input (turbine) rpm. Locked when
 * the slip stays near zero. Engine rpm (OBD) and turbine rpm (EGS) are read up
 * to ~80 ms apart, so the slip is the median of the last three samples and the
 * locked state has hysteresis. Header-only so the host tests and the plugin
 * share it.
 */
#ifndef LOCKUP_H
#define LOCKUP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LK_LOCK_RPM = 40, LK_UNLOCK_RPM = 90, LK_MIN_RPM = 1000 };
#define LK_NONE ((int32_t)0x80000000u)

typedef struct {
    int locked;
    int slip[3];
    unsigned count;
} lk_state;

static inline int lk_median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

/* Feeds one sample; writes the gear ("Gear 4", "Reverse", "Gear --") and the
   converter line ("Locked", "Slip 250 rpm", "Open", "--"). */
static inline void lk_update(lk_state *s, int32_t rpm, int32_t turbine, int32_t gear,
                             char *gear_text, size_t gear_size, char *conv_text, size_t conv_size) {
    int slip, abs_slip;
    if (gear >= 1 && gear <= 8) snprintf(gear_text, gear_size, "Gear %d", (int)gear);
    else if (gear == -1) snprintf(gear_text, gear_size, "Reverse");
    else snprintf(gear_text, gear_size, "Gear --");
    if (rpm == LK_NONE || turbine == LK_NONE || gear == 0) {
        s->count = 0;
        s->locked = 0;
        snprintf(conv_text, conv_size, "--");
        return;
    }
    if (rpm < LK_MIN_RPM) { /* idle / pulling away: the converter is open */
        s->count = 0;
        s->locked = 0;
        snprintf(conv_text, conv_size, "Open");
        return;
    }
    s->slip[s->count % 3] = (int)(rpm - turbine);
    ++s->count;
    if (s->count < 3) slip = (int)(rpm - turbine);
    else slip = lk_median3(s->slip[0], s->slip[1], s->slip[2]);
    abs_slip = slip < 0 ? -slip : slip;
    s->locked = s->locked ? abs_slip < LK_UNLOCK_RPM : abs_slip < LK_LOCK_RPM;
    if (s->locked) snprintf(conv_text, conv_size, "Locked");
    else snprintf(conv_text, conv_size, "Slip %d rpm", abs_slip);
}

#endif
