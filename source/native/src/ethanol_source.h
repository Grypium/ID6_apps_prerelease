/* ethanol_source.h - which ethanol reading an app asks the hub for.
 *
 * Off unless <app dir>/ethanol-on exists. Its first word picks the source:
 *   obd   standard OBD PID 52 "ethanol fuel %" (a plain read; the default)
 *   mhd   MHD's monitor block (2C 88 01), as upstream BoostDeck reads it
 *   auto  OBD first, MHD if the DME doesn't answer PID 52
 * Returns the hub argument ("obd", "mhd", "auto") or 0 when off.
 */
#ifndef ETHANOL_SOURCE_H
#define ETHANOL_SOURCE_H

#include <stdio.h>
#include <string.h>

static inline const char *ethanol_mode(const char *path) {
    char word[16] = "";
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(word, sizeof word, file)) word[0] = 0; /* fgets: already imported by every app */
    fclose(file);
    word[strcspn(word, " \t\r\n")] = 0;
    if (!strcmp(word, "mhd")) return "mhd";
    if (!strcmp(word, "auto")) return "auto";
    return "obd";
}

#endif
