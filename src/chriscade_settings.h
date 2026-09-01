#pragma once

#include <stdint.h>

enum class ChriscadeBootSound : uint8_t {
  OFF = 0,
  ARCADE,
  SYNTHWAVE,
  CYBER,
  COSMIC,
  PIXEL,
  VICTORY,
};

enum class ChriscadeBootTheme : uint8_t {
  NEON = 0,
  SUNSET,
  ICE,
  MATRIX,
  ROYAL,
};

void chriscade_settings_load();
void chriscade_settings_menu();
void chriscade_brightness_init();
void chriscade_brightness_shutdown();

uint8_t chriscade_brightness_percent();
bool chriscade_status_led_enabled();
void chriscade_status_led_apply();
bool chriscade_gameplay_touch_enabled();
bool chriscade_main_menu_touch_enabled();
bool chriscade_quick_boot_enabled();
bool chriscade_random_boot_enabled();
ChriscadeBootSound chriscade_boot_sound();
ChriscadeBootTheme chriscade_boot_theme();

// Shared launcher/app palette. Every CHRISCADE page uses these so the chosen
// boot color theme carries through the full interface.
uint16_t chriscade_theme_bg();
uint16_t chriscade_theme_panel();
uint16_t chriscade_theme_primary();
uint16_t chriscade_theme_secondary();
uint16_t chriscade_theme_accent();
uint16_t chriscade_theme_canvas_at(int y);
uint16_t chriscade_theme_card(uint8_t kind);
