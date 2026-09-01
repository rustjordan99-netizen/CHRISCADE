#include <Arduino.h>

#include "card_loader.h"
#include "chriscade_apps.h"
#include "chriscade_backlight.pio.h"
#include "chriscade_boot.h"
#include "chriscade_settings.h"
#include ".generated/chriscade_build_version.h"
#include "touch_control_settings.h"
#include "common.h"
#include "input.h"

#include <hardware/clocks.h>
#include <hardware/adc.h>
#include <hardware/flash.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>
#include <pico/bootrom.h>

static constexpr uint32_t SETTINGS_MAGIC = 0x43485354u; // "CHST"
static constexpr uint8_t SETTINGS_VERSION = 5;
static constexpr char SETTINGS_FILE[] = "CHRIS.CFG";
static constexpr uint BACKLIGHT_PIN = 18;
static constexpr uint STATUS_LED_POWER_PIN = 6;
static constexpr uint32_t BACKLIGHT_PERIOD = 255;

struct ChriscadeSettingsFile {
  uint32_t magic;
  uint32_t checksum;
  uint8_t version;
  uint8_t brightness_index;
  uint8_t status_led_enabled;
  uint8_t boot_sound;
  uint8_t boot_theme;
  uint8_t random_boot;
  uint8_t quick_boot;
  uint8_t touch_controls; // v5 bit flags; v4 gameplay boolean in the same byte
};
static_assert(sizeof(ChriscadeSettingsFile) == 16, "Keep saved settings compatible");

static ChriscadeSettingsFile settings;
static uint8_t runtime_boot_sound;
static uint8_t runtime_boot_theme;
static PIO backlight_pio = pio1;
static int backlight_sm = -1;
static uint backlight_program_offset;

static uint32_t settings_checksum(const ChriscadeSettingsFile& value) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value.version);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(value) - offsetof(ChriscadeSettingsFile, version); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

static void settings_defaults() {
  memset(&settings, 0, sizeof(settings));
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  settings.brightness_index = 4;
  settings.status_led_enabled = 1;
  settings.touch_controls = TouchControlSettings::DEFAULTS;
  settings.quick_boot = 0;
  settings.boot_sound = (uint8_t)ChriscadeBootSound::ARCADE;
  settings.boot_theme = (uint8_t)ChriscadeBootTheme::NEON;
  settings.random_boot = 0;
  runtime_boot_sound = settings.boot_sound;
  runtime_boot_theme = settings.boot_theme;
  settings.checksum = settings_checksum(settings);
}

static bool settings_valid(const ChriscadeSettingsFile& value) {
  return value.magic == SETTINGS_MAGIC &&
      (value.version >= 1 && value.version <= SETTINGS_VERSION) &&
      value.brightness_index < 5 && value.status_led_enabled < 2 &&
      value.boot_sound <= (uint8_t)ChriscadeBootSound::VICTORY &&
      value.boot_theme <= (uint8_t)ChriscadeBootTheme::ROYAL &&
      value.random_boot < 2 &&
      value.quick_boot < 2 &&
      TouchControlSettings::valid(value.version, value.touch_controls) &&
      value.checksum == settings_checksum(value);
}

static bool settings_save() {
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  settings.checksum = settings_checksum(settings);
  FsFile file;
  auto scope = UseSDPinFunctionScope();
  if (!file.open(SETTINGS_FILE, O_WRONLY | O_CREAT | O_TRUNC)) return false;
  bool success = file.write(&settings, sizeof(settings)) == sizeof(settings);
  file.sync();
  file.close();
  return success;
}

void chriscade_settings_load() {
  settings_defaults();
  ChriscadeSettingsFile loaded = {};
  bool migrated = false;
  FsFile file;
  {
    auto scope = UseSDPinFunctionScope();
    if (file.open(SETTINGS_FILE, O_RDONLY)) {
      if (file.read(&loaded, sizeof(loaded)) == sizeof(loaded) && settings_valid(loaded)) {
        settings = loaded;
        if (settings.version == 1) {
          // Version 1 stored Quick Boot in the byte now used by Status LED.
          // Preserve that choice while defaulting the new green LED option on.
          settings.quick_boot = settings.status_led_enabled;
          settings.status_led_enabled = 1;
        } else if (settings.version == 2) {
          // Version 2 had the LED option but temporarily removed Quick Boot.
          settings.quick_boot = 0;
        }
        settings.touch_controls = TouchControlSettings::migrate(
            loaded.version, loaded.touch_controls);
        migrated = loaded.version != SETTINGS_VERSION;
        settings.version = SETTINGS_VERSION;
      }
      file.close();
    }
  }
  if (migrated) settings_save();
  runtime_boot_sound = settings.boot_sound;
  runtime_boot_theme = settings.boot_theme;
  if (settings.random_boot) {
    // ADC noise and the variable SD-mount time provide a fresh non-security
    // seed. Random boot always chooses an audible sound, never OFF.
    randomSeed(time_us_32() ^ ((uint32_t)adc_read() << 16));
    runtime_boot_sound = (uint8_t)random(1, 7);
    runtime_boot_theme = (uint8_t)random(0, 5);
  }
}

