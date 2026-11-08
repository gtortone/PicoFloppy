/*
 * pico2/sdk_glue.c
 *
 * C-library stubs for the pico-sdk runtime. Built against the pico-sdk only
 * (no FlashFloppy headers).
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include <time.h>
#include "pico.h"

/* FlashFloppy keeps no calendar time, and nothing calls the SDK's calendar
 * helpers (pico_util/datetime.c). Those still refer to mktime() and
 * localtime_r(), which would otherwise drag newlib's time zone and scanf
 * code into an image that lives in SRAM (and newlib's strtol() alongside
 * FlashFloppy's own). */

struct tm *localtime_r(const time_t *time, struct tm *tm)
{
    (void)time;
    (void)tm;
    return NULL;
}

time_t mktime(struct tm *tm)
{
    (void)tm;
    return (time_t)-1;
}
