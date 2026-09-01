# Shared gameplay volume boost — 2026-08-30

New firmware: `CROWPANEL_PICO_GB_ALL_GAMES_LOUDER_AUDIO.uf2`

SHA256: `66492CBF863AC2E16DC949E77C91C271DD29683BA592F66CAFC0C889C4D8CEA5`

Built on the fine-tuned audio firmware. The boost is in the common
`crowpanel_audio_submit` output path, so every GB/GBC game, including GBZ files,
uses it. There are no title checks or per-ROM patches. Doom firmware and the
startup/UI sounds are not changed.

The startup jingle can use full-amplitude square-wave tones. Gameplay audio
mixes several channels, averages stereo to mono, filters the result, and limits
peaks; these have different average loudness. Both already use the same 700/4000
volume-knob thresholds and cubic taper.

The new `audio_output_curve.h` adds 1.5x gain before the existing soft limiter,
precomputed into the existing 321-byte table. Quiet/moderate signals are boosted
more than already-loud peaks. This is not a guarantee of 50% greater perceived
loudness or an exact jingle loudness match. The soft limiter reduces hard
clipping but cannot guarantee distortion-free physical speaker output.

No per-sample arithmetic or table RAM is added. Sample rate, 266 MHz clock,
filter, volume/mute controls, audio queue/IRQ refinements, rendering, saves,
settings and sleep behavior are retained.

Verification:

- Production curve compiled for Cortex-M0+ and executed under Unicorn: all
  321 entries passed silence, monotonicity, non-attenuation, soft-knee continuity,
  signed PWM symmetry and range tests. All larger uint16 inputs clamp safely.
- Queue startup/refill/recovery/reset and 50,000 randomized interleavings passed.
- Release Pico build passed: RAM 257,804 / 262,144 (unchanged), flash
  324,928 / 782,336. Existing unrelated build warnings remain.
- UF2 block numbering, magic and RP2040 family checked: 1,319 blocks, highest
  payload end 0x10052700, below ROM storage at 0x100BF000.
- Previous fine-tune, click-fix, DMA-queue and stable UF2 hashes rechecked;
  none overwritten. No ROM/save/settings files changed. Not auto-flashed.

Hardware test: begin below maximum volume, compare the same scenes with the
previous fine-tune build at the same knob position, and listen for harshness in
busy music/effects. Test more than one game, full mute/resume, and map scrolling.
Actual loudness and speaker distortion still require this listening test.