uint8_t chriscade_brightness_percent() {
  static const uint8_t values[] = {20, 40, 60, 80, 100};
  return values[settings.brightness_index];
}

bool chriscade_status_led_enabled() {
  return settings.status_led_enabled != 0;
}

void chriscade_status_led_apply() {
  gpio_put(STATUS_LED_POWER_PIN, settings.status_led_enabled ? 1 : 0);
  // GP7 is deliberately untouched: the battery monitor owns the red LED, so
  // a low-battery warning remains visible even when status lighting is off.
}

bool chriscade_gameplay_touch_enabled() {
  return TouchControlSettings::game_enabled(settings.touch_controls);
}

bool chriscade_main_menu_touch_enabled() {
  return TouchControlSettings::menu_enabled(settings.touch_controls);
}

bool chriscade_quick_boot_enabled() {
  return settings.quick_boot != 0;
}

bool chriscade_random_boot_enabled() {
  return settings.random_boot != 0;
}

ChriscadeBootSound chriscade_boot_sound() {
  return (ChriscadeBootSound)runtime_boot_sound;
}

ChriscadeBootTheme chriscade_boot_theme() {
  return (ChriscadeBootTheme)runtime_boot_theme;
}

uint16_t chriscade_theme_bg() {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565(14, 4, 16);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565(2, 8, 20);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(0, 8, 2);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565(10, 3, 24);
  return tft.color565(4, 5, 22);
}

uint16_t chriscade_theme_panel() {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565(38, 8, 45);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565(5, 20, 45);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(0, 24, 6);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565(31, 10, 58);
  return tft.color565(13, 16, 45);
}

uint16_t chriscade_theme_primary() {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565(255, 140, 40);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565(35, 210, 255);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(30, 255, 70);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565(255, 205, 65);
  return tft.color565(45, 225, 235);
}

uint16_t chriscade_theme_secondary() {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565(255, 55, 145);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565(110, 90, 255);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(135, 255, 80);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565(185, 75, 255);
  return tft.color565(245, 55, 175);
}

uint16_t chriscade_theme_accent() {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565(255, 180, 60);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565(105, 245, 225);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(210, 255, 220);
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565(255, 115, 175);
  return tft.color565(105, 255, 165);
}

uint16_t chriscade_theme_canvas_at(int y) {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET)
    return tft.color565((uint8_t)(14 + y / 22), (uint8_t)(4 + y / 75),
        (uint8_t)(16 + y / 16));
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE)
    return tft.color565((uint8_t)(2 + y / 80), (uint8_t)(8 + y / 28),
        (uint8_t)(20 + y / 10));
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX)
    return tft.color565(0, (uint8_t)(8 + y / 13), (uint8_t)(2 + y / 70));
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL)
    return tft.color565((uint8_t)(10 + y / 28), (uint8_t)(3 + y / 75),
        (uint8_t)(24 + y / 11));
  return tft.color565((uint8_t)(5 + y / 40),
      (uint8_t)(6 + y / 60), (uint8_t)(25 + y / 13));
}

uint16_t chriscade_theme_card(uint8_t kind) {
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::SUNSET) {
    if (kind == 0) return tft.color565(95, 43, 18);
    if (kind == 1) return tft.color565(92, 18, 68);
    return tft.color565(70, 28, 93);
  }
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ICE) {
    if (kind == 0) return tft.color565(8, 65, 95);
    if (kind == 1) return tft.color565(22, 50, 105);
    return tft.color565(20, 78, 98);
  }
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::MATRIX) {
    if (kind == 0) return tft.color565(5, 76, 23);
    if (kind == 1) return tft.color565(18, 92, 35);
    return tft.color565(8, 66, 45);
  }
  if (runtime_boot_theme == (uint8_t)ChriscadeBootTheme::ROYAL) {
    if (kind == 0) return tft.color565(92, 59, 18);
    if (kind == 1) return tft.color565(75, 22, 105);
    return tft.color565(49, 28, 105);
  }
  if (kind == 0) return tft.color565(12, 77, 89);
  if (kind == 1) return tft.color565(73, 27, 82);
  return tft.color565(31, 52, 92);
}

