# CHRISCADE

**A custom RP2040 handheld console with a 3D-printed shell, touch-first UI,
Game Boy / Game Boy Color emulation, microSD storage, and a small app library.**

<p align="center">
  <img src="doc/gallery/pokemon-crystal.jpg" width="31%" alt="CHRISCADE running Pokémon Crystal">
  <img src="doc/gallery/drawing-app.jpg" width="31%" alt="CHRISCADE drawing app">
  <img src="doc/gallery/blue-shell.jpg" width="31%" alt="CHRISCADE printed shell interior">
</p>

<p align="center"><i>From enclosure concept to printed handheld and custom firmware.</i></p>

<p align="center">
  <img src="doc/gallery/handheld-enclosure-render.png" width="68%" alt="CHRISCADE enclosure CAD render">
</p>

## What it does

- Runs legally owned Game Boy and Game Boy Color games from a FAT32 microSD card
- Saves game progress to the SD card
- Provides a themed game launcher with touch and physical controls
- Includes Draw, Calculator, Timer / Stopwatch / Alarm, Metronome, Settings, and Gallery apps
- Captures screenshots into per-game and per-app SD folders; screenshots can be renamed or edited
- Supports a custom animated boot sequence, boot themes/sounds, brightness, LEDs, battery status, audio, and sleep controls
- Uses a USB game-transfer workflow so games can be managed without removing the SD card

## Hardware

- CrowPanel RP2040 touchscreen controller/display
- Parallax joystick
- 6 mm A/B/X/Y buttons and approximately 3.3 mm Start, Select, and Power buttons
- 10 kΩ volume potentiometer, speaker/amplifier, status LEDs, microSD, and jumper wiring
- Custom 3D-printed enclosure and controls

The printable shell and button files, component list, and assembly notes are in
[hardware/README.md](hardware/README.md).

## Build

Install PlatformIO, then build the `pico` environment:

```sh
pio run -e pico
```

The UF2 is created at `.pio/build/pico/firmware.uf2`. Hold BOOTSEL while
connecting the Pico, then copy that file to the mounted `RPI-RP2` drive.

> This repository includes firmware, CAD, and documentation only. It does not
> contain commercial ROMs, save data, screenshots, or generated UF2 releases.
> Use only game dumps you legally own.

## Credits

CHRISCADE is a heavily customized fork built on
[YouMakeTech's Pico-GB](https://github.com/YouMakeTech/Pico-GB), itself based
on deltabeard's Peanut-GB / RP2040-GB work. Thanks to those projects and their
contributors, as well as Bodmer's TFT_eSPI and the other libraries included in
this source tree. Their license notices remain with their respective code.
