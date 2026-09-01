# Scrolling audio click fix

The user confirmed `CROWPANEL_PICO_GB_CRYSTAL_AUDIO_DMA_QUEUE.uf2` eliminated
the lag, but introduced clicks while walking. Keep that binary for comparison.

## Confirmed code defect (audible result still needs hardware testing)

The old two-channel chain treated an idle DMA channel as a free buffer.
On RP2040, a chain trigger reloads the original transfer count but does not
rewind READ_ADDR. A delayed producer can therefore cause an automatic restart
past the end of the old buffer. The old "both channels idle" recovery does not
reliably detect this condition. See the SDK's RP2040 `DMA_CH0_TRANS_COUNT`
register description or RP2040 datasheet section 2.5.1.1.

## Changes

- Three blocks with explicit FREE/FILLING/READY/PLAYING ownership.
- One non-chaining DMA channel; its shared DMA_IRQ_1 completion handler starts
  only a completed READY block and resets its source address every time.
- Conversion cannot overwrite a queued or playing block. Completion during
  conversion cannot consume a partially filled block.
- High DMA scheduling priority for the small audio transfers.
- Eight-sample boundary blend only at startup or after an actual underrun.
  Normal audio blocks, volume/gain, sample rate and CPU clock are unchanged.
- Mute and shutdown mask this channel's IRQ, abort, acknowledge stale completion
  and reset ownership. They do not disable the shared IRQ line for other users.
- Removed the 1,584-byte whole-frame sprite-list cache (reported neutral in
  testing) to fund the third audio block. The first-ten-sprites-per-line
  optimization and other renderer/CPU/display optimizations are retained.

## Verification

- Pico release build succeeded: static RAM 258,408 / 262,144 bytes;
  flash 325,432 / 782,336 bytes. Runtime heap headroom remains tight.
- `tests/run_audio_queue_test.py` compiles the production queue for Cortex-M0+
  and executes it under Unicorn. Startup, invalid publishes, full queues,
  completion during refill, underruns, resets and 50,000 randomized interleavings
  passed. This tests queue logic, not actual IRQ latency or physical audio.
- Test-only Unicorn package is project-local in `.audio-test-tools`.
  Set PYTHONPATH to that directory and run the test runner with bundled Python.
- Protected stable firmware SHA256:
  `502C6868A5D238A8C2760B4B4589B9D2F23BB1177DC89292A2B799230575CBA6`.
- Original lag-free queued-audio firmware SHA256:
  `9D23C894665C6DA02E0126D31C0A35E6D4B604C886F9F10C00D231A37E32EBC9`.

The new build is not flashed automatically. Test scrolling, menu music, volume
mute/resume and sleep/wake on the board. No ROM, save or settings files changed.