static void backlight_set_level() {
  if (backlight_sm < 0) return;
  static const uint8_t levels[] = {51, 102, 153, 204, 255};
  pio_sm_put(backlight_pio, (uint)backlight_sm, levels[settings.brightness_index]);
}

void chriscade_brightness_init() {
  if (backlight_sm < 0) {
    backlight_sm = pio_claim_unused_sm(backlight_pio, true);
    backlight_program_offset =
        pio_add_program(backlight_pio, &chriscade_backlight_pwm_program);
    pio_gpio_init(backlight_pio, BACKLIGHT_PIN);
    pio_sm_set_consecutive_pindirs(backlight_pio, (uint)backlight_sm,
        BACKLIGHT_PIN, 1, true);
    pio_sm_config config =
        chriscade_backlight_pwm_program_get_default_config(backlight_program_offset);
    sm_config_set_sideset_pins(&config, BACKLIGHT_PIN);
    sm_config_set_clkdiv(&config, 64.0f);
    pio_sm_init(backlight_pio, (uint)backlight_sm,
        backlight_program_offset, &config);
    pio_sm_put_blocking(backlight_pio, (uint)backlight_sm, BACKLIGHT_PERIOD);
    pio_sm_exec(backlight_pio, (uint)backlight_sm, pio_encode_pull(false, true));
    pio_sm_exec(backlight_pio, (uint)backlight_sm, pio_encode_out(pio_isr, 32));
    pio_sm_set_enabled(backlight_pio, (uint)backlight_sm, true);
  }
  // TFT_eSPI reconfigures GP18 as an ordinary output whenever it reinitializes
  // on core 1. Reattach the already-running PIO state machine afterward.
  pio_gpio_init(backlight_pio, BACKLIGHT_PIN);
  pio_sm_set_consecutive_pindirs(backlight_pio, (uint)backlight_sm,
      BACKLIGHT_PIN, 1, true);
  backlight_set_level();
}

