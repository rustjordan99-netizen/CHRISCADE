/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "gb.h"

// C Headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Project headers
#include "hedley.h"

#include "common.h"
#include "input.h"
#include "chriscade_boot.h"
#include "chriscade_settings.h"

#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <pico/time.h>

#if ENABLE_SDCARD
#include "card_loader.h"
#include "SdFat.h"
#ifdef ENABLE_USB_STORAGE_DEVICE
#include "msc.h"
#endif
#endif

#if ENABLE_SOUND
#if defined(CROWPANEL_PWM_AUDIO)
#include "crowpanel_audio.h"
#else
#include "i2s-audio.h"
#endif
#include "minigb_apu.h"

/**
 * Global variables for audio task
 * stream contains N=AUDIO_SAMPLES samples
 * each sample is 32 bits
 * 16 bits for the left channel + 16 bits for the right channel in stereo interleaved format)
 * This is intended to be played at AUDIO_SAMPLE_RATE Hz
 */
uint16_t* stream;

#if !defined(CROWPANEL_PWM_AUDIO)
i2s_config_t i2s_config;
#endif
#endif

uint_fast32_t frames = 0;

#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
static constexpr uint64_t GAMEBOY_FRAME_US = 16743;
static uint64_t muted_frame_deadline_us;
static bool muted_frame_pacing_active;

static void pace_muted_frame(bool muted, uint64_t frame_started_us) {
  if (!muted) {
    muted_frame_pacing_active = false;
    return;
  }

  uint64_t now = time_us_64();
  if (!muted_frame_pacing_active ||
      now > muted_frame_deadline_us + GAMEBOY_FRAME_US * 2u) {
    muted_frame_deadline_us = frame_started_us + GAMEBOY_FRAME_US;
    muted_frame_pacing_active = true;
  } else {
    muted_frame_deadline_us += GAMEBOY_FRAME_US;
  }

  if (now < muted_frame_deadline_us)
    busy_wait_until(from_us_since_boot(muted_frame_deadline_us));
}
#endif

void initSound();

void startEmulator() {
#if ENABLE_LCD
#if ENABLE_SDCARD
  Serial.println("Starting ROM file selector ...");
  rom_file_selector();
#endif
#endif

  sleep_ms(100);

  Serial.println("Init GB context ...");  
  initGbContext();

  /* Automatically assign a colour palette to the game */
  char rom_title[17];
  auto_assign_palette(palette, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));

#if ENABLE_SDCARD
  // The LCD and SD card share SPI1 on CrowPanel. Finish all SD access before
  // core 1 reinitializes and takes ownership of the display bus.
  Serial.println("Load save file ...");
  read_cart_ram_file(&gb);
#endif

#if defined(CROWPANEL_ILI9341)
  // 300 MHz was unstable on this board, so retain the proven 266 MHz clock.
  vreg_set_voltage(VREG_VOLTAGE_1_25);
  sleep_ms(10);
  const bool clock_set = set_sys_clock_khz(266000u, false);
  if (!clock_set) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
  }
#endif

  initSound();

#if ENABLE_LCD
  gb_init_lcd(&gb, &lcd_draw_line);

  /* Start Core1, which processes requests to the LCD. */
  Serial.println("Starting Core1 ...");
  multicore_launch_core1(core1_init);
#endif

#if ENABLE_SOUND
  // Initialize audio emulation
  Serial.println("Starting audio ...");
  audio_init();
#endif

  Serial.print("\n> ");
}

void reset(uint32_t sleepMs) {
  Serial.println("\nEmulation Ended");

  sleep_ms(sleepMs);

  /* stop lcd task running on core 1 */
  multicore_reset_core1();
  watchdog_reboot(0, 0, 0);
}

void halt() {
  while (true) {}
}

