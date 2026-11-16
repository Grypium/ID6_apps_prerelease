/* lockup.h - gear and torque converter state for ShiftDeck.
 *
 * Converter slip = engine rpm - transmission input (turbine) rpm. The hub pairs
 * the two readings in time; what is left is noise of a few tens of rpm and
 * short spikes while a shift is under way (one or two samples, logged on the
 * car). Real slip that matters lasts, so the slip shown is the smallest of the
 * last three samples, with hysteresis on the locked state. Feed one call per
 * new pair. Header-only so the host tests and the plugin share it.
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

static inline int lk_abs(int v) { return v < 0 ? -v : v; }

static inline void lk_gear_text(int32_t gear, char *gear_text, size_t gear_size) {
    if (gear >= 1 && gear <= 8) snprintf(gear_text, gear_size, "Gear %d", (int)gear);
    else if (gear == -1) snprintf(gear_text, gear_size, "Reverse");
    else snprintf(gear_text, gear_size, "Gear --");
}

/* Feeds one sample; writes the gear ("Gear 4", "Reverse", "Gear --") and the
   converter line ("Locked", "Slip 250 rpm", "Open", "--"). */
static inline void lk_update(lk_state *s, int32_t rpm, int32_t turbine, int32_t gear,
                             char *gear_text, size_t gear_size, char *conv_text, size_t conv_size) {
    int abs_slip;
    lk_gear_text(gear, gear_text, gear_size);
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
    s->slip[s->count % 3] = lk_abs((int)(rpm - turbine));
    ++s->count;
    abs_slip = s->slip[(s->count - 1) % 3];
    if (s->count >= 3) {
        unsigned i;
        for (i = 0; i < 3; ++i) if (s->slip[i] < abs_slip) abs_slip = s->slip[i];
    }
    s->locked = s->locked ? abs_slip < LK_UNLOCK_RPM : abs_slip < LK_LOCK_RPM;
    if (s->locked) snprintf(conv_text, conv_size, "Locked");
    else snprintf(conv_text, conv_size, "Slip %d rpm", abs_slip);
}

#endif