void chriscade_brightness_shutdown() {
  if (backlight_sm >= 0)
    pio_sm_set_enabled(backlight_pio, (uint)backlight_sm, false);
  gpio_set_function(BACKLIGHT_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(BACKLIGHT_PIN, GPIO_OUT);
  gpio_put(BACKLIGHT_PIN, 0);
}

static uint16_t settings_bg() { return chriscade_theme_bg(); }
static uint16_t settings_panel() { return chriscade_theme_panel(); }
static uint16_t settings_cyan() { return chriscade_theme_primary(); }
static uint16_t settings_pink() { return chriscade_theme_secondary(); }
static uint16_t settings_green() { return chriscade_theme_accent(); }

static void settings_background(const char* title, bool clear = true) {
  const uint16_t bg = settings_bg();
  if (clear) tft.fillScreen(bg);
  for (int y = 0; y < DISPLAY_HEIGHT; y += 8)
    tft.fillRect(0, y, DISPLAY_WIDTH, 8, chriscade_theme_canvas_at(y));
  tft.drawCircle(299, 71, 46, settings_panel());
  tft.drawCircle(299, 71, 34, settings_pink());
  tft.setTextColor(settings_pink());
  tft.drawString("CHRISCADE", 14, 8, 2);
  tft.setTextColor(settings_cyan());
  tft.drawString("CHRISCADE", 10, 6, 2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("CHRISCADE", 12, 7, 2);
  tft.setTextColor(settings_pink());
  tft.drawString(title, 14, 29, 1);
  tft.fillCircle(7, 34, 2, settings_cyan());

  const uint16_t home = tft.color565(10, 18, 39);
  chriscade_header_button(258, 51, "HOME", home, settings_cyan());
}

static const char* setting_label(uint8_t row) {
  switch (row) {
    case 0: return "BRIGHTNESS";
    case 1: return "STATUS LED";
    case 2: return "IN GAME TOUCH CONTROLS";
    case 3: return "MAIN MENU TOUCH CONTROLS";
    case 4: return "QUICK BOOT";
    case 5: return "BOOT SOUND";
    case 6: return "BOOT COLORS";
    case 7: return "RANDOM BOOT";
    case 8: return "TOUCH CALIBRATION";
    case 9: return "SYSTEM INFO";
    case 10: return "USB BOOT MODE";
    default: return "RESTORE DEFAULTS";
  }
}

static const char* setting_value(uint8_t row) {
  static char brightness[8];
  if (row == 0) {
    snprintf(brightness, sizeof(brightness), "%u%%", chriscade_brightness_percent());
    return brightness;
  }
  if (row == 1) return settings.status_led_enabled ? "ON" : "OFF";
  if (row == 2) return chriscade_gameplay_touch_enabled() ? "ON" : "OFF";
  if (row == 3) return chriscade_main_menu_touch_enabled() ? "ON" : "OFF";
  if (row == 4) return settings.quick_boot ? "ON" : "OFF";
  if (row == 5) {
    switch ((ChriscadeBootSound)settings.boot_sound) {
      case ChriscadeBootSound::OFF: return "OFF";
      case ChriscadeBootSound::ARCADE: return "ARCADE";
      case ChriscadeBootSound::SYNTHWAVE: return "SYNTH";
      case ChriscadeBootSound::CYBER: return "CYBER";
      case ChriscadeBootSound::COSMIC: return "COSMIC";
      case ChriscadeBootSound::PIXEL: return "PIXEL";
      default: return "VICTORY";
    }
  }
  if (row == 6) {
    switch ((ChriscadeBootTheme)settings.boot_theme) {
      case ChriscadeBootTheme::NEON: return "NEON";
      case ChriscadeBootTheme::SUNSET: return "SUNSET";
      case ChriscadeBootTheme::ICE: return "ICE";
      case ChriscadeBootTheme::MATRIX: return "MATRIX";
      default: return "ROYAL";
    }
  }
  if (row == 7) return settings.random_boot ? "ON" : "OFF";
  if (row == 8) return "START";
  if (row == 9) return "VIEW";
  if (row == 10) return "ENTER";
  return "RESET";
}

static constexpr uint8_t SETTINGS_COUNT = 12;
static constexpr uint8_t SETTINGS_PAGE_COUNT = 3;
static constexpr uint8_t settings_page_first[] = {0, 4, 8};
static constexpr uint8_t settings_page_rows[] = {4, 4, 4};
static const char* const settings_page_names[] = {"DISPLAY", "BOOT", "SYSTEM"};

static uint8_t settings_page_for_row(uint8_t row) {
  return row < 4 ? 0 : row < 8 ? 1 : 2;
}

static void draw_setting_row(uint8_t row, bool selected) {
  const uint8_t page = settings_page_for_row(row);
  const int y = 67 + (row - settings_page_first[page]) * 32;
  const uint16_t bg = selected ? chriscade_theme_card(2) : settings_panel();
  tft.fillRoundRect(15, y, 290, 26, 11, bg);
  tft.drawRoundRect(15, y, 290, 26, 11,
      selected ? settings_cyan() : tft.color565(39, 46, 79));
  tft.fillCircle(28, y + 13, selected ? 4 : 2,
      row >= 10 ? settings_pink() : selected ? settings_green() : tft.color565(74, 82, 128));
  tft.setTextColor(selected ? TFT_WHITE : tft.color565(178, 202, 220), bg);
  tft.drawString(setting_label(row), 41, y + 9, 1);
  tft.setTextColor(row >= 10 ? settings_pink() : settings_cyan(), bg);
  tft.drawRightString(setting_value(row), 290, y + 9, 1);
}

static void draw_settings_menu(uint8_t selected, bool clear = true) {
  if (clear) settings_background("SETTINGS // PERSONALIZE");
  const uint8_t page = settings_page_for_row(selected);
  const uint16_t content = settings_panel();
  tft.fillRoundRect(9, 40, 302, 164, 17, content);
  tft.drawRoundRect(9, 40, 302, 164, 17, tft.color565(35, 48, 82));
  for (uint8_t tab = 0; tab < SETTINGS_PAGE_COUNT; ++tab) {
    const int x = 16 + tab * 97;
    const uint16_t tab_bg = tab == page ? chriscade_theme_card(2) : content;
    tft.fillRoundRect(x, 44, 91, 17, 8, tab_bg);
    tft.drawRoundRect(x, 44, 91, 17, 8,
        tab == page ? settings_cyan() : tft.color565(49, 57, 92));
    tft.setTextColor(tab == page ? TFT_WHITE : tft.color565(130, 160, 185), tab_bg);
    tft.drawCentreString(settings_page_names[tab], x + 45, 49, 1);
  }
  for (uint8_t local = 0; local < settings_page_rows[page]; ++local) {
    const uint8_t row = settings_page_first[page] + local;
    draw_setting_row(row, row == selected);
  }
  const uint16_t footer = settings_panel();
  tft.fillRoundRect(12, 213, 296, 23, 11, footer);
  tft.drawRoundRect(12, 213, 296, 23, 11, tft.color565(35, 48, 82));
  tft.setTextColor(tft.color565(160, 195, 215), footer);
  tft.drawCentreString("UP/DOWN CHOOSE  //  LEFT/RIGHT TABS", 160, 217, 1);
  tft.drawCentreString("A CHANGE / OPEN  //  B HOME", 160, 226, 1);
}

static void wait_settings_buttons_released() {
  while (!readJoypad(PIN_A) || !readJoypad(PIN_B) ||
         !readJoypad(PIN_SELECT) || !readJoypad(PIN_START)) {
    chriscade_power_poll(false);
    delay(5);
  }
}

static bool wait_for_back_or_home_touch() {
  wait_settings_buttons_released();
  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_A) || !readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      chriscade_ui_click(420);
      wait_settings_buttons_released();
      return true;
    }
    uint16_t x, y;
    bool pressed = chriscade_touch_read(&x, &y);
    if (pressed && !was_pressed && x >= 250 && y <= 40) {
      chriscade_ui_click(420);
      while (chriscade_touch_read(&x, &y)) delay(5);
      return true;
    }
    was_pressed = pressed;
    delay(5);
  }
}

