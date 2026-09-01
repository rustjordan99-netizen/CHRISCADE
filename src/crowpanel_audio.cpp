#include <Arduino.h>

#include "common.h"
#include "crowpanel_audio.h"
#include "minigb_apu.h"
#include "audio_block_queue.h"
#include "audio_output_curve.h"
#include "game_picture.h"

#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/irq.h>
#include <hardware/sync.h>

// Ten-bit PWM gives the direct panel speaker four times as many duty-cycle
// levels as the original 8-bit output, while the carrier remains far above
// the audible range at the emulator's 266 MHz system clock.
static constexpr uint16_t PWM_WRAP = 1023;
static constexpr uint16_t PWM_CENTER = (PWM_WRAP + 1) / 2;
static constexpr uint8_t VOLUME_ADC_INPUT = 2;
static constexpr uint16_t PERFORMANCE_MUTE_ENTER = 650;
static constexpr uint16_t PERFORMANCE_MUTE_EXIT = 850;

// One block plays while up to two completed blocks wait behind it. Ownership
// prevents both overwriting queued audio and replaying an unrefilled block.
static uint32_t pwm_samples[AudioBlockQueue::capacity][AUDIO_SAMPLES];
// The PWM path runs for every one of the 22,050 samples per second. The
// Cortex-M0+ has no hardware divide, so precompute the nonlinear mappings
// that previously used divides in that hot loop.
static uint16_t pwm_level_lut[255];
static uint8_t soft_limit_lut[GameplayAudioCurve::max_magnitude + 1];
static int audio_dma_channel = -1;
static uint8_t saved_dma_irq_priority;
static AudioBlockQueue audio_queue;
static volatile uint32_t audio_queue_underruns;
static uint pwm_slice;
static uint16_t filtered_volume;
static bool volume_initialized;
static int32_t filtered_sample;
static bool performance_muted;

// Called from the completion IRQ or with interrupts disabled on core 0.
static void __time_critical_func(audio_start_ready_block)() {
  const int slot = audio_queue.start_next();
  if (slot < 0) return;
  dma_channel_set_read_addr(audio_dma_channel, pwm_samples[slot], false);
  dma_channel_set_trans_count(audio_dma_channel, AUDIO_SAMPLES, true);
}

static void __time_critical_func(audio_dma_complete_irq)() {
  if (audio_dma_channel < 0 ||
      !dma_irqn_get_channel_status(1, audio_dma_channel)) return;
  dma_channel_acknowledge_irq1(audio_dma_channel);
  audio_queue.complete();
  audio_start_ready_block();
  if (audio_queue.playing < 0) ++audio_queue_underruns;
}

// Stop without leaving a stale completion interrupt to free a reused block.
// The IRQ line itself is shared; only mask and acknowledge our DMA channel.
static void audio_stop_queue() {
  const uint32_t interrupts = save_and_disable_interrupts();
  dma_channel_set_irq1_enabled(audio_dma_channel, false);
  dma_channel_abort(audio_dma_channel);
  dma_channel_acknowledge_irq1(audio_dma_channel);
  audio_queue.reset();
  restore_interrupts(interrupts);
}

static void init_audio_lookup_tables() {
  for (uint16_t magnitude = 0; magnitude <= GameplayAudioCurve::max_magnitude;
      ++magnitude) {
    soft_limit_lut[magnitude] = GameplayAudioCurve::magnitude_level(magnitude);
  }
  for (int16_t sample = -127; sample <= 127; ++sample) {
    pwm_level_lut[sample + 127] = (uint16_t)(
        PWM_CENTER + (sample * (PWM_CENTER - 1)) / 127);
  }
}

static uint16_t read_volume_raw() {
  adc_select_input(VOLUME_ADC_INPUT);
  return 4095u - adc_read();
}

static uint16_t read_volume() {
  uint16_t raw = read_volume_raw();

  if (!volume_initialized) {
    filtered_volume = raw;
    volume_initialized = true;
  } else {
    filtered_volume = (filtered_volume * 3u + raw + 2u) >> 2;
  }

  if (filtered_volume <= 700u) return 0;
  if (filtered_volume >= 4000u) return 256;

  uint32_t volume =
      ((uint32_t)(filtered_volume - 700u) * 4000u) / (4000u - 700u);
  uint32_t tapered = (volume * volume) / 4000u;
  volume = (tapered * volume) / 4000u;
  return (volume * 256u + 2000u) / 4000u;
}

