# CMakeLists.txt
#
# PicoFloppy: FlashFloppy for the Raspberry Pi Pico 2 (RP2350), built on the
# pico-sdk.
#
#   cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk [-DFF_LEVEL=debug]
#   cmake --build build
#
# FF_LEVEL is one of prod (default), debug or logfile. The result is
# build/picofloppy.uf2 (and .elf/.bin). scripts/mk_dist.sh builds all three
# levels into a release zip.
#
# This is free and unencumbered software released into the public domain.
# See the file COPYING for more details, or visit <http://unlicense.org>.

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(picofloppy C CXX ASM)

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.2.0")
    message(FATAL_ERROR "pico-sdk 2.2.0 or later is required "
                        "(found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

set(FF_LEVEL prod CACHE STRING "Build level: prod, debug or logfile")
set_property(CACHE FF_LEVEL PROPERTY STRINGS prod debug logfile)
if(NOT FF_LEVEL MATCHES "^(prod|debug|logfile)$")
    message(FATAL_ERROR "FF_LEVEL must be prod, debug or logfile")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Firmware version, as shown at boot: -DFF_FW_VER=... (release builds pass
# the release version), else the short git hash.
if(NOT FF_FW_VER)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE FF_FW_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT FF_FW_VER)
    set(FF_FW_VER "unknown")
endif()

set(FF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(FF_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(FF_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${FF_GEN})

# Generated source: FF.CFG defaults.
add_custom_command(
    OUTPUT ${FF_GEN}/ff_cfg_defaults.h
    COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mk_config.py
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/FF.CFG
            ${FF_GEN}/ff_cfg_defaults.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mk_config.py
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/FF.CFG)

# FlashFloppy sources (Shugart interface). These are compiled with the usual
# FlashFloppy flags: every file implicitly includes decls.h.
set(FF_SOURCES
    ${FF_SRC}/arena.c
    ${FF_SRC}/build_info.c
    ${FF_SRC}/cache.c
    ${FF_SRC}/cancellation.c
    ${FF_SRC}/config.c
    ${FF_SRC}/cortex.c
    ${FF_SRC}/crc.c
    ${FF_SRC}/eject_menu.c
    ${FF_SRC}/ff_cfg_read.c
    ${FF_SRC}/flash_cfg.c
    ${FF_SRC}/floppy.c
    ${FF_SRC}/fpec_rp2350.c
    ${FF_SRC}/fs.c
    ${FF_SRC}/main.c
    ${FF_SRC}/main_menu.c
    ${FF_SRC}/mcu_rp2350.c
    ${FF_SRC}/nav.c
    ${FF_SRC}/printf.c
    ${FF_SRC}/sd_spi.c
    ${FF_SRC}/spi.c
    ${FF_SRC}/time.c
    ${FF_SRC}/timer.c
    ${FF_SRC}/ui_buttons.c
    ${FF_SRC}/ui_display.c
    ${FF_SRC}/util.c
    ${FF_SRC}/volume.c
    ${FF_SRC}/display/display.c
    ${FF_SRC}/display/lcd_rp2350.c
    ${FF_SRC}/display/led_7seg.c
    ${FF_SRC}/fatfs/ff.c
    ${FF_SRC}/fatfs/ffunicode.c
    ${FF_SRC}/image/adf.c
    ${FF_SRC}/image/da.c
    ${FF_SRC}/image/dsk.c
    ${FF_SRC}/image/dummy.c
    ${FF_SRC}/image/hfe.c
    ${FF_SRC}/image/image.c
    ${FF_SRC}/image/img.c
    ${FF_SRC}/image/mfm.c
    ${FF_SRC}/pico2/board.c
    ${FF_SRC}/pico2/flash_vol.c
    ${FF_SRC}/pico2/osd.c
    ${FF_SRC}/pico2/osd_amikbd.c
    ${FF_SRC}/pico2/osd_font.c
    ${FF_SRC}/pico2/osd_menu.c
    ${FF_SRC}/pico2/speaker.c)
if(FF_LEVEL STREQUAL "debug")
    list(APPEND FF_SOURCES ${FF_SRC}/console.c)
elseif(FF_LEVEL STREQUAL "logfile")
    list(APPEND FF_SOURCES ${FF_SRC}/logfile.c)
endif()

# Platform glue built purely against the pico-sdk and libraries (no
# FlashFloppy headers).
set(SDK_SOURCES
    ${FF_SRC}/display/oled_fonts.c
    ${FF_SRC}/pico2/sdk_glue.c
    ${FF_SRC}/pico2/usb_cdc.c
    ${FF_SRC}/pico2/usb_descriptors.c)

add_executable(picofloppy ${FF_SOURCES} ${SDK_SOURCES})

# u8g2 graphics library (OLED displays), fetched from git at configure time
# (https://github.com/olikraus/u8g2, pinned to a release tag). Only the files
# the firmware uses are checked out: a shallow, sparse, blob-less clone of
# about 1.5MB rather than the whole 400MB repository (mostly fonts; the
# firmware brings its own, see src/display/oled_fonts.c). For an offline
# build, point U8G2_SOURCE_DIR at an existing u8g2 checkout.
#
# To support another display controller, add its u8x8_d_*.c file to
# U8G2_FILES. Built with the SDK's flags, not FlashFloppy's.
set(U8G2_GIT_URL https://github.com/olikraus/u8g2.git CACHE STRING
    "u8g2 git repository")
set(U8G2_GIT_TAG 2.37.1 CACHE STRING "u8g2 release (git tag)")
set(U8G2_SOURCE_DIR "" CACHE PATH
    "Existing u8g2 checkout to build from, instead of fetching it")
set(U8G2_FILES
    u8g2_arc.c u8g2_bitmap.c u8g2_box.c u8g2_buffer.c u8g2_circle.c
    u8g2_cleardisplay.c u8g2_font.c u8g2_hvline.c u8g2_intersection.c
    u8g2_kerning.c u8g2_line.c u8g2_ll_hvline.c u8g2_polygon.c u8g2_setup.c
    u8x8_8x8.c u8x8_byte.c u8x8_cad.c u8x8_capture.c u8x8_display.c
    u8x8_gpio.c u8x8_setup.c u8x8_string.c u8x8_u16toa.c u8x8_u8toa.c
    # Display drivers: SSD1306 and SH1106, 128x64 and 128x32.
    u8x8_d_ssd1306_128x64_noname.c
    u8x8_d_ssd1306_128x32.c)

if(U8G2_SOURCE_DIR)
    set(U8G2_DIR ${U8G2_SOURCE_DIR})
else()
    find_package(Git REQUIRED)
    set(U8G2_DIR ${CMAKE_BINARY_DIR}/_deps/u8g2)
    set(U8G2_STAMP ${U8G2_DIR}/.picofloppy-${U8G2_GIT_TAG})
    if(NOT EXISTS ${U8G2_STAMP})
        message(STATUS "Fetching u8g2 ${U8G2_GIT_TAG} from ${U8G2_GIT_URL}")
        file(REMOVE_RECURSE ${U8G2_DIR})
        set(U8G2_PATTERNS /LICENSE /csrc/u8g2.h /csrc/u8x8.h)
        foreach(f ${U8G2_FILES})
            list(APPEND U8G2_PATTERNS /csrc/${f})
        endforeach()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --quiet --depth 1
                    --branch ${U8G2_GIT_TAG} --filter=blob:none --no-checkout
                    ${U8G2_GIT_URL} ${U8G2_DIR}
            RESULT_VARIABLE rc)
        if(rc EQUAL 0)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${U8G2_DIR} sparse-checkout set
                        --no-cone ${U8G2_PATTERNS}
                RESULT_VARIABLE rc)
        endif()
        if(rc EQUAL 0)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${U8G2_DIR} checkout --quiet
                RESULT_VARIABLE rc)
        endif()
        if(NOT rc EQUAL 0)
            file(REMOVE_RECURSE ${U8G2_DIR})
            message(FATAL_ERROR "Could not fetch u8g2 ${U8G2_GIT_TAG} from "
                "${U8G2_GIT_URL} (git 2.35+ needed for a sparse clone). "
                "Set U8G2_SOURCE_DIR to a local u8g2 checkout instead.")
        endif()
        file(TOUCH ${U8G2_STAMP})
    endif()
