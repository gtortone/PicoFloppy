/*
 * spi.c
 * 
 * Helper functions for the RP2350 SPI interfaces (pico-sdk).
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */


/* pico-sdk back end: the SPI handle is an SDK spi_inst_t (spi0/spi1). */
#define sdk_spi(spi) ((spi_inst_t *)(spi))

void spi_quiesce(SPI spi)
{
    spi_inst_t *s = sdk_spi(spi);
    while (!(spi_get_const_hw(s)->sr & SPI_SSPSR_TFE_BITS) || spi_is_busy(s))
        cpu_relax();
    while (spi_is_readable(s))
        (void)spi_get_hw(s)->dr; /* drain the rx fifo */
}

static void sdk_spi_set_bits(SPI spi, unsigned int bits)
{
    /* SPI mode 0, MSB first (as configured by spi_init()). */
    spi_set_format(sdk_spi(spi), bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void spi_16bit_frame(SPI spi)
{
    spi_quiesce(spi);
    sdk_spi_set_bits(spi, 16);
}

void spi_8bit_frame(SPI spi)
{
    spi_quiesce(spi);
    sdk_spi_set_bits(spi, 8);
}

void spi_xmit16(SPI spi, uint16_t out)
{
    spi_inst_t *s = sdk_spi(spi);
    while (!spi_is_writable(s))
        cpu_relax();
    spi_get_hw(s)->dr = out;
}

uint16_t spi_xchg16(SPI spi, uint16_t out)
{
    spi_inst_t *s = sdk_spi(spi);
    while (!spi_is_writable(s))
        cpu_relax();
    spi_get_hw(s)->dr = out;
    while (!spi_is_readable(s))
        continue;
    return spi_get_hw(s)->dr;
}
