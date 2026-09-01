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

#include "common.h"
#include "input.h"
#include "gb.h"
#include "chriscade_settings.h"
#include "chriscade_apps.h"

#if ENABLE_INPUT == INPUT_CROWPANEL
#include <hardware/adc.h>
#endif

#if ENABLE_SDCARD
#include "card_loader.h"
#endif

#if ENABLE_SOUND
#if defined(CROWPANEL_PWM_AUDIO)
#include "crowpanel_audio.h"
#else
#include "i2s-audio.h"
extern i2s_config_t i2s_config;
#endif
#endif

extern uint8_t gamma_int;

static uint8_t palette_selected = 0;

#if defined(CROWPANEL_ILI9341)
enum : uint8_t {
  TOUCH_GAME_B      = 1u << 0,
  TOUCH_GAME_A      = 1u << 1,
  TOUCH_GAME_SELECT = 1u << 2,
  TOUCH_GAME_START  = 1u << 3,
};

static volatile uint8_t gameplay_touch_buttons;

void pollGameplayTouch() {
  // The resistive panel reports one point. The upper 3/4 is split into B/A;
  // the lower 1/4 is split into Select/Start. A touch remains held until the
  // controller reports release on a later poll.
  uint16_t x = 0;
  uint16_t y = 0;
  uint8_t buttons = 0;
  if (!chriscade_gameplay_touch_enabled()) {
    __atomic_store_n(&gameplay_touch_buttons, 0, __ATOMIC_RELEASE);
    return;
  }
  gpio_put(SD_CS_PIN, 1);
  // The normal UI reader validates five samples with several millisecond
  // delays. Four large gameplay zones need only a pressure check and one
  // calibrated sample, which keeps touch overhead well below a frame.
  if (tft.getTouchRawZ() > 300) {
    tft.getTouchRaw(&x, &y);
    tft.convertRawXY(&x, &y);
  }
  if (x < DISPLAY_WIDTH && y < DISPLAY_HEIGHT && (x != 0 || y != 0)) {
    if (y < (DISPLAY_HEIGHT * 3u) / 4u)
      buttons = x < DISPLAY_WIDTH / 2u ? TOUCH_GAME_B : TOUCH_GAME_A;
    else
      buttons = x < DISPLAY_WIDTH / 2u ? TOUCH_GAME_SELECT : TOUCH_GAME_START;
  }
  __atomic_store_n(&gameplay_touch_buttons, buttons, __ATOMIC_RELEASE);
}
#else
void pollGameplayTouch() {}
#endif

static struct
{
  unsigned a : 1;
  unsigned b : 1;
  unsigned select : 1;
  unsigned start : 1;
  unsigned right : 1;
  unsigned left : 1;
  unsigned up : 1;
  unsigned down : 1;
} prev_joypad_bits;

#if ENABLE_INPUT == INPUT_PCF8574

static PCF8574 pcf8574(PCF8574_ADDR, PCF8574_SDA, PCF8574_SCL);

static void initJoypadI2CIoExpander() {
  for (int pin = 0; pin < 8; ++pin) {
    pcf8574.pinMode(pin, INPUT_PULLUP);
  }

  pcf8574.setLatency(5);
  
  if (!pcf8574.begin()){
		Serial.println("PCF8574 initialization failed");
    reset();
	}
}

bool readJoypad(uint8_t pin) {
  return pcf8574.digitalRead(pin);
}

#elif ENABLE_INPUT == INPUT_GPIO

static void initJoypadGpios() {
  gpio_set_function(PIN_UP, GPIO_FUNC_SIO);
  gpio_set_function(PIN_DOWN, GPIO_FUNC_SIO);
  gpio_set_function(PIN_LEFT, GPIO_FUNC_SIO);
  gpio_set_function(PIN_RIGHT, GPIO_FUNC_SIO);
  gpio_set_function(PIN_A, GPIO_FUNC_SIO);
  gpio_set_function(PIN_B, GPIO_FUNC_SIO);
  gpio_set_function(PIN_SELECT, GPIO_FUNC_SIO);
  gpio_set_function(PIN_START, GPIO_FUNC_SIO);

  gpio_set_dir(PIN_UP, false);
  gpio_set_dir(PIN_DOWN, false);
  gpio_set_dir(PIN_LEFT, false);
  gpio_set_dir(PIN_RIGHT, false);
  gpio_set_dir(PIN_A, false);
  gpio_set_dir(PIN_B, false);
  gpio_set_dir(PIN_SELECT, false);
  gpio_set_dir(PIN_START, false);

  gpio_pull_up(PIN_UP);
  gpio_pull_up(PIN_DOWN);
  gpio_pull_up(PIN_LEFT);
  gpio_pull_up(PIN_RIGHT);
  gpio_pull_up(PIN_A);
  gpio_pull_up(PIN_B);
  gpio_pull_up(PIN_SELECT);
  gpio_pull_up(PIN_START);  
}

