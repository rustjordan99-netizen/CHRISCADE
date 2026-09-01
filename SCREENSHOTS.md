# In-game screenshots

Press X once during gameplay to save the displayed game frame. The game pauses
briefly during SD access and resumes automatically. A two-click shutter confirms
a successful save; a low double beep means capture failed (for example, a full
gallery or SD error). Both respect the physical volume pot, including mute.

Open App Library > Screenshots to view/delete pictures. The existing four shared
drawing/screenshot slots and RGB565 file format are unchanged. Game captures are
labelled with the ROM's internal title. Existing pictures and saves are preserved.
If the four slots are occupied, delete an unwanted picture before capturing.

Implementation: capture the completed 160x144 indexed RAM frame and current
palette. Reconstruct native/stretch/aspect scaling into the existing 320-pixel
DMA scratch row and publish the 320x240 image through the same verified temporary
file/rename path as drawings. Core 1 drains DMA and yields the SPI bus cooperatively;
it is not reset, and TFT DMA is not reinitialized. No TFT GRAM readback is used.
Gameplay audio's existing DMA channel and PCM scratch buffer provide feedback;
the boot-tone driver is not invoked while gameplay owns the speaker.

Tests: run `tests/run_drawing_test.py` with the project's ARM/Unicorn dependencies.
It compares every exported pixel for GB and GBC in all scaling modes, exercises
the shared save-failure/collision paths, checks the shutter envelope, and retains
the drawing regression tests. Physical-device SPI handoff and sound still require
on-device testing (repeat captures, mute, full gallery and resume in GB/GBC).
