/*
 * decls.h
 * 
 * Pull in all other header files in an orderly fashion. Source files include
 * only this header, and only once.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>

#include "build_enums.h"
/* pico-sdk headers come first, ahead of FlashFloppy's utility macros. */
#include "mcu/rp2350_sdk.h"
#include "types.h"
#include "mcu/common_regs.h"
#include "mcu/common.h"
#include "mcu/rp2350.h"
#include "intrinsics.h"

#include "time.h"
#include "../src/fatfs/ff.h"
#include "util.h"
#include "list.h"
#include "cache.h"
#include "da.h"
#include "hxc.h"
#include "cancellation.h"
#include "spi.h"
#include "timer.h"
#include "fs.h"
#include "floppy.h"
#include "volume.h"
#include "config.h"
#include "osd.h"