bool readJoypad(uint8_t pin) {
  return gpio_get(pin);
}

#elif ENABLE_INPUT == INPUT_CROWPANEL

static void initCrowPanelJoypad() {
  const uint8_t buttonPins[] = {
    PIN_A, PIN_B, PIN_BUTTON_X, PIN_BUTTON_Y, PIN_START, PIN_SELECT
  };

  for (uint8_t pin : buttonPins) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, false);
    gpio_pull_up(pin);
  }

  gpio_set_function(PIN_LOW_POWER, GPIO_FUNC_SIO);
  gpio_set_dir(PIN_LOW_POWER, false);
  gpio_pull_up(PIN_LOW_POWER);

  adc_init();
  adc_gpio_init(PIN_JOYSTICK_X);
  adc_gpio_init(PIN_JOYSTICK_Y);
}

static uint16_t readCrowPanelAxis(uint8_t adcInput) {
  adc_select_input(adcInput);
  return adc_read();
}

bool readJoypad(uint8_t pin) {
  switch (pin) {
  case PIN_LEFT:
    return readCrowPanelAxis(0) >= JOYSTICK_LOW;
  case PIN_RIGHT:
    return readCrowPanelAxis(0) <= JOYSTICK_HIGH;
  case PIN_UP:
    return readCrowPanelAxis(1) <= JOYSTICK_HIGH;
  case PIN_DOWN:
    return readCrowPanelAxis(1) >= JOYSTICK_LOW;
  default:
    return gpio_get(pin);
  }
}

#endif

void initJoypad() {
  Serial.println("Init Joypad IOs ...");
#if ENABLE_INPUT == INPUT_PCF8574
  initJoypadI2CIoExpander();
#elif ENABLE_INPUT == INPUT_GPIO
  initJoypadGpios();
#elif ENABLE_INPUT == INPUT_CROWPANEL
  initCrowPanelJoypad();
#endif
  Serial.println("Joypad IOs initialized");
}

#define PALETTE_COUNT 15