static void show_system_info() {
  settings_background("SETTINGS // SYSTEM INFO");
  const uint16_t panel = settings_panel();
  tft.fillRoundRect(18, 50, 284, 151, 18, panel);
  tft.drawRoundRect(18, 50, 284, 151, 18, settings_cyan());
  tft.setTextColor(TFT_WHITE, panel);
  tft.drawString("CHRISCADE " CHRISCADE_BUILD_VERSION, 34, 64, 2);
  tft.setTextColor(tft.color565(160, 195, 220), panel);
  tft.drawString("DUAL CORE ARM CORTEX-M0+", 34, 88, 1);
  tft.drawString("264 KB SRAM  //  2 MB FLASH", 34, 106, 1);
  char line[40];
  snprintf(line, sizeof(line), "CURRENT CLOCK  %lu MHZ",
      (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
  tft.drawString(line, 34, 124, 1);
  unsigned battery = chriscade_battery_millivolts();
  if (battery) snprintf(line, sizeof(line), "BATTERY  %u.%02u V", battery / 1000u,
      (battery % 1000u) / 10u);
  else snprintf(line, sizeof(line), "BATTERY  EXTERNAL / USB");
  tft.drawString(line, 34, 142, 1);
  snprintf(line, sizeof(line), "DISPLAY  320x240  %u%%", chriscade_brightness_percent());
  tft.drawString(line, 34, 160, 1);
  tft.setTextColor(settings_green(), panel);
  tft.drawString("SD ROMS + SAVES + SETTINGS", 34, 180, 1);
  wait_for_back_or_home_touch();
}

static bool confirm_restore() {
  settings_background("SETTINGS // RESTORE DEFAULTS");
  const uint16_t panel = settings_panel();
  tft.fillRoundRect(27, 68, 266, 105, 19, panel);
  tft.drawRoundRect(27, 68, 266, 105, 19, settings_pink());
  tft.setTextColor(TFT_WHITE, panel);
  tft.drawCentreString("RESTORE CHRISCADE SETTINGS?", 160, 84, 2);
  tft.setTextColor(tft.color565(155, 190, 210), panel);
  tft.drawCentreString("GAMES AND SAVE DATA STAY SAFE", 160, 112, 1);
  tft.fillRoundRect(48, 140, 94, 25, 12, tft.color565(58, 18, 62));
  tft.drawRoundRect(48, 140, 94, 25, 12, settings_pink());
  tft.setTextColor(TFT_WHITE, tft.color565(58, 18, 62));
  tft.drawCentreString("A  RESTORE", 95, 148, 1);
  tft.fillRoundRect(178, 140, 94, 25, 12, tft.color565(10, 42, 61));
  tft.drawRoundRect(178, 140, 94, 25, 12, settings_cyan());
  tft.setTextColor(TFT_WHITE, tft.color565(10, 42, 61));
  tft.drawCentreString("B  CANCEL", 225, 148, 1);
  wait_settings_buttons_released();
  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_A)) { wait_settings_buttons_released(); return true; }
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_settings_buttons_released(); return false;
    }
    uint16_t x, y;
    bool pressed = chriscade_touch_read(&x, &y);
    if (pressed && !was_pressed && y >= 132 && y <= 174) {
      bool restore = x < 160;
      while (chriscade_touch_read(&x, &y)) delay(5);
      return restore;
    }
    was_pressed = pressed;
    delay(5);
  }
}