endif()

set(U8G2_SOURCES)
foreach(f ${U8G2_FILES})
    list(APPEND U8G2_SOURCES ${U8G2_DIR}/csrc/${f})
endforeach()
add_library(u8g2 STATIC ${U8G2_SOURCES})
target_include_directories(u8g2 PUBLIC ${U8G2_DIR}/csrc)
target_link_libraries(u8g2 PRIVATE pico_platform)
target_compile_options(u8g2 PRIVATE -ffunction-sections -fdata-sections)

# FlashFloppy compile flags, applied to its own sources only:
# target-wide options would also reach the SDK's sources.
set(FF_CFLAGS
    -Os -std=gnu11 -ffreestanding
    -Wall -Werror -Wno-format -Wdeclaration-after-statement
    -Wstrict-prototypes -Wredundant-decls -Wnested-externs
    -fno-common -fno-exceptions -fno-strict-aliasing
    -Wno-unused-value -ffunction-sections -Wundef
    -DLEVEL=LEVEL_${FF_LEVEL}
    # The C library headers which the SDK pulls in should declare ISO C
    # only: BSD extras such as index() collide with FlashFloppy names.
    -D_ISOC99_SOURCE
    -iquote${FF_INC} -iquote${FF_GEN} -includedecls.h)
set_source_files_properties(${FF_SOURCES} PROPERTIES COMPILE_OPTIONS
    "${FF_CFLAGS}")
