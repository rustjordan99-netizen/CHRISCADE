# CHRISCADE hardware

## Core components

| Part | Used in this build |
| --- | --- |
| Main controller/display | CrowPanel RP2040 with integrated touch display |
| Analog control | Parallax joystick |
| Main buttons | 6 mm diameter buttons for A, B, X, and Y |
| Small buttons | Approximately 3.3 mm buttons for Start, Select, and Power |
| Volume | 10 kΩ potentiometer and printed volume spinner |
| Wiring | Jumper wires; quantity and lengths depend on enclosure routing |
| Enclosure | Custom 3D-printed shell and control parts |

The firmware pin assignments and electrical behavior are defined in the source
configuration. Check them before changing any component or wiring.

## Printable files

All printable parts are in [`cad/`](cad/):

| File | Part |
| --- | --- |
| `front shell.3mf` | Front half of the handheld shell |
| `back.3mf` | Rear half of the handheld shell |
| `button panel.3mf` | Button-panel insert |
| `ABXY.3mf` | A/B/X/Y button set |
| `small buttons.3mf` | Small auxiliary button set |
| `select.3mf` | Select button |
| `start stop.3mf` | Start/Stop button |
| `power.3mf` | Power button |
| `volume spinner.3mf` | Knob for the 10 kΩ volume potentiometer |

## Assembly notes

- Test the display, controls, SD card, speaker, and power wiring before closing
  the case.
- Keep jumper wiring clear of the joystick and button travel.
- Test-fit buttons and the volume knob after printing; printer tolerances vary.
- The CAD files are supplied as `.3mf`, which can be opened directly in most
  modern slicers.
