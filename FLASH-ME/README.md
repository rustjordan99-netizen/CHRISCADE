# Flash CHRISCADE

## Use this file

Copy [`CHRISCADE_v273_FLASH_THIS.uf2`](CHRISCADE_v273_FLASH_THIS.uf2) to the
CrowPanel RP2040:

1. Disconnect USB power from the handheld.
2. Hold the board's **BOOTSEL** button while connecting the USB cable.
3. Release BOOTSEL when the `RPI-RP2` drive appears.
4. Copy `CHRISCADE_v273_FLASH_THIS.uf2` onto the `RPI-RP2` drive.
5. Wait for the drive to disappear and the handheld to reboot.

This is the tested v273 build. It includes the gallery folder cleanup, exact
Game Library folder logos, no-flash Undo repaint, and multi-stroke Undo.

Flashing firmware does not erase game saves stored on the microSD card. Keep
the card inserted during normal use, but do not confuse the `RPI-RP2` drive
with the game microSD card.
