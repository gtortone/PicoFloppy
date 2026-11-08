/*
 * fpec_rp2350.c
 *
 * RP2350 QSPI flash programming via the pico-sdk (hardware_flash). Presents
 * the same halfword-granular write interface as the STM32 FPEC: sub-page
 * writes are implemented as read-modify-write of 256-byte flash program
 * pages (only 1->0 bit transitions, as guaranteed by the flash_cfg layer).
 *
 * All firmware code and data execute from SRAM (a copy_to_ram binary), so
 * XIP may be safely suspended for the duration of a flash operation; the
 * SDK's flash routines are themselves RAM-resident. Interrupts are kept off
 * meanwhile, as an interrupt handler might otherwise touch the XIP window.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define FLASH_PROG_PAGE 256 /* the SDK's (program) FLASH_PAGE_SIZE */

void fpec_init(void)
{
}

void fpec_page_erase(uint32_t flash_address)
{
    uint32_t off = flash_address - XIP_BASE;
    uint32_t ints;

    ASSERT(!(off & (FLASH_PAGE_SIZE-1)));

    ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

void fpec_write(const void *data, unsigned int size, uint32_t flash_address)
{
    const uint8_t *d = data;
    uint8_t page[FLASH_PROG_PAGE];
    uint32_t ints;

    while (size != 0) {
        uint32_t page_addr = flash_address & ~(FLASH_PROG_PAGE-1);
        unsigned int page_off = flash_address & (FLASH_PROG_PAGE-1);
        unsigned int nr = min_t(unsigned int, size, FLASH_PROG_PAGE-page_off);

        /* Read-modify-write of one flash page. */
        memcpy(page, (void *)page_addr, FLASH_PROG_PAGE);
        memcpy(&page[page_off], d, nr);

        ints = save_and_disable_interrupts();
        flash_range_program(page_addr - XIP_BASE, page, FLASH_PROG_PAGE);
        restore_interrupts(ints);

        d += nr;
        size -= nr;
        flash_address += nr;
    }
}
