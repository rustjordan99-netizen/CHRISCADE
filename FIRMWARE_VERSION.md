# CHRISCADE firmware revisions

The historical combined Gameboy/Doom baseline is 264. New emulator UF2s
are numbered v265 onward and display that revision in Settings > System Info.

`version_build.py` runs automatically for PlatformIO builds. It embeds the next
revision in the settings screen, then updates `firmware_version.json` only after
UF2 generation. Failed compilation and clean-only operations do not increment
the counter. Copying, renaming, uploading, or reflashing a UF2 keeps its version.
An ordinary build intentionally generates the next version, even with no source
changes. Retain the JSON counter and build history when cleaning old artifacts.

The automation covers this project's PlatformIO builds, not external converters
or the separate legacy Doom CMake project. Any future Doom release must be
coordinated with this counter before assigning its release revision.
