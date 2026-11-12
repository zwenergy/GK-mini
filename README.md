# GK mini
An AI-slopped Timetop GameKing emulator "on" the Pokemon mini.
The actual emulator runs on the RP2040 of an PM2040-based flash cart.

## Overview
GK mini emulates the Timetop GameKing (GM-218, also GameKing II) "on" the Pokemon mini via the [PM2040](https://github.com/zwenergy/PM2040) flash cart.
Like [GB mini](https://github.com/zwenergy/GB-mini), the emulator does **not** run on the Pokemon mini itself but on the RP2040 of the PM2040 cart.
The Pokemon mini runs the same small frame/audio pull program as GB mini (`rpfb.h`): it reads the frame buffer and the audio sample from the cart's address space and writes back the key state.

The emulation is written from scratch in C and follows the hardware behaviour of the [GameKing MiSTer core](https://github.com/MiSTer-devel/GameKing_MiSTer) (register map, banking, interrupt controller, timers, PSG, DMA and LCD controller of the Sitronix ST2204), cross-checked against MAME's `gameking` driver.

## Emulated hardware
* **CPU**: W65C02S core of the ST2204 at 6 MHz, including the Rockwell bit instructions, WAI and STP (STP runs as NOP, as on the MiSTer core). Passes Klaus Dormann's 6502 functional test and 65C02 extended opcode test.
* **Memory/banking**: 10 KB internal RAM, the 16 KB program window (PRR, IRR swap during interrupts), the 32 KB data window (DRR), and the chip-select decode for the internal 512 KB BIOS ROM and the cartridge (/CE0).
* **Interrupts**: all levels with per-level vectors, auto-clear and priority as in the RTL.
* **Timers**: base timer (2 Hz to 2048 Hz taps), prescaler, timer 0, timer 1 (prescaler and 32.768 kHz modes), watchdog.
* **DMA**: banked and unbanked sources, fixed or incrementing destination. Transfers complete instantly, with the CPU charged two cycles per byte.
* **LCD controller**: 48x32, 4 grey levels (2 bpp, 1 bpp also handled), frame interrupt at ~122 Hz, reverse mode.
* **PSG**: both square channels with volume, noise, and the PWM DAC modes, including the sample interrupt the games use to stream PCM.

Not emulated: UART/IR and SPI (only their registers), the GameKing III.

## Button Mapping
The D-Pad and A/B buttons are mapped directly.
The C-Button of the PM is mapped to START.
The Power button of the PM is mapped to SELECT.

To mute the sound, hold the C button while switching the Pokemon mini on (as in Pokemon mini games). C only acts as START again once it has been released.

For turning off the Pokemon mini hold the Power button for roughly 2 seconds (handled by the PM program, as in GB mini).

## Video
The GameKing's 48x32 screen is scaled by exactly 2x to the Pokemon mini's 96x64, so it fills the screen with no cropping.
The four grey levels are shown by blending three PM frames (as in GB mini with `FRAMEBLEND 2`), with the pattern also spread across each 2x2 pixel block to reduce flicker.

## Audio
The PSG output is sampled at 15625 Hz (exactly twice the GameKing's typical 7812.5 Hz PCM rate) and played back on the PM as 5-bit samples.
The audio stream also paces the emulator: when the sample buffer is full, the emulation waits, which locks it to real-time speed.

## Usage
You need a [PM2040](https://github.com/zwenergy/PM2040)-based flash cart, a GameKing BIOS dump (`gm218.bin`, 512 KB, as used by MAME and the MiSTer core) and a GameKing cartridge ROM.

Build a flashable UF2 with the [online patcher](https://zwenergy.github.io/GKminiPatch/index.html): select the GK mini UF2, the BIOS and the game ROM, then click Patch.

Or use the command-line tool, which produces the identical file:

```
python3 tools/gkpatch.py PM2040_GK.uf2 gm218.bin game.bin -o game_pm.uf2
```

Hold the BOOT button of the PM2040 cart and drag'n'drop `game_pm.uf2` onto the cart.
If no valid image is found in flash, the PM shows a diagnostic screen of eight lines of hex digits:

| Line | Content |
| --- | --- |
| 1 | info block bytes 0-11 (should start `474B4D494E490001`, then `00000800`) |
| 2 | info block bytes 12-19 (ROM size, flags; little endian) |
| 3 | info block bytes 0-11 read through the uncached XIP alias |
| 4 | first 12 BIOS bytes (flash `0x40000`) |
| 5 | BIOS bytes `0x7FF4-0x7FFF` (interrupt and reset vectors) |
| 6 | first 12 ROM bytes (flash `0xC0000`) |
| 7 | flash JEDEC ID (manufacturer, type, capacity as 2^n bytes, e.g. `15` = 2 MB), then the error code: `01` bad magic, `02` bad BIOS size, `03` ROM too large |
| 8 | flash size the firmware was built for |

By default the tool patches out the BIOS auto-sleep (the GameKing blanks the screen after about two minutes without input and wakes only after START is held for about two seconds), using the same patch sites as the MiSTer core's "Disable Auto-Sleep" option. It only patches when the original bytes match. Use `--keep-sleep` to flash the BIOS untouched.

### Flash layout
| Offset | Content |
| --- | --- |
| `0x000000` | firmware (must stay below `0x3F000`) |
| `0x03F000` | info block (magic, BIOS size, ROM size, flags) |
| `0x040000` | GameKing BIOS, 512 KB |
| `0x0C0000` | cartridge ROM, up to 1.25 MB on a 2 MB flash |

## Building
Firmware (needs the [Pico SDK](https://github.com/raspberrypi/pico-sdk) and `arm-none-eabi-gcc`):

```
cd code
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make
```

This produces the ROM-less base firmware `PM2040_GK.uf2`. It uses the same memory map as GB mini, so the time from power-on to the PIO/DMA cart bus coming up stays as short as GB mini's (the Pokemon mini gives up on the cart otherwise). The emulator core (`gk.c`) is linked into a separate section (`.gk_ram` in `memmap.ld`) that is stored in flash and only copied to RAM by `doGK()` after the bus is running, so the flash bus serves only the BIOS and cartridge reads during emulation. The BIOS resident pages and the first 64 KB of the cartridge are also copied into RAM at that point (into uninitialized RAM, which crt0 does not clear).

The system clock is 240 MHz at 1.30 V, as in GB mini. Override it with `-DSYS_CLOCK_KHZ=...` added to the compile definitions if needed.

## Credits
* Hardware behaviour: [GameKing MiSTer core](https://github.com/MiSTer-devel/GameKing_MiSTer) by Jamie Blanks, and MAME's GameKing/ST2204 emulation (Peter Trauner, AJR).
* PM2040 bus interface and the PM-side program: [GB mini](https://github.com/zwenergy/GB-mini) / [PM2040](https://github.com/zwenergy/PM2040).