void crowpanel_audio_init() {
  adc_init();
  adc_gpio_init(CROWPANEL_VOLUME_PIN);

  gpio_set_function(CROWPANEL_SPEAKER_PIN, GPIO_FUNC_PWM);
  gpio_set_drive_strength(CROWPANEL_SPEAKER_PIN, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(CROWPANEL_SPEAKER_PIN, GPIO_SLEW_RATE_FAST);

  pwm_slice = pwm_gpio_to_slice_num(CROWPANEL_SPEAKER_PIN);
  // The boot jingle also uses this PWM slice.  Ensure its wrap interrupt is
  // disabled before gameplay switches the slice to DMA-paced PCM output.
  pwm_set_irq_enabled(pwm_slice, false);
  pwm_clear_irq(pwm_slice);
  pwm_config pwm_config = pwm_get_default_config();
  pwm_config_set_clkdiv_int(&pwm_config, 1);
  pwm_config_set_wrap(&pwm_config, PWM_WRAP);
  pwm_init(pwm_slice, &pwm_config, false);
  pwm_set_both_levels(pwm_slice, PWM_CENTER, PWM_CENTER);
  init_audio_lookup_tables();

  audio_dma_channel = dma_claim_unused_channel(true);

  uint32_t timer_divisor =
      (clock_get_hz(clk_sys) + AUDIO_SAMPLE_RATE / 2u) / AUDIO_SAMPLE_RATE;
  hard_assert(timer_divisor > 0 && timer_divisor <= UINT16_MAX);
  dma_timer_set_fraction(0, 1, (uint16_t)timer_divisor);
  dma_channel_config dma_config =
      dma_channel_get_default_config(audio_dma_channel);
  channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_config, true);
  channel_config_set_write_increment(&dma_config, false);
  channel_config_set_dreq(&dma_config, dma_get_timer_dreq(0));
  channel_config_set_high_priority(&dma_config, true);
  // Self-chain disables chaining. RP2040 reloads TRANS_COUNT on a chain
  // trigger but leaves READ_ADDR advanced; a late refill must never re-trigger
  // that channel and read beyond the original buffer.
  channel_config_set_chain_to(&dma_config, audio_dma_channel);
  dma_channel_configure(audio_dma_channel, &dma_config,
      &pwm_hw->slice[pwm_slice].cc, pwm_samples[0], 0, false);

  audio_queue.reset();
  audio_queue_underruns = 0;
  filtered_sample = 0;
  performance_muted = false;
  dma_channel_acknowledge_irq1(audio_dma_channel);
  irq_add_shared_handler(DMA_IRQ_1, audio_dma_complete_irq,
      PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  // A block boundary has only one sample period (~45 us) to rearm DMA.
  // Let this short SRAM handler preempt default-priority USB/tick work, but
  // never lower an existing higher priority on the shared IRQ line.
  saved_dma_irq_priority = (uint8_t)irq_get_priority(DMA_IRQ_1);
  if (saved_dma_irq_priority > 0x40u) irq_set_priority(DMA_IRQ_1, 0x40u);
  dma_channel_set_irq1_enabled(audio_dma_channel, true);
  irq_set_enabled(DMA_IRQ_1, true);

  pwm_set_enabled(pwm_slice, true);
}

void __time_critical_func(crowpanel_audio_submit)(const int16_t* stereo_samples,
    size_t stereo_bytes) {
  if (audio_dma_channel < 0 || performance_muted) return;

  int next_buffer;
  uint64_t wait_started = time_us_64();
  for (;;) {
    const uint32_t interrupts = save_and_disable_interrupts();
    next_buffer = audio_queue.reserve();
    restore_interrupts(interrupts);
    if (next_buffer >= 0) break;
    if (time_us_64() - wait_started > 100000u) {
      // Fault recovery also resets ownership; never abort just one side of a
      // live chain or overwrite a READY/PLAYING block to make room.
      audio_stop_queue();
      dma_channel_set_irq1_enabled(audio_dma_channel, true);
      wait_started = time_us_64();
    }
    tight_loop_contents();
  }

  uint32_t* const output = pwm_samples[next_buffer];

  const size_t frame_count = stereo_bytes / (sizeof(int16_t) * 2u);
  const size_t count = min(frame_count, (size_t)AUDIO_SAMPLES);
  const uint16_t volume = read_volume();

  for (size_t i = 0; i < count; ++i) {
    int32_t mono = ((int32_t)stereo_samples[i * 2] + stereo_samples[i * 2 + 1]) / 2;
    // Keep the existing smoothing. The precomputed curve adds a modest
    // game-wide loudness boost before softly limiting the combined channels.
    filtered_sample += (mono - filtered_sample) / 2;
    const int32_t scaled = (filtered_sample * volume * 5) >> 17;
    uint32_t magnitude = (uint32_t)(scaled < 0 ? -scaled : scaled);
    if (magnitude > GameplayAudioCurve::max_magnitude)
      magnitude = GameplayAudioCurve::max_magnitude;
    mono = soft_limit_lut[magnitude];
    if (scaled < 0) mono = -mono;
    const uint32_t level = pwm_level_lut[mono + 127];
    output[i] = level | (level << 16);
  }
  for (size_t i = count; i < AUDIO_SAMPLES; ++i) {
    output[i] = PWM_CENTER | ((uint32_t)PWM_CENTER << 16);
  }

  const uint32_t interrupts = save_and_disable_interrupts();
  if (audio_queue.playing < 0 && audio_queue.ready_count == 0) {
    // Only blend when starting from held PWM (startup or real underrun).
    // Leave normal block boundaries and the existing volume/gain untouched.
    const uint32_t held = pwm_hw->slice[pwm_slice].cc & 0xFFFFu;
    for (uint32_t i = 0; i < 8 && i < count; ++i) {
      const uint32_t level =
          (held * (7u - i) + (output[i] & 0xFFFFu) * (i + 1u)) >> 3;
      output[i] = level | (level << 16);
    }
  }
  const bool published = audio_queue.publish((uint8_t)next_buffer);
  hard_assert(published);
  audio_start_ready_block();
  restore_interrupts(interrupts);
}

bool crowpanel_audio_performance_muted() {
  if (audio_dma_channel < 0) return true;

  const uint16_t raw = read_volume_raw();
  if (!performance_muted && raw <= PERFORMANCE_MUTE_ENTER) {
    performance_muted = true;
    audio_stop_queue();
    pwm_set_both_levels(pwm_slice, PWM_CENTER, PWM_CENTER);
    filtered_sample = 0;
  } else if (performance_muted && raw >= PERFORMANCE_MUTE_EXIT) {
    performance_muted = false;
    // Prebuffer a fresh queue; no stale pre-mute samples can be replayed.
    dma_channel_set_irq1_enabled(audio_dma_channel, true);
    filtered_sample = 0;
  }
  return performance_muted;
}

void crowpanel_audio_shutdown() {
  if (audio_dma_channel >= 0) {
    audio_stop_queue();
    irq_remove_handler(DMA_IRQ_1, audio_dma_complete_irq);
    irq_set_priority(DMA_IRQ_1, saved_dma_irq_priority);
    dma_channel_unclaim(audio_dma_channel);
    audio_dma_channel = -1;
  }

  performance_muted = true;

  pwm_set_enabled(pwm_slice, false);
  gpio_set_function(CROWPANEL_SPEAKER_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(CROWPANEL_SPEAKER_PIN, GPIO_OUT);
  gpio_put(CROWPANEL_SPEAKER_PIN, 0);
}

void crowpanel_audio_screenshot_pause() {
  if (audio_dma_channel < 0) return;
  audio_stop_queue();
  pwm_set_both_levels(pwm_slice, PWM_CENTER, PWM_CENTER);
  filtered_sample = 0;
}

void crowpanel_audio_screenshot_resume(bool saved, int16_t* stereo_scratch) {
  if (audio_dma_channel < 0) return;
  if (crowpanel_audio_performance_muted()) return;
  dma_channel_set_irq1_enabled(audio_dma_channel, true);
  uint32_t noise = 0x43485249u;
  // A short shutter on success; a low double beep for SD errors/full gallery.
  // Use the gameplay PWM path, so volume/mute work and no timer is reconfigured.
  for (unsigned block = 0; block < 4; ++block) {
    for (unsigned i = 0; i < AUDIO_SAMPLES; ++i) {
      const unsigned index = block * AUDIO_SAMPLES + i;
      const int16_t sample = saved ? screenshot_shutter_sample(index, noise) :
          (index < 450 || (index >= 700 && index < 1150)) ?
              ((index / 44u) & 1u ? 4500 : -4500) : 0;
      stereo_scratch[i * 2] = sample;
      stereo_scratch[i * 2 + 1] = sample;
    }
    crowpanel_audio_submit(stereo_scratch, AUDIO_BUFFER_SIZE_BYTES);
  }
}
