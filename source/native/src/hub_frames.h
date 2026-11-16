#ifndef HUB_FRAMES_H
#define HUB_FRAMES_H

#include <stdint.h>

/* Extra hub output for the rebuilt BoostDeck, ShiftDeck and PerfTimers.
   Every value is a fixed-point int32; HUB_NONE = not available right now.
   The official plugins never see these: they keep their own frames. */
#define HUB_NONE ((int32_t)0x80000000u)

/* BoostDeck third row and Active Ethanol, about three times a second. */
enum { DECK_PORT = 42867, DECK_STALE_MS = 1500 };
struct deck_frame {
    uint64_t sampled_ms;
    int32_t oil_temp;  /* 0.1 C, DME 4408 */
    int32_t torque;    /* 0.1 Nm, OBD 62 x reference torque (OBD 63) */
    int32_t power;     /* 0.1 kW, torque x rpm */
    int32_t throttle;  /* 0.1 %, OBD 11 */
    int32_t ethanol;   /* 0.1 %, MHD active ethanol (only with --ethanol) */
};

/* ShiftDeck: gear and torque converter, five times a second. */
enum { GEAR_PORT = 42868, GEAR_STALE_MS = 1000 };
struct gear_frame {
    uint64_t sampled_ms;
    int32_t rpm;       /* engine rpm (OBD 0C), HUB_NONE if unknown */
    int32_t turbine;   /* transmission input (turbine) rpm, EGS DA34 */
    int32_t gear;      /* 1..8, -1 reverse, 0 unknown (EGS DA2E) */
};

/* PerfTimers: speed from the four wheel sensors (EGS DA1D), every 60 ms.
   front = mean of the two slowest wheels, driven = mean of the two fastest.
   On a rear-drive car that is the undriven fronts vs the spinning rears. */
enum { TIMER_FRAME_STALE_MS = 750 };
struct timer_frame {
    uint64_t sampled_ms;
    int32_t kph;       /* OBD speed, whole km/h (fallback), -1 unknown */
    int32_t front;     /* 0.01 km/h, HUB_NONE if the wheel read failed */
    int32_t driven;    /* 0.01 km/h */
};

/* FuelBlend: tank level (OBD 2F, % of tank), every two seconds. */
enum { FUEL_PORT = 42869, FUEL_STALE_MS = 6000 };
struct fuel_frame {
    uint64_t sampled_ms;
    int32_t level;     /* 0.1 % of the tank, HUB_NONE if unknown */
};

/* EGS DA1D: four words, (raw - 0x7FFF) x WHEEL_KPH km/h; sign follows the
   sensor, so magnitudes are used. Scale fitted against DSC DBE4 (/100 km/h)
   on a low-speed drive: per-wheel medians 0.01724..0.01732. */
#define WHEEL_KPH 0.01728

/* Sorts four wheel speeds (km/h, any sign) into front/driven averages. */
static inline void hub_wheels(const double w[4], double *front, double *driven) {
    double a[4];
    unsigned i, j;
    for (i = 0; i < 4; ++i) a[i] = w[i] < 0 ? -w[i] : w[i];
    for (i = 1; i < 4; ++i)
        for (j = i; j > 0 && a[j - 1] > a[j]; --j) { double t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    *front = (a[0] + a[1]) / 2;
    *driven = (a[2] + a[3]) / 2;
}

#endif
