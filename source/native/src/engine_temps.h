/* engine_temps.h - warm-up and heat guidance for Engine Temps (B58).
 * Temperatures in degrees C. Header-only so host tests and the plugin share it.
 * Thresholds are conservative owner guidance, not BMW limits: the car's own
 * variable redline and warnings always take precedence. */
#ifndef ENGINE_TEMPS_H
#define ENGINE_TEMPS_H

enum { ET_NO_DATA, ET_COLD, ET_WARMING, ET_NEARLY, ET_WARM, ET_HOT };

#define ET_OIL_COLD 60.0    /* below: cold */
#define ET_OIL_WARMING 80.0 /* below: warming up */
#define ET_OIL_READY 90.0   /* at or above: full performance */
#define ET_OIL_HOT 130.0
#define ET_COOLANT_HOT 120.0
#define ET_TRANS_HOT 120.0
#define ET_INTAKE_HOT 60.0

static inline int et_oil_stage(int valid, double oil) {
    if (!valid) return ET_NO_DATA;
    if (oil >= ET_OIL_HOT) return ET_HOT;
    if (oil >= ET_OIL_READY) return ET_WARM;
    if (oil >= ET_OIL_WARMING) return ET_NEARLY;
    if (oil >= ET_OIL_COLD) return ET_WARMING;
    return ET_COLD;
}

static inline const char *et_guide(int stage) {
    switch (stage) {
    case ET_COLD: return "Oil cold: drive gently and keep it under 3,500 rpm.";
    case ET_WARMING: return "Oil warming up: under 4,500 rpm, no full throttle yet.";
    case ET_NEARLY: return "Oil nearly warm: normal driving is fine, wait a little for full throttle.";
    case ET_WARM: return "Oil warm: ready for full performance.";
    case ET_HOT: return "Oil hot: ease off and let it cool.";
    default: return "Waiting for engine data... (ignition on)";
    }
}

static inline double et_fahrenheit(double c) { return c * 1.8 + 32; }

#endif