static bool confirm_usb_boot_mode() {
  settings_background("SETTINGS // USB BOOT MODE");
  const uint16_t panel = settings_panel();
  tft.fillRoundRect(27, 68, 266, 105, 19, panel);
  tft.drawRoundRect(27, 68, 266, 105, 19, settings_pink());
  tft.setTextColor(TFT_WHITE, panel);
  tft.drawCentreString("ENTER USB BOOT MODE?", 160, 84, 2);
  tft.setTextColor(tft.color565(155, 190, 210), panel);
  tft.drawCentreString("PICO WILL REBOOT AS RPI-RP2", 160, 108, 1);
  tft.drawCentreString("FLASH A UF2 FROM YOUR PC", 160, 122, 1);
  tft.fillRoundRect(48, 140, 94, 25, 12, tft.color565(58, 18, 62));
  tft.drawRoundRect(48, 140, 94, 25, 12, settings_pink());
  tft.setTextColor(TFT_WHITE, tft.color565(58, 18, 62));
  tft.drawCentreString("A  ENTER", 95, 148, 1);
  tft.fillRoundRect(178, 140, 94, 25, 12, tft.color565(10, 42, 61));
  tft.drawRoundRect(178, 140, 94, 25, 12, settings_cyan());
  tft.setTextColor(TFT_WHITE, tft.color565(10, 42, 61));
  tft.drawCentreString("B  CANCEL", 225, 148, 1);
  wait_settings_buttons_released();
  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_A)) { wait_settings_buttons_released(); return true; }
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_settings_buttons_released(); return false;
    }
    uint16_t x, y;
    bool pressed = chriscade_touch_read(&x, &y);
    if (pressed && !was_pressed && y >= 132 && y <= 174) {
      const bool enter = x < 160;
      while (chriscade_touch_read(&x, &y)) delay(5);
      return enter;
    }
    was_pressed = pressed;
    delay(5);
  }
}

static void change_setting(uint8_t row, int direction) {
  if (row == 0) {
    settings.brightness_index = (uint8_t)((settings.brightness_index +
        (direction > 0 ? 1 : 4)) % 5);
    backlight_set_level();
  } else if (row == 1) {
    settings.status_led_enabled ^= 1;
    chriscade_status_led_apply();
  } else if (row == 2) {
    settings.touch_controls ^= TouchControlSettings::GAME;
  } else if (row == 3) {
    settings.touch_controls ^= TouchControlSettings::MAIN_MENU;
  } else if (row == 4) {
    settings.quick_boot ^= 1;
  } else if (row == 5) {
    settings.boot_sound = (uint8_t)((settings.boot_sound +
        (direction > 0 ? 1 : 6)) % 7);
    runtime_boot_sound = settings.boot_sound;
  } else if (row == 6) {
    settings.boot_theme = (uint8_t)((settings.boot_theme +
        (direction > 0 ? 1 : 4)) % 5);
    runtime_boot_theme = settings.boot_theme;
  } else if (row == 7) {
    settings.random_boot ^= 1;
  }
  settings_save();
}

