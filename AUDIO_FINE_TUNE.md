# Audio fine-tune — 2026-08-30

## Starting point and retained gains

The user reported that queued DMA eliminated walking slowdown, and the guarded
three-buffer queue reduced the subsequent click to almost negligible. This
build starts from `CROWPANEL_PICO_GB_CRYSTAL_AUDIO_CLICK_FIX.uf2`, not an older
slow build.

Retained without retuning or removing:

- CGB first-ten-sprites-per-scanline culling and simplified sprite priority.
- Fast eight-pixel background segments and nibble expansion, hidden-background
  skipping behind the window, cached scanline register inputs.
- Precomputed display scaling, four-pixel row hashing, palette-change checks,
  and conversion only for changed rows.
- Display DMA line storage in scratch X SRAM, independent of main RAM traffic.
- Display dispatch after audio submission, two stable source framebuffers,
  and suppression of rendering when the display cannot accept a frame.
- Existing direct CPU memory accesses, wide accesses, cached ROM pointers and
  WRAM-to-VRAM DMA copies. These are retained, not claimed as independently
  proven gains from this pass.
- Three explicitly owned audio blocks, high-priority DMA transfers, safe
  address reset on every block, no unsafe automatic chain/replay, and the
  existing short startup/underrun blend.
- Ten-bit PWM, soft-limiter/level lookup tables, 22,050 Hz sample rate, existing
  filter and volume curve, and muted-game speed limiting.

The neutral whole-frame sprite-list cache remains removed to leave space for
the third audio buffer. No higher overclock or additional framebuffer/cache
allocation is introduced. CPU remains at the existing 266 MHz setting, with
its existing fallback. Screen size, ROM storage, saves, UI, settings, touch,
LEDs and sleep behavior are not edited.

## New refinements

1. Remove provably zero integer contributions in square, wave and noise
   phase-transition loops. The old code divided the phase fraction BEFORE
   multiplying by sample amplitude, so it always truncated to zero. Every
   oscillator, sweep, envelope and length transition is preserved.
2. Read only the final wave-table phase for each output sample, and replace
   wave output division with exact signed power-of-two scaling. Negative
   samples still truncate toward zero. Hoist stable stereo routing gains out
   of the per-sample loops for all channels.
3. Replace queue cursor modulo-three operations with increment-and-wrap
   branches, reducing completion-path overhead without changing ownership.
4. Temporarily raise DMA_IRQ_1 CPU priority to 0x40 if it was lower priority.
   This lets the short SRAM completion handler preempt default-priority work
   near the ~45 us sample deadline. Preserve any already-higher priority and
   restore the original value on audio shutdown. DMA transfer priority was
   already high and remains so.

These changes provide extra audio-production headroom and a faster block
handoff; they do not establish that the remaining physical click is eliminated.

## Verification

- Release `pico` build passed. Existing APU MAX/MIN macro redefinition warnings
  remain; they predate this change.
- Static RAM: **257,804 / 262,144 bytes**, 604 fewer than the starting build.
  Runtime heap headroom remains tight; no new allocation was added.
- Flash: **324,936 / 782,336 bytes**, 496 fewer than the starting build.
- Linked audio callback and completion handler are in SRAM. The display DMA
  line buffer remains at 0x20040000 in scratch X.
- `tests/run_apu_differential_test.py` compiles both the untouched local APU
  reference and current production source for Cortex-M0+, then runs them with
  Unicorn. **512 scenarios / 4,096 audio frames** matched every stereo sample,
  register read, and internal state byte. Covers channel frequency extremes,
  counter boundaries, duty cycles, wave volume, routing, envelopes, sweeps,
  retriggers, muted/disabled channels, power changes and randomized writes.
- A synthetic four-channel callback used **121,663 vs 213,598 guest ARM
  instructions (43.0% fewer)**. This measures only that synthetic callback,
  not whole-game speed or real MCU cycles, bus contention or IRQ timing.
- Production queue tests passed startup, delayed conversion, underrun recovery,
  reset and **50,000 randomized producer/consumer interleavings**.
- Tests are outside the firmware build. `tests/apu_reference.cpp` is a frozen
  copy of the pre-change local source, with its original license header.
- UF2: 1,319 valid RP2040 blocks, payload ends at **0x10052700**, before ROM
  storage at **0x100BF000**. No SD/ROM/save/settings files were modified.

Run both test scripts with the project-local `.audio-test-tools` on PYTHONPATH
using the bundled Python runtime. They require the existing ARM toolchain.

## Firmware

New file: `CROWPANEL_PICO_GB_CRYSTAL_AUDIO_FINE_TUNE.uf2`

SHA256: `D1D55345DB4DC5EFC2FDCACC946E4805B656822D538285876990B177940D9D75`

Previous binaries preserved and hashes rechecked:

- `CROWPANEL_PICO_GB_CRYSTAL_AUDIO_CLICK_FIX.uf2`:
  `431EE532D3AE9EEEC208BB39CD84BF891AD22DB46A2F72063D59A0A85B92DB0E`
- `CROWPANEL_PICO_GB_CRYSTAL_AUDIO_DMA_QUEUE.uf2`:
  `9D23C894665C6DA02E0126D31C0A35E6D4B604C886F9F10C00D231A37E32EBC9`
- `CROWPANEL_PICO_GB_SD_SAVE.uf2`:
  `502C6868A5D238A8C2760B4B4589B9D2F23BB1177DC89292A2B799230575CBA6`

Not automatically flashed. On the handheld compare walking into new map areas
against standing still, bumping into a wall and opening the menu. Also check
volume mute/resume, sleep/wake, and another game such as Mario for regressions.