set_source_files_properties(${FF_SRC}/build_info.c PROPERTIES
    COMPILE_DEFINITIONS "FW_VER=\"${FF_FW_VER}\"")
# Avoid GCC turning our memcpy/memset loops into calls to themselves.
set_source_files_properties(${FF_SRC}/util.c PROPERTIES
    COMPILE_FLAGS "-fno-tree-loop-distribute-patterns")
set_source_files_properties(${FF_SRC}/ff_cfg_read.c ${FF_SRC}/flash_cfg.c
    PROPERTIES OBJECT_DEPENDS ${FF_GEN}/ff_cfg_defaults.h)

# The PIO flux engine (RDATA generator, WDATA capture).
pico_generate_pio_header(picofloppy ${FF_SRC}/pico2/flux.pio
    OUTPUT_DIR ${FF_GEN})
# The FF OSD engine (sync measurement, video output, Amiga keyboard).
pico_generate_pio_header(picofloppy ${FF_SRC}/pico2/osd.pio
    OUTPUT_DIR ${FF_GEN})
# ...whose timing is checked statically against the STM32 flux encoding.
add_custom_command(TARGET picofloppy PRE_LINK
    COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_pio_timing.py
            ${FF_GEN}/flux.pio.h
    VERBATIM)

# TinyUSB configuration (tusb_config.h) for the USB CDC console.
target_include_directories(picofloppy PRIVATE ${FF_SRC}/pico2)

target_compile_definitions(picofloppy PRIVATE
    # clk_sys = 144MHz: 12MHz * 120 = 1440MHz VCO, / 5 / 2. Two PIO cycles
    # per tick then reproduce the STM32 firmware's 72MHz SAMPLECLK exactly.
    SYS_CLK_HZ=144000000
    PLL_SYS_VCO_FREQ_HZ=1440000000
    PLL_SYS_POSTDIV1=5
    PLL_SYS_POSTDIV2=2
    # clk_peri stays on clk_sys (UART/SPI baud maths use clock_get_hz()).
    # Main (thread) stack: 4kB, the whole of SCRATCH_Y.
    PICO_STACK_SIZE=0x1000
    # FlashFloppy runs its own interrupt priorities and never blocks in
    # SDK sleep/alarm code; no default alarm pool is needed.
    PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1)

target_link_libraries(picofloppy
    pico_runtime
    pico_bootrom
    pico_multicore
    pico_unique_id
    hardware_clocks
    hardware_dma
    hardware_flash
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_spi
    hardware_sync
    hardware_timer
    hardware_watchdog
    tinyusb_device
    u8g2)

# FlashFloppy's linker symbols, mapped onto the SDK's layout:
#  - the thread stack is the SDK's main stack (SCRATCH_Y);
#  - _smaintext/_emaintext bound the code for crash-dump stack scans.
target_link_options(picofloppy PRIVATE
    -Wl,--defsym=_thread_stackbottom=__StackBottom
    -Wl,--defsym=_thread_stacktop=__StackTop
    -Wl,--defsym=_smaintext=__ram_text_start__
    -Wl,--defsym=_emaintext=__ram_text_end__)

# Run entirely from SRAM: QSPI (XIP) timing can never disturb the flux
# engine, and flash can be reprogrammed without special care.
pico_set_binary_type(picofloppy copy_to_ram)

# No pico_stdio or pico_printf: FlashFloppy has its own console (USB CDC) and its own printf family (printf.c), which must not be wrapped.
pico_set_printf_implementation(picofloppy compiler)

pico_add_extra_outputs(picofloppy)

# Memory usage summary (per linker region) at the end of the link.
set_property(TARGET picofloppy APPEND_STRING PROPERTY
    LINK_FLAGS " -Wl,--print-memory-usage")
