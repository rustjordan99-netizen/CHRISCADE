# Game-library file settings — 2026-08-30

Firmware: `CROWPANEL_PICO_GB_GAME_SETTINGS.uf2`

SHA256: `3B1583F108E37D3654220772001CB49984AE92162B9525EAADD059A8E6AF7128`

Built on the all-games louder-audio firmware; retains that boost, the guarded
audio queue, sound-generation optimizations and existing game/display settings.

## Controls

- Highlight a game in Game Library and press SELECT, or tap the bottom-right
  SELECT SETTINGS button. B still returns Home; A still loads a game.
- Settings offers Rename and Delete for that highlighted SD file. Touch works,
  as do Up/Down and A. B/SELECT, or tapping the settings footer, returns.
- Rename uses a touch keyboard with uppercase/lowercase, digits, punctuation,
  Space, Delete-character, Clear, Save and Back. The stick and A also operate
  the keyboard; START saves and B cancels. The original extension is kept.
- Delete opens a separate confirmation showing the complete filename, with
  Cancel selected by default. Choose Right then A, or tap Delete, to confirm.
  B/SELECT cancels. Entry input must be released before a new action is read,
  so holding the opening A press/touch cannot also confirm deletion.
- After rename, the A-Z list follows the renamed file to its new position.
  After deletion, selection is clamped and an empty final page returns to the
  previous page. An entirely empty library still supports Back and Settings.

## File protection and limitations

- Only the exact selected `.gb`, `.gbc` or `.gbz` root-directory file is targeted.
  No recursion, paths, wildcard deletion or filesystem formatting.
- The filename policy rejects path separators, control/invalid FAT characters,
  empty/oversized names and Windows device names. Rename preserves extension.
- Existing destination files/directories are never overwritten. SdFat's own
  exclusive-create rename behavior also enforces this. Case-only renames are
  reported as unchanged because the SD filesystem is case-insensitive.
- Read-only files and directories are not modified. Missing files and I/O
  failures are reported rather than treated as successful operations.
- Save filenames are derived from the internal ROM header, not the SD filename.
  These menus never rename or delete save/settings files.
- Deletion removes the SD ROM file. There is no on-device trash/undo; restore
  it by copying the original ROM back. The last flashed ROM on the Pico is not
  erased, so START Last Game can still run that cached copy.
- Avoid removing power/card during any SD update. As with existing save writes,
  filesystem/power failures cannot be made fully transactional here.

## Implementation and verification

- UI uses the current theme helpers, rounded controls and existing click sound.
  Focus and keyboard edits redraw only their changed controls/field.
- The rename editor reuses 512 bytes of the existing flash-sector workspace
  while in menus; it allocates no new persistent buffer or gameplay work.
- `src/rom_file_actions.h` contains the shared filename rules and exact file
  operation. `tests/run_rom_file_actions_test.py` compiles this production code
  for Cortex-M0+ and executes it with an in-memory fake filesystem under Unicorn.
- Tests passed for bounded names, extension retention, UTF-8 filenames,
  traversal/invalid names, case-insensitive collisions, directories, read-only
  attributes, absent files, simulated I/O failures, single-file rename/delete
  and preservation of other game/save/settings entries. No real game file was
  opened, renamed or deleted by these tests.
- Confirm/cancel input release handling, keyboard bounds and page refresh paths
  were source-reviewed. Physical touchscreen behavior still needs a board test.
- Final release build passed. Static RAM **257,804 / 262,144 bytes** (unchanged);
  flash **341,448 / 782,336 bytes**. Menu buffers stay off the small core-0 stack.
- UF2 checked: **1,384 RP2040 blocks**, payload ends **0x10056800**, before ROM
  storage at **0x100BF000**. ROM capacity is unchanged.
- All five prior stable/audio firmware hashes were checked and remain intact.
  New UF2 is separate and has not been flashed automatically.

On-device check: cancel a delete first (A on default Cancel, B, and touch), then
try renaming a test ROM, a duplicate-name rejection, and deleting a backed-up
test ROM. Check the last entry on a page, the empty library, and loading its
existing save after a rename. No actual SD-card files were changed during this
development task.
