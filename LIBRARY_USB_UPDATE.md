# CHRISCADE library update — 2026-08-30

Firmware: `CROWPANEL_PICO_GB_LIBRARY_USB_UPDATE.uf2`

SHA256: `E043BF24B14F014035DF7B46D6E470A0CF10B83B0BD06601FF5B4DA20D14B3DC`

## Library and editing

- Removed left/right page controls. Up/Down continuously scrolls through the
  alphabetical library, revealing one game at a time at the visible edge.
  Selection stops at the beginning/end rather than wrapping. Only eight names
  are held in memory; scrolling doesn't allocate the whole library.
- After the final game, Down highlights ADD GAME; Up returns to the last game.
  ADD GAME is also always available by touching its bottom-left button, including
  in an empty library. A opens the highlighted game or Add Game. B returns Home.
- Bottom-right button reads `SELECT // SETTINGS`. It and the physical Select
  button open Rename/Delete for the highlighted game. Existing delete-confirmation
  and save-preservation behavior remains intact.
- Rename has a visible insertion cursor. Tap the name to place it, use the
  on-screen arrows, or press X/Y to move left/right. Typing inserts at that point;
  DEL removes the previous character, not the entire suffix. The view follows
  the cursor for long names, and the original extension is kept.
- The small game-list and larger cartridge-writing Pokeballs share a circular
  pixel mask. Red fill and white fill cannot extend beyond the black outline.

## Add a game without removing the SD card

1. Flash this UF2 as usual, then open Game Library > ADD GAME.
2. Connect the handheld to the PC using a USB data cable.
3. Double-click `SEND_GAME_USB.cmd` in this project folder.
4. Choose a `.gb`, `.gbc`, or `.gbz` file. Select the handheld's COM port from
   the list (not an unrelated serial device).
5. Wait for the verified-success message, then press B on the handheld. The
   refreshed library includes the new file in alphabetical order.

This uses USB serial, **not a drive in File Explorer**. The Windows launcher
uses the existing Python runtime and project-local pyserial, without installing
anything new. Advanced usage: `send_game_usb.py --port COM5 "path/to/Game.gb"`.
Close any serial monitor holding the same COM port before running it.

The uploader accepts only new ROM files at the SD root. It cannot read files
back, expose raw sectors, overwrite existing names, format the card, or delete
other files. Saves/settings are not upload targets. Pick a different filename
if a game already exists. The 16 MiB transfer cap is not a new emulator/flash
capacity: existing ROM-size and supported-format limits still apply when loading.

Uploads use bounded 512-byte acknowledged chunks and a streaming CRC32 check.
The ROM appears in the library only after successful verification and rename
from an exclusively created temporary file. B cancels; a stalled transfer times
out after ten seconds without a byte. Cancellation/error attempts to remove only
that session's new temporary file. Existing temporary files are never reused or
removed. Sudden power loss or card failure can leave an incomplete `CC*.TMP`
file; it is not shown as a game. As with all SD writes, avoid unplugging power or
removing the card during the transfer—filesystem corruption cannot be ruled out.
Deep-sleep polling pauses during an active transfer and resumes afterward.

## Implementation and checks

- USB reception and SD writes run in the menu foreground, before the second
  display core and emulator audio start. No USB callback touches the shared SPI
  bus. Raw USB mass storage remains disabled.
- Reuses the existing flash-sector workspace for upload data, rename text and
  directory-scan scratch at nonoverlapping offsets. No new persistent buffer or
  per-frame gameplay work. Nested filename buffers were moved off core 0's 2 KB
  stack. Only changed rows/controls repaint while scrolling, not the full screen.
- Release build passes: static RAM **257,804 / 262,144 bytes**, unchanged;
  flash **352,696 / 782,336 bytes**.
- UF2 verified: **1,428 RP2040 blocks**, payload end **0x10059400**, safely below
  the ROM area at **0x100BF000**. All six previous stable/recent firmware hashes
  remain unchanged. No board was flashed automatically.
- `tests/run_library_update_tests.py`: actual Cortex-M0+ helper code, fake SD.
  Covers cursor insertion/deletion/UTF-8/bounds, both Pokeball masks, continuous
  scrolling for libraries of 0–30 games, short shifted windows after deletion,
  upload header/CRC validation, exclusive creation, existing-name collisions,
  write/sync/rename failures, cancellation, and preservation of original files.
  Includes the earlier rename/delete policy regression tests.
- `tests/test_usb_rom_sender.py`: five tests, mock serial/in-memory files only;
  exact payloads across chunk boundaries, Unicode filenames, duplicates,
  incomplete writes, rejected extensions and empty inputs.
- Audio regressions pass: 50,000 queue interleavings; 4,096 sample/state-exact APU
  frames against the stored reference; all 321 output-table entries and bounds.
  Existing all-games loudness boost and 266 MHz clock setting are unchanged.

Physical touchscreen, USB throughput and actual SD transfer still need a board
test. Start with a backed-up ROM under a new test filename, then try a duplicate
upload (must be refused) and B cancellation. No real game/save files were changed
by development tests.
