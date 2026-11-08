/*
 * rp2350_sdk.h
 *
 * pico-sdk headers used by the RP2350 port. Included by decls.h ahead of
 * every FlashFloppy header, so that FlashFloppy's short utility macros
 * (m(), min(), max(), ...) cannot collide with identifiers inside the SDK.
 *
 * FlashFloppy builds with a stricter warning set than the SDK is written
 * for (-Wundef, -Wstrict-prototypes, ... with -Werror), so those warnings
 * are relaxed for the SDK headers only.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wnested-externs"
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

#include "pico.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/m33.h"
#include "hardware/structs/sio.h"

#pragma GCC diagnostic pop

/* hardware/flash.h defines FLASH_PAGE_SIZE as the 256-byte program page.
 * FlashFloppy's FLASH_PAGE_SIZE is the erase unit (FLASH_SECTOR_SIZE on
 * RP2350), defined in mcu/common.h. */
#undef FLASH_PAGE_SIZE