void updatePalette() {
  if (palette_selected == 0) {
    char rom_title[17];
    auto_assign_palette(palette, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
  } else {
    manual_assign_palette(palette, palette_selected - 1);
  }
}

void nextPalette() {
  palette_selected = (palette_selected + 1) % PALETTE_COUNT;
  updatePalette();
}

void prevPalette() {
  palette_selected = (palette_selected + PALETTE_COUNT - 1) % PALETTE_COUNT;
  updatePalette();
}

void handleSerial() {
  static uint64_t start_time = time_us_64();

  /* Serial monitor commands */
  int input = Serial.read();
  if (input <= 0) {
    return;
  }

  switch (input) {
  case 'i':
    gb.direct.interlace = !gb.direct.interlace;
    break;

  case 'f':
    gb.direct.frame_skip = !gb.direct.frame_skip;
    break;

  case 'b': {
    uint64_t end_time;
    uint32_t diff;
    uint32_t fps;

    end_time = time_us_64();
    diff = end_time - start_time;
    fps = ((uint64_t)frames * 1000 * 1000) / diff;
    Serial.printf("Frames: %u\n"
                  "Time: %lu us\n"
                  "FPS: %lu\n",
        frames, diff, fps);
    Serial.flush();
    frames = 0;
    start_time = time_us_64();
    break;
  }

  case '\n':
  case '\r': {
    gb.direct.joypad_bits.start = 0;
    break;
  }

  case '\b': {
    gb.direct.joypad_bits.select = 0;
    break;
  }

  case '8': {
    gb.direct.joypad_bits.up = 0;
    break;
  }

  case '2': {
    gb.direct.joypad_bits.down = 0;
    break;
  }

  case '4': {
    gb.direct.joypad_bits.left = 0;
    break;
  }

  case '6': {
    gb.direct.joypad_bits.right = 0;
    break;
  }

  case 'z':
  case 'w': {
    gb.direct.joypad_bits.a = 0;
    break;
  }

  case 'x': {
    gb.direct.joypad_bits.b = 0;
    break;
  }

  case 'q':
    reset();

  case 'p':
    nextPalette();

  default:
    break;
  }
}

void handleJoypad() {
#if ENABLE_LCD && ENABLE_SDCARD && ENABLE_INPUT == INPUT_CROWPANEL
  // X is screenshot-only during gameplay; edge detection prevents a held
  // button from filling the gallery. X/Y remain unchanged in the app library.
  static bool screenshot_held = false;
  const bool screenshot_pressed = !readJoypad(PIN_BUTTON_X);
  if (screenshot_pressed && !screenshot_held) {
    screenshot_held = true;
#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
    crowpanel_audio_screenshot_pause();
#endif
    char title[17] = {};
    gb_get_rom_name(&gb, title);
    const bool saved = chriscade_capture_game_screen(title);
#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
    extern uint16_t* stream;
    crowpanel_audio_screenshot_resume(saved, (int16_t*)stream);
#else
    (void)saved;
#endif
  }
  screenshot_held = screenshot_pressed;
#endif
  /* Update buttons state */
  prev_joypad_bits.up = gb.direct.joypad_bits.up;
  prev_joypad_bits.down = gb.direct.joypad_bits.down;
  prev_joypad_bits.left = gb.direct.joypad_bits.left;
  prev_joypad_bits.right = gb.direct.joypad_bits.right;
  prev_joypad_bits.a = gb.direct.joypad_bits.a;
  prev_joypad_bits.b = gb.direct.joypad_bits.b;
  prev_joypad_bits.select = gb.direct.joypad_bits.select;
  prev_joypad_bits.start = gb.direct.joypad_bits.start;

#if ENABLE_INPUT != 0
  gb.direct.joypad_bits.up = readJoypad(PIN_UP);
  gb.direct.joypad_bits.down = readJoypad(PIN_DOWN);
  gb.direct.joypad_bits.left = readJoypad(PIN_LEFT);
  gb.direct.joypad_bits.right = readJoypad(PIN_RIGHT);
  gb.direct.joypad_bits.a = readJoypad(PIN_A);
  gb.direct.joypad_bits.b = readJoypad(PIN_B);
  gb.direct.joypad_bits.select = readJoypad(PIN_SELECT);
  gb.direct.joypad_bits.start = readJoypad(PIN_START);
#if defined(CROWPANEL_ILI9341)
  // Game Boy joypad bits are active-low. Touch augments the physical controls,
  // so either input source can hold a button down.
  const uint8_t touch =
      __atomic_load_n(&gameplay_touch_buttons, __ATOMIC_ACQUIRE);
  if (touch & TOUCH_GAME_A) gb.direct.joypad_bits.a = 0;
  if (touch & TOUCH_GAME_B) gb.direct.joypad_bits.b = 0;
  if (touch & TOUCH_GAME_SELECT) gb.direct.joypad_bits.select = 0;
  if (touch & TOUCH_GAME_START) gb.direct.joypad_bits.start = 0;
#endif
#endif

#if 0
  if (!gb.direct.joypad_bits.up) {
    if (!gb.direct.joypad_bits.select && prev_joypad_bits.select) {
      gamma_int++;
      Serial.printf("gamma: %d\n", gamma_int);
      get_colour_palette(palette, 0, 5);
    }
  }
  if (!gb.direct.joypad_bits.down) {
    if (!gb.direct.joypad_bits.select && prev_joypad_bits.select) {
      gamma_int--;
      get_colour_palette(palette, 0, 5);
      Serial.printf("gamma: %d\n", gamma_int);
    }
  }
#endif

  /* hotkeys (select + * combo)*/
  if (!gb.direct.joypad_bits.select) {
#if ENABLE_SOUND
#if !defined(CROWPANEL_PWM_AUDIO)
    if (!gb.direct.joypad_bits.up && prev_joypad_bits.up) {
      /* select + up: increase sound volume */
      i2s_increase_volume(&i2s_config);
    }
    if (!gb.direct.joypad_bits.down && prev_joypad_bits.down) {
      /* select + down: decrease sound volume */
      i2s_decrease_volume(&i2s_config);
    }
#endif
#endif
    if (!gb.direct.joypad_bits.right && prev_joypad_bits.right) {
      /* select + right: select the next manual color palette */
      nextPalette();
    }
    if (!gb.direct.joypad_bits.left && prev_joypad_bits.left) {
      /* select + left: select the previous manual color palette */
      prevPalette();
    }
    if (!gb.direct.joypad_bits.start && prev_joypad_bits.start) {
      /* select + start: save ram and resets to the game selection menu */
#if ENABLE_SDCARD
      // Stop the LCD/SPI owner before using the shared bus. The emulator
      // reboots immediately afterward, so no live bus handoff is required.
#if ENABLE_LCD
      multicore_reset_core1();
      sleep_ms(5);
#endif
      write_cart_ram_file(&gb);
#endif
      reset();
    }
    if (!gb.direct.joypad_bits.a && prev_joypad_bits.a) {
      /* select + A: enable/disable frame-skip => fast-forward */
      gb.direct.frame_skip = !gb.direct.frame_skip;
      Serial.printf("I gb.direct.frame_skip = %d\n", gb.direct.frame_skip);
    }
    if (!gb.direct.joypad_bits.b && prev_joypad_bits.b) {
      /* select + B: change scaling mode */
      scalingMode = (ScalingMode)(((int) scalingMode + 1) % ((int) ScalingMode::COUNT));
      union core_cmd cmd;
      cmd.cmd = CORE_CMD_IDLE_SET;
      multicore_fifo_push_blocking(cmd.full);
      Serial.printf("I Scaling mode: = %d\n", scalingMode);
    }
  }
}