void initSound() {
#if ENABLE_SOUND
  Serial.println("Initialize Sound ...");

  // Allocate memory for the stream buffer
  stream = (uint16_t*) malloc(AUDIO_BUFFER_SIZE_BYTES);
  assert(stream != NULL);
  memset(stream, 0, AUDIO_BUFFER_SIZE_BYTES); // Zero out the stream buffer

#if defined(CROWPANEL_PWM_AUDIO)
  crowpanel_audio_init();
#else
  // Initialize I2S sound driver
  i2s_config = i2s_get_default_config();
  i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
  i2s_config.dma_trans_count = AUDIO_SAMPLES;
  i2s_config.data_pin = I2S_DIN_PIN,
	i2s_config.clock_pin_base = I2S_BCLK_LRC_PIN_BASE,
  i2s_volume(&i2s_config, 2);
  i2s_init(&i2s_config);
#endif

  Serial.println("Sound initialized");
#endif
}

void setup() {
#if defined(CROWPANEL_ILI9341)
  // Keep every other device on the shared SPI1 bus deselected during startup.
  gpio_init(TOUCH_CS_PIN);
  gpio_set_dir(TOUCH_CS_PIN, true);
  gpio_put(TOUCH_CS_PIN, true);
  gpio_init(SD_CS_PIN);
  gpio_set_dir(SD_CS_PIN, true);
  gpio_put(SD_CS_PIN, true);
#endif
#if ENABLE_LCD
  lcd_init(false);
#endif

  // Initialise USB serial connection for debugging.
  Serial.begin(115200);
  //while (!Serial) ;
  //delay(2000);

  chriscade_power_init();
#if ENABLE_SDCARD
  // Settings live beside ROMs and saves, so mount the card before rendering
  // the configurable startup sequence.
  init_sdcard();
  chriscade_settings_load();
  chriscade_status_led_apply();
#endif
#if ENABLE_LCD
  chriscade_brightness_init();
  chriscade_boot_screen();
#endif

#if ENABLE_SDCARD
#if ENABLE_USB_STORAGE_DEVICE
  initUsbStorageDevice();
#endif
#endif
  
  initJoypad();

  startEmulator();
}

void loop() {
#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
  const uint64_t frame_started_us = time_us_64();
#endif
  // Run the hot frame scheduler and CPU stepper together from SRAM so each
  // emulated instruction avoids crossing the flash-to-RAM veneer.
  runGbFrameFast();

  frames++;
#if ENABLE_SOUND
#if defined(CROWPANEL_PWM_AUDIO)
  // Fully skip APU mixing and PWM/DMA conversion at the physical minimum
  // volume setting. This trades muted-audio accuracy for more game CPU time.
  const bool audio_muted = crowpanel_audio_performance_muted();
  if (!audio_muted) {
    audio_callback(NULL, (int16_t*) stream, AUDIO_BUFFER_SIZE_BYTES);
    crowpanel_audio_submit((int16_t*) stream, AUDIO_BUFFER_SIZE_BYTES);
  }
  // Audio DMA normally supplies native-speed pacing.  Preserve that pacing
  // when the pot is at minimum so mute improves headroom without turbo-charging
  // Mario or other games.
  pace_muted_frame(audio_muted, frame_started_us);
#else
  audio_callback(NULL, (int16_t*) stream, AUDIO_BUFFER_SIZE_BYTES);
  i2s_dma_write(&i2s_config, (int16_t*) stream);
#endif
#endif

#if ENABLE_LCD
  // Start the next screen transfer only after the new PWM block is playing.
  // Scrolling can then consume shared SRAM/display bandwidth without delaying
  // preparation of the audio block that must cover that transfer.
  lcd_present_pending_frame();
#endif

  handleJoypad();
  chriscade_power_poll(true);
  handleSerial();
}

void error(String message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED);
  tft.drawString(message, 0, ERROR_TEXT_OFFSET, FONT_ID);
  Serial.printf("E %s\n", message.c_str());
  Serial.flush();
  reset(5000);
}