static void activate_setting(uint8_t row) {
  if (row < 8) {
    change_setting(row, 1);
    if (row == 5) {
      // Put the new name on-screen before the blocking sound preview begins.
      draw_setting_row(row, true);
      chriscade_preview_boot_sound(settings.boot_sound);
    }
    else chriscade_ui_click(620);
    return;
  }
  if (row == 8) {
    chriscade_ui_click(760);
    chriscade_touch_recalibrate();
  } else if (row == 9) {
    chriscade_ui_click(760);
    show_system_info();
  } else if (row == 10) {
    chriscade_ui_click(760);
    if (confirm_usb_boot_mode()) {
      // The RP2040 boot ROM exposes the normal RPI-RP2 UF2 drive. This call
      // never writes to the SD card, settings, ROM cache or save data.
      reset_usb_boot(0, 0);
    }
  } else {
    chriscade_ui_click(360);
    if (confirm_restore()) {
      settings_defaults();
      settings_save();
      backlight_set_level();
      chriscade_status_led_apply();
      chriscade_ui_click(1047);
    }
  }
}

void chriscade_settings_menu() {
  chriscade_touch_init();
  uint8_t selected = 0;
  draw_settings_menu(selected);
  wait_settings_buttons_released();
  bool touch_was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      chriscade_ui_click(360);
      wait_settings_buttons_released();
      return;
    }
    if (!readJoypad(PIN_DOWN)) {
      uint8_t old_selected = selected;
      selected = (uint8_t)((selected + 1) % SETTINGS_COUNT);
      chriscade_ui_click(560);
      if (settings_page_for_row(old_selected) == settings_page_for_row(selected)) {
        draw_setting_row(old_selected, false);
        draw_setting_row(selected, true);
      } else {
        draw_settings_menu(selected, false);
      }
      delay(145);
    } else if (!readJoypad(PIN_UP)) {
      uint8_t old_selected = selected;
      selected = (uint8_t)((selected + SETTINGS_COUNT - 1) % SETTINGS_COUNT);
      chriscade_ui_click(500);
      if (settings_page_for_row(old_selected) == settings_page_for_row(selected)) {
        draw_setting_row(old_selected, false);
        draw_setting_row(selected, true);
      } else {
        draw_settings_menu(selected, false);
      }
      delay(145);
    } else if (!readJoypad(PIN_RIGHT) || !readJoypad(PIN_LEFT)) {
      const uint8_t old_page = settings_page_for_row(selected);
      const bool moving_right = !readJoypad(PIN_RIGHT);
      const uint8_t page = (uint8_t)((old_page + SETTINGS_PAGE_COUNT +
          (moving_right ? 1 : -1)) % SETTINGS_PAGE_COUNT);
      selected = settings_page_first[page];
      chriscade_ui_click(moving_right ? 620 : 480);
      draw_settings_menu(selected, false);
      delay(145);
    } else if (!readJoypad(PIN_A)) {
      wait_settings_buttons_released();
      activate_setting(selected);
      if (selected == 6) draw_settings_menu(selected, true);
      else if (selected < 8) draw_setting_row(selected, true);
      else draw_settings_menu(selected);
      touch_was_pressed = false;
      continue;
    }

    uint16_t x, y;
    bool pressed = chriscade_touch_read(&x, &y);
    if (pressed && !touch_was_pressed) {
      if (x >= 250 && y <= 40) {
        chriscade_ui_click(360);
        while (chriscade_touch_read(&x, &y)) delay(5);
        return;
      }
      if (y >= 42 && y <= 63 && x >= 14 && x <= 309) {
        uint8_t page = min((uint8_t)2, (uint8_t)((x - 14) / 97));
        selected = settings_page_first[page];
        chriscade_ui_click(540 + page * 80);
        while (chriscade_touch_read(&x, &y)) delay(5);
        draw_settings_menu(selected, false);
        touch_was_pressed = false;
        continue;
      }
      const uint8_t page = settings_page_for_row(selected);
      int local = y >= 65 && y < 67 + settings_page_rows[page] * 32 ?
          ((int)y - 67) / 32 : -1;
      int row = local >= 0 ? settings_page_first[page] + local : -1;
      if (row >= 0 && row < SETTINGS_COUNT) {
        uint8_t old_selected = selected;
        selected = (uint8_t)row;
        while (chriscade_touch_read(&x, &y)) delay(5);
        if (selected < 8 && old_selected != selected)
          draw_setting_row(old_selected, false);
        activate_setting(selected);
        if (selected == 6) draw_settings_menu(selected, true);
        else if (selected < 8) draw_setting_row(selected, true);
        else draw_settings_menu(selected);
        touch_was_pressed = false;
        continue;
      }
    }
    touch_was_pressed = pressed;
    delay(5);
  }
}
