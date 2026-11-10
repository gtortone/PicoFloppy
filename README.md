# PicoFloppy

**PicoFloppy** turns a **Raspberry Pi Pico 2 (RP2350)** into a floppy-drive
emulator. It is a port of [FlashFloppy](https://github.com/keirf/flashfloppy),
the widely used Gotek firmware, to the RP2350 and the
[pico-sdk](https://github.com/raspberrypi/pico-sdk). It connects to a retro
computer's floppy cable like a real drive and reads its disk images from a
microSD card.

The FlashFloppy core is unchanged: image formats, `FF.CFG` options, the
display and menu logic, and the HxC-compatible modes. What is new is the
platform underneath it, and an on-screen display built into the firmware.

## Features

- **Shugart / IBM PC floppy interface** on 13 GPIOs, wired in bus-pin
  order. The flux engine runs on PIO0 and DMA at the same 72MHz sample
  clock as the STM32 firmware, so all of FlashFloppy's image handling works
  unmodified.
- **Disk images on microSD** (SPI), either a bare 3.3V card socket or a 5V
  breakout module. With no card fitted, it falls back to a **FAT volume in
  the Pico's own QSPI flash** (3MB) that you build with `scripts/mk_fat.py`.
- **All of FlashFloppy's image formats** (ADF, HFE, IMG/IMA, ST, DSK, D81,
  and many more), with read and write support.
- **Displays**: SSD1306/SH1106 OLEDs (128x32 or 128x64, through
  [u8g2](https://github.com/olikraus/u8g2)), HD44780 LCDs with a PCF8574 I2C
  backpack, and the TM1651 3-digit LED display. Display updates go out by
  DMA in the background.
- **Built-in FF OSD**: the [FF OSD](https://github.com/keirf/flashfloppy-osd)
  on-screen display and Amiga keyboard controller runs on the RP2350's
  second core. It needs no extra board and no I2C link. It overlays the
  drive status on the host's 15kHz video, and the Amiga keyboard can drive
  the emulator.
- **Front panel**: three buttons, or a rotary encoder, plus the on-board LED
  as a drive-activity light.
- **USB serial console** on the Pico's own socket (debug builds). Opening
  the port at 1200 baud reboots the board into BOOTSEL for a firmware
  update.
- Runs entirely from SRAM (a `copy_to_ram` binary), so flash access can
  never disturb flux timing.

## Pinout

Physical RP2350 GPIO numbers, on a Raspberry Pi Pico 2.

| GPIO | Pico pin | Function | Floppy bus pin |
|-----:|---------:|----------|---------------:|
| 0  | 1  | FF OSD: Amiga KB_DATA (open drain) | |
| 1  | 2  | FF OSD: Amiga KB_CLOCK in | |
| 2  | 4  | DSKCHG / HDEN out ("pin02") | 2 |
| 3  | 5  | INDEX out | 8 |
| 4  | 6  | Drive select in (see below) | 10 or 12 |
| 5  | 7  | MOTOR in | 16 |
| 6  | 9  | DIR in | 18 |
| 7  | 10 | STEP in | 20 |
| 8  | 11 | WDATA in (PIO0) | 22 |
| 9  | 12 | WGATE in | 24 |
| 10 | 14 | TRK0 out | 26 |
| 11 | 15 | WRPROT out | 28 |
| 12 | 16 | RDATA out (PIO0) | 30 |
| 13 | 17 | SIDE in | 32 |
| 14 | 19 | RDY / DSKCHG out ("pin34") | 34 |
| 15 | 20 | FF OSD: video CSYNC in | |
| 16 | 21 | microSD MISO (SPI0) | |
| 17 | 22 | microSD CS | |
| 18 | 24 | microSD SCK (SPI0) | |
| 19 | 25 | microSD MOSI (SPI0) | |
| 20 | 26 | Display I2C0 SDA (or LED display DAT) | |
| 21 | 27 | Display I2C0 SCL (or LED display CLK) | |
| 22 | 29 | Button: Select | |
| 25 | —  | On-board LED: drive selected | |
| 26 | 31 | Button: Right / rotary encoder A | |
| 27 | 32 | Button: Left / rotary encoder B | |
| 28 | 34 | FF OSD: video out (via ~270 ohm to R, G or B) | |

The odd-numbered floppy bus pins are all ground. For bus pins 16 to 34,
`bus = GPIO * 2 + 6`, so a 34-way header can be wired straight across.

**Drive select** (GPIO 4) goes to **one** bus pin, like the S0/S1 jumper
on a Gotek: **pin 10** (unit 0, S0) for the Amiga and the Atari ST, **pin
12** (S1) for IBM PCs, where the cable decides the drive letter (A: after
the twist, B: on the straight connector). The firmware is the same either
way.

Power: the floppy ribbon carries no power. Feed **+5V from the drive power
connector** (Berg or Molex) to **VSYS** (pin 39) through a Schottky diode,
so that USB can stay connected for the console. Power the Pico from the
host's own supply: with the Pico unpowered, the host would present 5V to
unpowered pads.

Buttons connect to ground (internal pull-ups). The OLED runs at 3.3V
(3V3 OUT, pin 36). A 5V microSD module (with its own regulator and level
shifter) takes its VCC from VSYS.

Not fitted on this platform: speaker, JC jumper, UART console. Their pins
went to the on-screen display. The interface defaults to Shugart; set
`interface` in `FF.CFG` for the others.

## Amiga

Wire drive select to **bus pin 10** (unit 0: the Amiga's internal DF0).
The default `FF.CFG` settings suit the Amiga: `interface = shugart`,
`pin02 = auto`, `pin34 = auto`, `chgrst = step`. Use `.adf` images; for
the HxC on-screen file selector, copy `HxC_Compat_Mode/Amiga/AUTOBOOT.HFE`
(in the release zip) to the root of the SD card.

## FF OSD on an Amiga

| Signal | GPIO | Amiga 500 |
|--------|-----:|-----------|
| CSYNC | 15 | PCB via next to CIA-A (U7) = Denise pin 32 |
| Video out | 28 | via ~270 ohm to R, G or B (e.g. green, at ferrite E432) |
| KB_DATA | 0 | RP501 pin 2 |
| KB_CLOCK | 1 | RP501 pin 3 |
| GND | GND | common ground |

For other models (A600, A1200) see the
[FF OSD wiki](https://github.com/keirf/flashfloppy-osd/wiki/Amiga-Connections).
CSYNC and the keyboard lines are 5V signals. They go on 5V-tolerant GPIOs
(the ADC pins 26-29 are not). The keyboard is optional.

With **L.Ctrl + L.Alt** held (these key presses are hidden from the Amiga):

| Key | Action |
|-----|--------|
| Left / Right / Up | Prev / Next / Select (the front-panel buttons) |
| Help | OSD menu: sync polarity, double height, position; saved to flash |
| Del | OSD on / off |
| W / A / S / D | Move the OSD box |
| Return | Hold the keyboard: hotkeys work without L.Ctrl + L.Alt; again to release |

The OSD shows alongside an OLED or LCD. With no I2C display, the OSD alone
is the display.

## Building

You need the Arm GNU toolchain (`arm-none-eabi-gcc` with newlib), CMake
3.13+, Python 3, git 2.35+, a host C++ compiler (the SDK builds `pioasm`
and `picotool`), and pico-sdk 2.2.0 or later with its TinyUSB submodule:

```
git clone -b 2.2.0 https://github.com/raspberrypi/pico-sdk
git -C pico-sdk submodule update --init lib/tinyusb
export PICO_SDK_PATH=$PWD/pico-sdk
```

Then, from the PicoFloppy tree:

```
cmake -B build                      # production build (FF_LEVEL=prod)
cmake --build build -j              # -> build/picofloppy.uf2 (.elf, .bin)

cmake -B build-debug -DFF_LEVEL=debug
cmake --build build-debug -j        # with the USB serial console
```

Each build level goes in its own build directory. CMake options:

| Option | Default | Meaning |
|--------|---------|---------|
| `FF_LEVEL` | `prod` | `prod`, `debug` (USB serial console) or `logfile` (in-RAM log) |
| `FF_FW_VER` | short git hash | Firmware version shown at boot |
| `U8G2_GIT_TAG` | `2.37.1` | u8g2 release to fetch |
| `U8G2_SOURCE_DIR` | (empty) | Use this u8g2 checkout instead of fetching it (offline builds) |

The [u8g2](https://github.com/olikraus/u8g2) library is not part of this
repository: on the first configure, CMake fetches the files it needs from
git (a shallow, sparse clone of about 1.5MB), so network access is needed
once per build directory.

`scripts/mk_dist.sh [version]` builds all three levels and packages them,
with the HxC compatibility files, helper scripts and licenses, as
`dist/PicoFloppy-<version>.zip`. The CI workflow runs it on every push,
and the release workflow on every `v*.*` tag (`.github/workflows`).

## Flashing

Hold BOOTSEL while plugging the Pico 2 into USB, then copy
`picofloppy.uf2` to the `RP2350` drive, or use
`picotool load -x build/picofloppy.uf2`. There is no FlashFloppy
bootloader: firmware updates always go through BOOTSEL, which can also be
reached from the main menu (*Update Firmware*) or over USB (below).

## USB serial console

The Pico 2's own USB socket is a USB CDC serial port (`/dev/ttyACM0` on
Linux). Debug builds write their log to it; output from before a terminal
is opened is kept (4kB) and sent when it is. The line rate is ignored.

**Opening the port at 1200 baud and closing it again reboots the board
into BOOTSEL**, as on Arduino boards, ready for a new UF2:

```
stty -F /dev/ttyACM0 1200
```

## Disk images in the internal flash

With no microSD card, the firmware uses a FAT volume in the Pico's own
flash, from 1MB to 4MB. Build one from a directory of disk images and
install it from BOOTSEL:

```
python3 scripts/mk_fat.py my_disks/ fs.uf2   # also writes fs.bin
picotool load -o 0x10100000 fs.bin           # or copy fs.uf2 to RP2350
```

The firmware and the image store are flashed independently. The volume is
writable (host writes, `IMAGE_A.CFG`, the eject menu), but QSPI flash
endures about 100k erase cycles per block. An SD card is probed only at
boot.

## Limitations

- Shugart/PC interface only (no Apple II or QuickDisk firmware); no USB
  stick storage.
- No speaker, JC jumper or UART console: their pins went to the OSD.
- The FF OSD supports 15kHz (PAL/NTSC) video only, and has none of the FF
  OSD board's user output pins (F1-F10 hotkeys).
- The `chgrst=pa14` option is not supported; use `chgrst=step` or a delay.

For using the emulator itself (image formats, `FF.CFG`, host
compatibility, navigation modes), see the
[FlashFloppy wiki](https://github.com/keirf/flashfloppy/wiki). Everything
there applies, except for the Gotek-specific hardware pages.

## Acknowledgments

PicoFloppy stands on other people's work:

- **Keir Fraser**, author of [FlashFloppy](https://github.com/keirf/flashfloppy)
  and [FF OSD](https://github.com/keirf/flashfloppy-osd). Nearly all of the
  emulator, and the OSD logic, is his code.
- **[bondimachine/flashfloppy](https://github.com/bondimachine/flashfloppy)**,
  the bare-metal Raspberry Pi Pico 2 port of FlashFloppy that this project
  started from.
- **Raspberry Pi Ltd**: the [pico-sdk](https://github.com/raspberrypi/pico-sdk)
  and [picotool](https://github.com/raspberrypi/picotool).
- **Ha Thach** and contributors: [TinyUSB](https://github.com/hathach/tinyusb),
  the USB stack behind the serial console.
- **Oliver Kraus**: [u8g2](https://github.com/olikraus/u8g2), the OLED graphics
  library.
- **ChaN**: [FatFs](http://elm-chan.org/fsw/ff/), the FAT filesystem module.
- **Chris Cannam**: the Codec terminal font, the base of the 6x13 display
  font.
- The [HxC Floppy Emulator](https://hxc2001.com) project (Jean-François
  Del Nero), whose file selector and image formats FlashFloppy supports.

## Reference repositories

| Project | Repository |
|---------|------------|
| FlashFloppy (upstream) | <https://github.com/keirf/flashfloppy> |
| FlashFloppy wiki | <https://github.com/keirf/flashfloppy/wiki> |
| FF OSD | <https://github.com/keirf/flashfloppy-osd> |
| FlashFloppy HxC file selector | <https://github.com/keirf/flashfloppy-hxc-file-selector> |
| Pico 2 bare-metal port (starting point) | <https://github.com/bondimachine/flashfloppy> |
| pico-sdk | <https://github.com/raspberrypi/pico-sdk> |
| picotool | <https://github.com/raspberrypi/picotool> |
| TinyUSB | <https://github.com/hathach/tinyusb> |
| u8g2 | <https://github.com/olikraus/u8g2> |
| FatFs | <http://elm-chan.org/fsw/ff/> |

## License

PicoFloppy is released under the [MIT License](LICENSE).

It is built on code that its authors placed in the public domain
([Unlicense](https://unlicense.org)): FlashFloppy and FF OSD, by Keir
Fraser. The third-party code keeps its own license: FatFs (`src/fatfs/`,
BSD-style, notice in the source), u8g2 (BSD-2-Clause, fetched at build
time), the pico-sdk (BSD-3-Clause) and TinyUSB (MIT). The release zip
carries their license texts in `licenses/`; include them with any binary
redistribution.
