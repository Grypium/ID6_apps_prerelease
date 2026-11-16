#ifndef TEMPS_FRAME_H
#define TEMPS_FRAME_H

#include <stdint.h>

/* Engine Temps: the shared hub sends one of these to TEMPS_PORT about once a
   second. Temperatures are in tenths of a degree C; TEMPS_NONE = unavailable. */
enum { TEMPS_PORT = 42866, TEMPS_STALE_MS = 3500 };
#define TEMPS_NONE ((int32_t)0x80000000u)

struct temps_frame {
    uint64_t sampled_ms;
    int32_t oil, coolant, intake, trans;
};

#endif
