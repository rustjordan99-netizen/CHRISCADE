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

 #if ENABLE_SDCARD

#include <Arduino.h>
#include "SdFat.h"
#include "hardware/flash.h"

#include "common.h"
#include "input.h"
#include "chriscade_boot.h"
#include "chriscade_apps.h"
#include "chriscade_settings.h"
#include "gb.h"
#include "card_loader.h"
#include "rom_file_actions.h"
#include "rom_entry_recovery.h"
#include "rom_ui_helpers.h"
#include "rom_usb_upload.h"

#define RAM_SAVENAME_LENGTH 17

SdFs sd;

// Keep the large ROM-loader workspaces out of the small core-0 stack. The
// original nested 5.5 KB filename table and 4 KB sector buffer could corrupt
// each other during a load, causing hangs or a false flash-mismatch error.
static uint8_t rom_sector_buffer[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static char rom_menu_filenames[FILES_PER_PAGE][MAX_PATH_LENGTH];
static_assert(MAX_PATH_LENGTH == RomFileActions::name_capacity,
    "ROM menu and safe filename buffers must agree");

gpio_function_t UseSDPinFunctionScope::sd_sck_pin_func = GPIO_FUNC_NULL;
gpio_function_t UseSDPinFunctionScope::sd_mosi_pin_func = GPIO_FUNC_NULL;

bool init_sdcard_hardware() {
  auto scope = UseSDPinFunctionScope();

  SD_SPI.setMISO(SD_MISO_PIN);
  SD_SPI.setMOSI(SD_MOSI_PIN);
  SD_SPI.setSCK(SD_SCK_PIN);
  bool success = sd.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(25), &SD_SPI));
  if (success) {
    UseSDPinFunctionScope::init(); 
  }
  return success;
}

void init_sdcard() {
  Serial.println("Initialize SD-Card ...");

  if (!init_sdcard_hardware()) {
    tft.setCursor(0, ERROR_TEXT_OFFSET, FONT_ID);
    tft.setTextColor(TFT_RED);
    sd.printSdError(&tft);
    sd.printSdError(&Serial);
    reset(5000);
  }
  if (sd.vol()->fatType() == 0) { // vol() and fatType() do not access the sd-card
    error("Can't find a valid FAT16/FAT32/exFAT partition");
  }
  
  Serial.printf("SD-Card initialized: FAT-Type=%d\n", sd.vol()->fatType());
}

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s* gb) {
  auto scope = UseSDPinFunctionScope();

  char filename[RAM_SAVENAME_LENGTH];
  uint_fast32_t save_size;
  FsFile file;

  gb_get_rom_name(gb, filename);
  save_size = gb_get_save_size(gb);
  if (save_size > 0) {
    if (!file.open(filename, O_RDONLY)) {
      Serial.printf("E f_open(%s) error\n", filename);
    } else {
      memset(ram, 0, sizeof(ram));
      file.read(ram, min((uint_fast32_t)file.size(), save_size));
    }

    if (!file.close()) {
      Serial.printf("E f_close error\n");
    }
  }

  Serial.printf("I read_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, save_size);
}

/**
 * Write a save file to the SD card
 */
bool write_cart_ram_file(struct gb_s* gb) {
  bool success = true;
  auto scope = UseSDPinFunctionScope();

  char filename[RAM_SAVENAME_LENGTH];
  uint_fast32_t save_size;
  FsFile file;

  gb_get_rom_name(gb, filename);
  save_size = gb_get_save_size(gb);
  if (save_size > 0) {
    if (!file.open(filename, O_WRONLY | O_CREAT | O_TRUNC)) {
      Serial.printf("E f_open(%s) error\n", filename);
      success = false;
    } else {
      if (file.write(ram, save_size) != save_size) {
        Serial.printf("E f_write(%s) error\n", filename);
        success = false;
      }

      if (!file.close()) {
        Serial.printf("E f_close error\n");
        success = false;
      }
    }
  }

  Serial.printf("I write_cart_ram_file(%s) %s (%lu bytes)\n",
      filename, success ? "COMPLETE" : "FAILED", save_size);
  return success;
}

bool write_rom_sector_to_flash(FsFile& file, uint8_t* buffer, uint32_t offset) {
  auto scope = UseSDPinFunctionScope();

  memset(buffer, 0xFF, FLASH_SECTOR_SIZE);
  int nread = file.read(buffer, FLASH_SECTOR_SIZE);
  if (nread < 0) {
    scope.close();
    error("Failed to read file!");    
  }

  if (nread == 0) {
    return false;
  }

  uint32_t flash_offset = ((uint32_t) &rom[offset]) - XIP_BASE;

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(flash_offset, FLASH_SECTOR_SIZE);
  flash_range_program(flash_offset, buffer, FLASH_SECTOR_SIZE);
  restore_interrupts(ints);

  /* Read back target region and check programming */
  if (memcmp(&rom[offset], buffer, FLASH_SECTOR_SIZE) != 0) {
    scope.close();
    error("Programming failed - Flash mismatch");
  }

  return true;
}

static void open_rom_file(FsFile& file, char* filename) {
  auto scope = UseSDPinFunctionScope();

  if (!file.open(filename, O_RDONLY)) {
    scope.close();
    error("Failed to open ROM: " + String(filename));
  }
}

static void close_rom_file(FsFile& file) {
  auto scope = UseSDPinFunctionScope();

  if (!file.close()) {
    Serial.printf("E f_close error\n");
  }
}

static uint16_t ui_bg() { return chriscade_theme_bg(); }
static uint16_t ui_panel() { return chriscade_theme_panel(); }
static uint16_t ui_cyan() { return chriscade_theme_primary(); }
static uint16_t ui_pink() { return chriscade_theme_secondary(); }
static uint16_t ui_green() { return chriscade_theme_accent(); }

static uint16_t ui_canvas_at(int y) {
  return chriscade_theme_canvas_at(y);
}

static void ui_plot_clipped(int x, int y, int left, int right, int top,
    int bottom, uint16_t color) {
  if (x >= left && x < right && y >= top && y < bottom)
    tft.drawPixel(x, y, color);
}

static void ui_draw_clipped_circle(int cx, int cy, int radius, int left,
    int right, int top, int bottom, uint16_t color) {
  int x = radius;
  int y = 0;
  int error = 1 - radius;
  while (x >= y) {
    ui_plot_clipped(cx + x, cy + y, left, right, top, bottom, color);
    ui_plot_clipped(cx + y, cy + x, left, right, top, bottom, color);
    ui_plot_clipped(cx - y, cy + x, left, right, top, bottom, color);
    ui_plot_clipped(cx - x, cy + y, left, right, top, bottom, color);
    ui_plot_clipped(cx - x, cy - y, left, right, top, bottom, color);
    ui_plot_clipped(cx - y, cy - x, left, right, top, bottom, color);
    ui_plot_clipped(cx + y, cy - x, left, right, top, bottom, color);
    ui_plot_clipped(cx + x, cy - y, left, right, top, bottom, color);
    ++y;
    if (error < 0) {
      error += 2 * y + 1;
    } else {
      --x;
      error += 2 * (y - x) + 1;
    }
  }
}

static void ui_draw_clipped_line(int x0, int y0, int x1, int y1, int left,
    int right, int top, int bottom, uint16_t color) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  int sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  while (true) {
    ui_plot_clipped(x0, y0, left, right, top, bottom, color);
    if (x0 == x1 && y0 == y1) break;
    int twice = error * 2;
    if (twice >= dy) { error += dy; x0 += sx; }
    if (twice <= dx) { error += dx; y0 += sy; }
  }
}

static void ui_draw_clipped_dot(int cx, int cy, int radius, int left,
    int right, int top, int bottom, uint16_t color) {
  for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
      if (x * x + y * y <= radius * radius)
        ui_plot_clipped(cx + x, cy + y, left, right, top, bottom, color);
}

static void ui_draw_background_decorations(int left, int right, int top,
    int bottom) {
  ui_draw_clipped_circle(296, 74, 51, left, right, top, bottom, ui_panel());
  ui_draw_clipped_circle(296, 74, 41, left, right, top, bottom, ui_pink());
  ui_draw_clipped_circle(296, 74, 31, left, right, top, bottom, ui_cyan());
  if (top <= 202 && bottom > 158) {
    ui_draw_clipped_line(0, 188, 74, 158, left, right, top, bottom, ui_cyan());
    ui_draw_clipped_line(0, 202, 105, 158, left, right, top, bottom, ui_pink());
  }
  for (int i = 0; i < 12; ++i) {
    int x = (i * 71 + 19) % DISPLAY_WIDTH;
    int y = 48 + (i * 43) % 154;
    int radius = (i % 4 == 0) ? 2 : 1;
    uint16_t color = (i % 3 == 0) ? ui_pink() :
        (i % 3 == 1) ? ui_cyan() : tft.color565(70, 92, 155);
    ui_draw_clipped_dot(x, y, radius, left, right, top, bottom, color);
  }
}

static void ui_draw_background() {
  const uint16_t bg = ui_bg();
  tft.fillScreen(bg);

  // Broad dusk gradient bands plus sparse neon points keep the screen lively
  // without a framebuffer or animated background task.
  for (int y = 0; y < DISPLAY_HEIGHT; y += 8) {
    tft.fillRect(0, y, DISPLAY_WIDTH, 8, ui_canvas_at(y));
  }
  ui_draw_background_decorations(0, DISPLAY_WIDTH, 0, DISPLAY_HEIGHT);
}

// Restore only a menu's changing content area.  Keeping the header and the
// rest of the background on-screen avoids a visible full-screen flash while
// switching between launcher cards.
static void ui_restore_background_rect(int left, int top, int width, int height) {
  const int right = min(left + width, DISPLAY_WIDTH);
  const int bottom = min(top + height, DISPLAY_HEIGHT);
  for (int y = top; y < bottom;) {
    const int band_y = (y / 8) * 8;
    const int next_band = min(bottom, band_y + 8);
    tft.fillRect(left, y, right - left, next_band - y, ui_canvas_at(band_y));
    y = next_band;
  }
  ui_draw_background_decorations(left, right, top, bottom);
}

static void ui_draw_header(const char* section) {
  const uint16_t cyan = ui_cyan();
  const uint16_t pink = ui_pink();
  const uint16_t green = ui_green();

  // A subtle chromatic offset echoes the cyan/pink CHRISCADE boot logo.
  tft.setTextColor(pink);
  tft.drawString("CHRISCADE", 14, 8, 2);
  tft.setTextColor(cyan);
  tft.drawString("CHRISCADE", 10, 6, 2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("CHRISCADE", 12, 7, 2);
  tft.setTextColor(pink);
  tft.drawString(section, 14, 28, 1);
  tft.fillCircle(7, 33, 2, cyan);

  unsigned battery_mv = chriscade_battery_millivolts();
  char battery_text[16];
  if (battery_mv) {
    snprintf(battery_text, sizeof(battery_text), "%u.%02uV",
        battery_mv / 1000u, (battery_mv % 1000u) / 10u);
  } else {
    strcpy(battery_text, "--.-V");
  }
  const uint16_t battery_color = chriscade_battery_is_low() ? TFT_RED : green;
  tft.fillRoundRect(244, 9, 64, 19, 9, tft.color565(10, 18, 39));
  tft.drawRoundRect(244, 9, 64, 19, 9, battery_color);
  tft.fillCircle(253, 18, 3, battery_color);
  tft.setTextColor(battery_color, tft.color565(10, 18, 39));
  tft.drawRightString(battery_text, 301, 14, 1);
}

static void ui_draw_footer(const char* primary, const char* secondary) {
  const uint16_t footer = ui_panel();
  tft.fillRoundRect(12, 210, DISPLAY_WIDTH - 24, 26, 12, footer);
  tft.drawRoundRect(12, 210, DISPLAY_WIDTH - 24, 26, 12,
      tft.color565(35, 48, 82));
  tft.fillCircle(24, 223, 3, ui_cyan());
  tft.fillCircle(DISPLAY_WIDTH - 24, 223, 3, ui_pink());
  tft.setTextColor(tft.color565(175, 210, 225), footer);
  tft.drawCentreString(primary, DISPLAY_WIDTH / 2, 214, 1);
  if (secondary && *secondary) {
    tft.setTextColor(tft.color565(90, 125, 155), footer);
    tft.drawCentreString(secondary, DISPLAY_WIDTH / 2, 225, 1);
  }
}

static char rom_sort_fold(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

enum class RomCartridgeIcon : uint8_t { GENERIC, POKEBALL, MUSHROOM, TRIFORCE };

static bool rom_name_contains(const char* filename, const char* word) {
  for (const char* start = filename; *start; ++start) {
    const char* text = start;
    const char* match = word;
    while (*text && *match && rom_sort_fold(*text) == rom_sort_fold(*match)) {
      ++text;
      ++match;
    }
    if (!*match) return true;
  }
  return false;
}

static RomCartridgeIcon rom_cartridge_icon(const char* filename) {
  if (rom_name_contains(filename, "pokemon")) return RomCartridgeIcon::POKEBALL;
  if (rom_name_contains(filename, "mario")) return RomCartridgeIcon::MUSHROOM;
  if (rom_name_contains(filename, "zelda")) return RomCartridgeIcon::TRIFORCE;
  return RomCartridgeIcon::GENERIC;
}

static void draw_pokeball(int cx, int cy, int radius) {
  const uint16_t colors[] = {0, tft.color565(30, 35, 49),
      tft.color565(235, 48, 62), TFT_WHITE};
  // A single circular mask defines fill AND outline. No square red cap or
  // mismatched fillCircle/drawCircle raster pixels can escape the black edge.
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius;) {
      const auto color = pokeball_pixel(x, y, radius);
      const int start = x++;
      while (x <= radius && pokeball_pixel(x, y, radius) == color) ++x;
      if (color != PokeballPixel::CLEAR)
        tft.drawFastHLine(cx + start, cy + y, x - start, colors[(uint8_t)color]);
    }
  }
}

static void draw_mushroom(int cx, int cy, int half_width) {
  const uint16_t colors[] = {0, tft.color565(30, 35, 49),
      tft.color565(235, 55, 55), TFT_WHITE};
  for (int y = -half_width; y <= half_width; ++y) {
    for (int x = -half_width; x <= half_width;) {
      const auto color = mushroom_pixel(x, y, half_width);
      const int start = x++;
      while (x <= half_width && mushroom_pixel(x, y, half_width) == color) ++x;
      if (color != MushroomPixel::CLEAR)
        tft.drawFastHLine(cx + start, cy + y, x - start, colors[(uint8_t)color]);
    }
  }
}

static void draw_loading_cartridge_symbol(const char* filename, uint16_t label_bg) {
  const int cx = DISPLAY_WIDTH / 2;
  const int cy = 121;
  switch (rom_cartridge_icon(filename)) {
    case RomCartridgeIcon::POKEBALL: {
      draw_pokeball(cx, cy, 13);
      break;
    }
    case RomCartridgeIcon::MUSHROOM: {
      draw_mushroom(cx, cy, 18);
      break;
    }
    case RomCartridgeIcon::TRIFORCE: {
      const uint16_t gold = tft.color565(255, 198, 45);
      tft.fillTriangle(cx, cy - 14, cx - 9, cy + 1, cx + 9, cy + 1, gold);
      tft.fillTriangle(cx - 9, cy + 1, cx - 18, cy + 16, cx, cy + 16, gold);
      tft.fillTriangle(cx + 9, cy + 1, cx, cy + 16, cx + 18, cy + 16, gold);
      break;
    }
    default:
      tft.setTextColor(TFT_WHITE, label_bg);
      tft.drawCentreString("GB", cx, 118, 2);
      break;
  }
}

/**
 * Load a .gb rom file in flash from the SD card
 */
void load_cart_rom_file(char* filename) {
  FsFile file;
  open_rom_file(file, filename);

  const uint32_t file_size = (uint32_t)file.size();
  if (file_size > (uint32_t)MAX_ROM_SIZE) {
    close_rom_file(file);
    error("ROM is too large for Pico flash: " + String(filename));
  }

  const uint16_t bg = ui_bg();
  const uint16_t panel = ui_panel();
  const uint16_t cyan = ui_cyan();
  const uint16_t pink = ui_pink();
  const uint16_t pale = tft.color565(215, 240, 248);
  ui_draw_background();
  ui_draw_header("FLASH LINK // CARTRIDGE");

  char loading_name[45];
  size_t name_len = strlen(filename);
  if (name_len > 4 && filename[name_len - 4] == '.') name_len -= 4;
  else if (name_len > 3 && filename[name_len - 3] == '.') name_len -= 3;
  if (name_len > sizeof(loading_name) - 1) name_len = sizeof(loading_name) - 1;
  memcpy(loading_name, filename, name_len);
  loading_name[name_len] = '\0';

  tft.fillRoundRect(22, 48, DISPLAY_WIDTH - 44, 38, 18, panel);
  tft.drawRoundRect(22, 48, DISPLAY_WIDTH - 44, 38, 18,
      tft.color565(44, 65, 105));
  tft.fillCircle(35, 67, 4, pink);
  tft.setTextColor(pale, panel);
  tft.drawCentreString(loading_name, DISPLAY_WIDTH / 2, 56, 1);
  tft.setTextColor(tft.color565(125, 145, 180), panel);
  char size_text[28];
  snprintf(size_text, sizeof(size_text), "%lu KB GAME ROM",
      (unsigned long)((file_size + 1023u) / 1024u));
  tft.drawCentreString(size_text, DISPLAY_WIDTH / 2, 70, 1);

  // A soft-edged cartridge floating inside a neon data orbit.
  tft.drawCircle(DISPLAY_WIDTH / 2, 132, 48, tft.color565(35, 55, 105));
  tft.drawCircle(DISPLAY_WIDTH / 2, 132, 42, tft.color565(62, 28, 105));
  tft.fillCircle(112, 107, 3, pink);
  tft.fillCircle(207, 151, 3, cyan);
  tft.fillRoundRect(116, 94, 88, 72, 14, tft.color565(28, 32, 70));
  tft.drawRoundRect(116, 94, 88, 72, 14, cyan);
  tft.fillRoundRect(127, 105, 66, 33, 9, tft.color565(8, 12, 34));
  tft.fillRect(127, 105, 66, 5, pink);
  draw_loading_cartridge_symbol(filename, tft.color565(8, 12, 34));
  for (int x = 126; x <= 190; x += 8)
    tft.fillRoundRect(x, 157, 5, 8, 2, tft.color565(255, 190, 55));

  tft.setTextColor(cyan);
  tft.drawCentreString("WRITING CARTRIDGE", DISPLAY_WIDTH / 2, 176, 1);
  tft.fillRoundRect(22, 194, 276, 13, 6, tft.color565(7, 10, 29));
  tft.drawRoundRect(22, 194, 276, 13, 6, tft.color565(50, 68, 108));
  tft.setTextColor(tft.color565(105, 125, 165));
  tft.drawString("SECURE LINK", 24, 181, 1);
  tft.setTextColor(pink);
  tft.drawRightString("0%", 296, 181, 1);

  Serial.printf("I Program target region...\n");

  chriscade_loading_sound_begin();
  chriscade_loading_sound_set(190);

  uint32_t offset = 0;
  uint8_t last_progress = 255;
  while (write_rom_sector_to_flash(file, rom_sector_buffer, offset)) {
    /* Next sector */
    offset += FLASH_SECTOR_SIZE;
    uint32_t complete = min(offset, file_size);
    uint8_t progress = file_size ? (uint8_t)((complete * 100u) / file_size) : 100;
    if (progress != last_progress) {
      const int bar_width = (progress * 270) / 100;
      if (bar_width > 0)
        tft.fillRoundRect(25, 197, bar_width, 7, 3, cyan);
      if (bar_width > 12)
        tft.drawFastHLine(29, 198, bar_width - 10,
            tft.color565(190, 255, 250));
      char percent_text[8];
      snprintf(percent_text, sizeof(percent_text), "%u%%", progress);
      tft.fillRoundRect(264, 178, 34, 12, 6, bg);
      tft.setTextColor(pink, bg);
      tft.drawRightString(percent_text, 296, 181, 1);
      chriscade_loading_sound_set(190u + (unsigned)progress * 2u);
      last_progress = progress;
    }
    chriscade_power_poll(false);
  }

  close_rom_file(file);

  tft.fillRoundRect(72, 174, 176, 17, 8, tft.color565(8, 42, 37));
  tft.setTextColor(ui_green(), tft.color565(8, 42, 37));
  tft.drawCentreString("SYNC COMPLETE // BOOTING", DISPLAY_WIDTH / 2, 179, 1);
  tft.fillRoundRect(25, 197, 270, 7, 3, ui_green());
  chriscade_loading_sound_set(784); delay(55);
  chriscade_loading_sound_set(1175); delay(90);
  chriscade_loading_sound_end();
  delay(100);

  Serial.printf("I load_cart_rom_file(%s) COMPLETE\n", filename);
}

static int rom_filename_compare(const char* lhs, const char* rhs) {
  while (*lhs && *rhs) {
    unsigned char a = (unsigned char)rom_sort_fold(*lhs++);
    unsigned char b = (unsigned char)rom_sort_fold(*rhs++);
    if (a != b) return a < b ? -1 : 1;
  }
  if (*lhs) return 1;
  if (*rhs) return -1;
  return 0;
}

static bool rom_filename_supported(const char* filename) {
  return RomFileActions::extension(filename) != nullptr;
}

// Find the alphabetically next ROM with one directory scan. Repeating this
// for the visible page is slower than holding every filename in RAM, but the
// scan only happens in the menu and keeps gameplay memory available to GB.
static bool find_next_rom_filename(const char* after, char* next, bool backwards = false) {
  FsFile dir;
  FsFile file;
  // Menu-only scratch, separate from rename text/destination and scrolling's
  // output name. Keep nested directory scans off the 2 KB core-0 stack.
  char* candidate = reinterpret_cast<char*>(rom_sector_buffer) + 4 * MAX_PATH_LENGTH;
  bool found = false;

  if (!dir.open("/")) error("Failed to open root dir");
  while (file.openNext(&dir, O_RDONLY)) {
    const bool is_directory = file.isDir();
    candidate[0] = '\0';
    file.getName(candidate, MAX_PATH_LENGTH);
    file.close();

    if (is_directory || !rom_filename_supported(candidate)) continue;
    const int from_anchor = rom_filename_compare(candidate, after);
    if (*after && (backwards ? from_anchor >= 0 : from_anchor <= 0)) continue;
    const int from_best = found ? rom_filename_compare(candidate, next) : 0;
    if (!found || (backwards ? from_best > 0 : from_best < 0)) {
      strncpy(next, candidate, MAX_PATH_LENGTH - 1);
      next[MAX_PATH_LENGTH - 1] = '\0';
      found = true;
    }
  }
  dir.close();
  return found;
}

static uint16_t read_file_page_from_card(char filename[FILES_PER_PAGE][MAX_PATH_LENGTH], uint16_t first_file) {
  auto scope = UseSDPinFunctionScope();

  for (uint8_t ifile = 0; ifile < FILES_PER_PAGE; ++ifile)
    filename[ifile][0] = '\0';

  const uint32_t last_file = first_file + FILES_PER_PAGE;
  char* previous = reinterpret_cast<char*>(rom_sector_buffer) + 2 * MAX_PATH_LENGTH;
  char* next = reinterpret_cast<char*>(rom_sector_buffer) + 3 * MAX_PATH_LENGTH;
  previous[0] = '\0';
  uint16_t page_files = 0;

  for (uint32_t sorted_index = 0; sorted_index < last_file; ++sorted_index) {
    next[0] = '\0';
    if (!find_next_rom_filename(previous, next)) break;
    if (sorted_index >= first_file) {
      strcpy(filename[page_files++], next);
    }
    strcpy(previous, next);
  }

  return page_files;
}

static constexpr int ROM_LIST_TOP = 48;
static constexpr int ROM_ENTRY_HEIGHT = 20;

static void rom_display_label(const char* filename, char* label, size_t label_size) {
  size_t length = strlen(filename);
  if (length > 4 && filename[length - 4] == '.') length -= 4;
  else if (length > 3 && filename[length - 3] == '.') length -= 3;
  bool clipped = length >= label_size;
  if (clipped) length = label_size - 1;
  memcpy(label, filename, length);
  label[length] = '\0';
  if (clipped && label_size > 4) {
    label[label_size - 4] = '.';
    label[label_size - 3] = '.';
    label[label_size - 2] = '.';
    label[label_size - 1] = '\0';
  }
}

static void draw_rom_cartridge(const char* filename, int y, bool selected) {
  const int cx = 26;
  const int cy = y + 9;
  const uint16_t shell = selected ? tft.color565(220, 238, 236) :
      tft.color565(105, 125, 151);
  const uint16_t edge = selected ? TFT_WHITE : tft.color565(145, 165, 188);
  tft.fillRoundRect(17, y + 2, 18, 14, 3, shell);
  tft.drawRoundRect(17, y + 2, 18, 14, 3, edge);
  tft.drawFastHLine(21, y + 4, 10, tft.color565(55, 70, 95));

  switch (rom_cartridge_icon(filename)) {
    case RomCartridgeIcon::POKEBALL: {
      draw_pokeball(cx, cy + 1, 4);
      break;
    }
    case RomCartridgeIcon::MUSHROOM: {
      draw_mushroom(cx, cy, 6);
      break;
    }
    case RomCartridgeIcon::TRIFORCE: {
      const uint16_t gold = tft.color565(255, 198, 45);
      tft.fillTriangle(cx, cy - 4, cx - 3, cy + 1, cx + 3, cy + 1, gold);
      tft.fillTriangle(cx - 3, cy + 1, cx - 6, cy + 6, cx, cy + 6, gold);
      tft.fillTriangle(cx + 3, cy + 1, cx, cy + 6, cx + 6, cy + 6, gold);
      break;
    }
    default:
      tft.fillRect(cx - 4, cy - 1, 8, 5,
          selected ? ui_cyan() : tft.color565(55, 80, 105));
      tft.drawFastVLine(cx - 2, cy, 3, shell);
      tft.drawFastVLine(cx + 1, cy, 3, shell);
      break;
  }
}

void print_file_entry(char* s, uint8_t index, uint8_t num_files, bool selected = false) {
  if (num_files == 0) {
    const uint16_t panel = ui_panel();
    tft.fillRoundRect(25, 82, DISPLAY_WIDTH - 50, 72, 18, panel);
    tft.drawRoundRect(25, 82, DISPLAY_WIDTH - 50, 72, 18, ui_cyan());
    tft.fillCircle(53, 104, 7, ui_pink());
    tft.drawCircle(53, 104, 11, tft.color565(90, 35, 105));
    tft.setTextColor(tft.color565(205, 230, 240), panel);
    tft.drawCentreString("YOUR LIBRARY IS EMPTY", DISPLAY_WIDTH / 2, 99, 2);
    tft.setTextColor(tft.color565(105, 140, 170), panel);
    tft.drawCentreString("DROP .GB / .GBC / .GBZ ON THE SD CARD", DISPLAY_WIDTH / 2, 128, 1);
    return;
  }

  const int y = ROM_LIST_TOP + index * ROM_ENTRY_HEIGHT;
  const uint16_t canvas = ui_canvas_at(y);
  const uint16_t bg = selected ? chriscade_theme_card(0) : ui_panel();
  tft.fillRect(8, y, DISPLAY_WIDTH - 16, ROM_ENTRY_HEIGHT - 1, canvas);
  tft.fillRoundRect(14, y, DISPLAY_WIDTH - 28, ROM_ENTRY_HEIGHT - 2, 8, bg);
  if (selected) {
    tft.drawRoundRect(14, y, DISPLAY_WIDTH - 28, ROM_ENTRY_HEIGHT - 2, 8,
        ui_cyan());
    tft.fillCircle(DISPLAY_WIDTH - 27, y + 9, 3, ui_green());
  }
  draw_rom_cartridge(s, y, selected);

  char label[45];
  rom_display_label(s, label, sizeof(label));
  tft.setTextColor(selected ? TFT_WHITE : tft.color565(175, 200, 220), bg);
  tft.drawString(label, 42, y + 5, FONT_ID);
}

/**
 * Function used by the rom file selector to display one page of .gb rom files
 */
static void draw_library_actions(bool add_selected) {
  const uint16_t bg = add_selected ? chriscade_theme_card(0) : ui_panel();
  tft.fillRoundRect(12, 210, 112, 26, 11, bg);
  tft.drawRoundRect(12, 210, 112, 26, 11, add_selected ? ui_green() : ui_cyan());
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString("+ ADD GAME", 68, 219, 1);
  tft.fillRoundRect(132, 210, 176, 26, 11, chriscade_theme_card(2));
  tft.drawRoundRect(132, 210, 176, 26, 11, ui_cyan());
  tft.setTextColor(TFT_WHITE, chriscade_theme_card(2));
  tft.drawCentreString("SELECT // SETTINGS", 220, 219, 1);
}

static void draw_library_range(uint16_t first, uint16_t count) {
  char text[24];
  snprintf(text, sizeof(text), "%u-%u", count ? first + 1 : 0, first + count);
  tft.fillRoundRect(252, 30, 56, 14, 7, ui_panel());
  tft.setTextColor(TFT_WHITE, ui_panel());
  tft.drawCentreString(text, 280, 33, 1);
}

static uint16_t rom_file_selector_display_page(char filename[FILES_PER_PAGE][MAX_PATH_LENGTH], uint16_t first_file) {
  uint16_t num_files = read_file_page_from_card(filename, first_file);

  ui_draw_background();
  ui_draw_header("GAME LIBRARY // A-Z");

  draw_library_range(first_file, num_files);

  // A real touch target as well as a physical SELECT shortcut. It sits below
  // all eight game rows, so tapping settings cannot accidentally load a ROM.
  draw_library_actions(num_files == 0);
  for (uint8_t ifile = 0; ifile < num_files; ifile++) {
    print_file_entry(filename[ifile], ifile, num_files);
  }

  return num_files;
}

/**
 * The ROM selector displays pages of up to FILES_PER_PAGE rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the root directory of the SD card
 */
static void wait_launcher_buttons_released() {
  while (!readJoypad(PIN_A) || !readJoypad(PIN_B) ||
         !readJoypad(PIN_SELECT) || !readJoypad(PIN_START)) {
    chriscade_power_poll(false);
    sleep_ms(5);
  }
}

static void rom_wait_input_released() {
  uint16_t x, y;
  while (!readJoypad(PIN_A) || !readJoypad(PIN_B) ||
      !readJoypad(PIN_SELECT) || !readJoypad(PIN_START) ||
      chriscade_touch_read(&x, &y)) {
    chriscade_power_poll(false);
    sleep_ms(5);
  }
}

enum class RomMenuEvent { NONE, A, BACK, START, UP, DOWN, LEFT, RIGHT, TOUCH };
static RomMenuEvent rom_menu_input(uint16_t* x, uint16_t* y) {
  chriscade_power_poll(false);
  RomMenuEvent event = RomMenuEvent::NONE;
  if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) event = RomMenuEvent::BACK;
  else if (!readJoypad(PIN_A)) event = RomMenuEvent::A;
  else if (!readJoypad(PIN_START)) event = RomMenuEvent::START;
  if (event != RomMenuEvent::NONE) {
    rom_wait_input_released();
    return event;
  }
  if (!readJoypad(PIN_UP)) event = RomMenuEvent::UP;
  else if (!readJoypad(PIN_DOWN)) event = RomMenuEvent::DOWN;
  else if (!readJoypad(PIN_LEFT)) event = RomMenuEvent::LEFT;
  else if (!readJoypad(PIN_RIGHT)) event = RomMenuEvent::RIGHT;
  if (event != RomMenuEvent::NONE) { sleep_ms(130); return event; }
  if (chriscade_touch_read(x, y)) {
    rom_wait_input_released();
    return RomMenuEvent::TOUCH;
  }
  sleep_ms(5);
  return RomMenuEvent::NONE;
}

static void rom_action_button(int x, int y, int w, int h, const char* text,
    bool selected, bool danger = false) {
  const uint16_t bg = selected ? chriscade_theme_card(danger ? 1 : 2) : ui_panel();
  tft.fillRoundRect(x, y, w, h, min(10, h / 2), bg);
  tft.drawRoundRect(x, y, w, h, min(10, h / 2),
      selected ? (danger ? ui_pink() : ui_cyan()) : ui_panel());
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString(text, x + w / 2, y + (h - 8) / 2, 1);
}

static void rom_settings_filename(const char* filename) {
  tft.fillRoundRect(12, 46, 296, 67, 12, ui_panel());
  tft.drawRoundRect(12, 46, 296, 67, 12, ui_cyan());
  tft.setTextColor(TFT_WHITE, ui_panel());
  // Show the full selected filename (up to 255 bytes), not an ambiguous title
  // shortened before a region/revision suffix. No path prefix is accepted.
  for (int line = 0; *filename && line < 6; ++line) {
    char text[47];
    const size_t count = min(strlen(filename), sizeof(text) - 1);
    memcpy(text, filename, count);
    text[count] = '\0';
    tft.drawString(text, 20, 53 + line * 9, 1);
    filename += count;
  }
}

static const char* rom_action_message(RomFileActions::Result result) {
  using RomFileActions::Result;
  switch (result) {
    case Result::OK: return "DONE // SAVE DATA KEPT";
    case Result::INVALID_NAME: return "INVALID NAME // USE A GAME TITLE";
    case Result::NOT_FOUND: return "GAME NOT FOUND // CHECK SD CARD";
    case Result::READ_ONLY: return "FILE IS READ ONLY";
    case Result::EXISTS: return "NAME ALREADY USED // CHOOSE ANOTHER";
    case Result::UNCHANGED: return "SAME NAME (IGNORES LETTER CASE)";
    case Result::REMOVED_DAMAGED: return "BROKEN ENTRY REMOVED // SAVES KEPT";
    default: return "SD ERROR // CHECK THE CARD";
  }
}

// Called only after remove() failed for a previously verified regular ROM.
// A broken FAT chain can make normal deletion fail before unlinking the name.
// Keep unknown clusters allocated; remove only this exact FAT32 root entry
// and its checksum-verified LFN entries. Never modify the FAT or another file.
static RomFileActions::Result rom_remove_damaged_entry(const char* filename) {
  using RomFileActions::Result;
  using RomEntryRecovery::Entry;
  if (!RomFileActions::valid_rom(filename) || sd.vol()->fatType() != 32)
    return Result::SD_ERROR;
  static_assert(sizeof(RomEntryRecovery::Workspace) <= sizeof(rom_sector_buffer),
      "Deletion recovery must reuse the existing menu workspace");
  auto& work = *reinterpret_cast<RomEntryRecovery::Workspace*>(rom_sector_buffer);
  FsFile root, file;
  if (!root.open("/", O_RDONLY)) return Result::SD_ERROR;
  if (!file.open(&root, filename, O_RDONLY)) {
    // remove() can unlink the short entry, then fail cleaning its long name.
    // Report that case only after a complete, error-free directory scan.
    root.rewind();
    char* candidate = reinterpret_cast<char*>(work.before);
    while (file.openNext(&root, O_RDONLY)) {
      const bool named = file.getName(candidate, MAX_PATH_LENGTH) > 0;
      const bool found = named && RomFileActions::same_name(candidate, filename);
      if (!file.close() || !named || found) return Result::SD_ERROR;
    }
    if (root.getError() || !root.close()) return Result::SD_ERROR;
    return Result::REMOVED_DAMAGED;
  }
  char* opened_name = reinterpret_cast<char*>(work.before);
  if (!file.isFile() || !file.getName(opened_name, MAX_PATH_LENGTH) ||
      !RomFileActions::same_name(filename, opened_name)) return Result::SD_ERROR;
  const uint32_t target_index = file.dirIndex();
  const uint32_t bytes_per_cluster = sd.vol()->bytesPerCluster();
  const uint32_t clusters = sd.vol()->clusterCount();
  const uint32_t data_start = sd.vol()->dataStartSector();
  if (bytes_per_cluster < 512 || (bytes_per_cluster & (bytes_per_cluster - 1)))
    return Result::SD_ERROR;
  const uint32_t sectors_per_cluster = bytes_per_cluster / 512;
  const uint64_t end = (uint64_t)data_start + (uint64_t)clusters * sectors_per_cluster;
  const uint32_t card_sectors = sd.card()->sectorCount();
  if (!card_sectors || end > card_sectors || end > UINT32_MAX) return Result::SD_ERROR;

  auto snapshot = [&](uint32_t index, Entry& entry) {
    if (index > 65535 || !root.seekSet(index * 32u) ||
        root.read(entry.bytes, 32) != 32) return false;
    const uint32_t cluster = root.curCluster();
    if (cluster < 2 || cluster - 2 >= clusters) return false;
    entry.sector = data_start + (cluster - 2) * sectors_per_cluster +
        ((index * 32u) % bytes_per_cluster) / 512;
    entry.offset = (index * 32u) % 512;
    return entry.sector >= data_start && entry.sector < end;
  };
  if (!snapshot(target_index, work.entries[0])) return Result::SD_ERROR;
  const uint8_t* entry = work.entries[0].bytes;
  if (!RomEntryRecovery::rom_entry(entry)) return Result::SD_ERROR;
  const uint32_t first_cluster = (uint32_t)entry[26] | ((uint32_t)entry[27] << 8) |
      ((uint32_t)entry[20] << 16) | ((uint32_t)entry[21] << 24);
  const uint32_t file_size = (uint32_t)entry[28] | ((uint32_t)entry[29] << 8) |
      ((uint32_t)entry[30] << 16) | ((uint32_t)entry[31] << 24);
  if (first_cluster != file.firstCluster() || file_size != file.fileSize())
    return Result::SD_ERROR;
  char short_name[13];
  unsigned length = 0;
  for (unsigned i = 0; i < 8 && entry[i] != ' '; ++i) short_name[length++] = entry[i];
  short_name[length++] = '.';
  for (unsigned i = 8; i < 11 && entry[i] != ' '; ++i) short_name[length++] = entry[i];
  short_name[length] = 0;
  const bool needs_lfn = !RomFileActions::same_name(short_name, filename);
  const uint8_t sum = RomEntryRecovery::checksum(entry);
  unsigned count = 1;
  bool complete_lfn = false;
  for (unsigned order = 1; order <= 20 && order <= target_index; ++order) {
    Entry& previous = work.entries[order];
    if (!snapshot(target_index - order, previous)) return Result::SD_ERROR;
    if (!RomEntryRecovery::long_entry(previous.bytes, order, sum)) {
      if (order == 1 && !needs_lfn) break;
      return Result::SD_ERROR;
    }
    ++count;
    if (previous.bytes[0] & 0x40) { complete_lfn = true; break; }
  }
  if ((needs_lfn || count > 1) && !complete_lfn) return Result::SD_ERROR;
  if (!file.close() || !root.close()) return Result::SD_ERROR;
  const bool removed = RomEntryRecovery::erase(*sd.card(), work, count,
      data_start, (uint32_t)end);
  // All file handles are closed/synced before raw I/O. Rebuild volume caches
  // afterwards even on error, so a stale sector cannot resurrect the entry.
  if (!sd.volumeBegin()) return Result::SD_ERROR;
  Serial.printf("I ROM entry recovery: %s (%s); FAT clusters left allocated\n",
      filename, removed ? "removed" : "failed");
  return removed ? Result::REMOVED_DAMAGED : Result::SD_ERROR;
}

// Finish/destruct the normal action's file handles before recovery needs its
// own stack workspace; the RP2040 core-0 stack is only 2 KiB.
static __attribute__((noinline)) RomFileActions::Result rom_apply_action_once(
    RomFileActions::Action action, const char* filename, const char* new_name) {
  return RomFileActions::apply<FsFile>(action, filename, new_name, O_RDONLY, O_RDWR);
}

static RomFileActions::Result rom_apply_action(RomFileActions::Action action,
    const char* filename, const char* new_name = nullptr) {
  auto scope = UseSDPinFunctionScope();
  const auto result = rom_apply_action_once(action, filename, new_name);
  if (result == RomFileActions::Result::DELETE_FAILED)
    return rom_remove_damaged_entry(filename);
  return result;
}

static bool rom_confirm_delete(const char* filename) {
  ui_draw_background();
  ui_draw_header("GAME SETTINGS // DELETE");
  rom_settings_filename(filename);
  tft.setTextColor(TFT_WHITE);
  tft.drawCentreString("DELETE THIS GAME FROM SD?", 160, 121, 2);
  tft.setTextColor(ui_green());
  tft.drawCentreString("SAVE DATA WILL BE KEPT", 160, 145, 1);
  tft.setTextColor(ui_pink());
  tft.drawCentreString("RESTORE THE ROM BY COPYING IT BACK", 160, 158, 1);
  bool remove = false; // A fresh A press cancels unless Delete was selected.
  auto buttons = [&]() {
    rom_action_button(18, 176, 132, 28, "CANCEL", !remove);
    rom_action_button(170, 176, 132, 28, "DELETE", remove, true);
  };
  buttons();
  ui_draw_footer("LEFT/RIGHT CHOOSE  //  A CONFIRM", "B CANCEL");
  rom_wait_input_released();
  while (true) {
    uint16_t x = 0, y = 0;
    const auto event = rom_menu_input(&x, &y);
    if (event == RomMenuEvent::BACK) return false;
    if (event == RomMenuEvent::LEFT || event == RomMenuEvent::RIGHT) {
      remove = event == RomMenuEvent::RIGHT;
      buttons();
      chriscade_ui_click(480);
    }
    if (event == RomMenuEvent::A) return remove;
    if (event == RomMenuEvent::TOUCH && y >= 176 && y < 204) {
      if (x >= 18 && x < 150) return false;
      if (x >= 170 && x < 302) return true;
    }
  }
}

// Menu-only reuse of the existing flash-sector workspace. Loading a ROM starts
// only after these screens return; avoid new persistent buffers in tight RAM.
static char* rom_renamed_filename() {
  return reinterpret_cast<char*>(rom_sector_buffer) + MAX_PATH_LENGTH;
}

static bool rom_rename_keyboard(const char* filename) {
  const char* extension = RomFileActions::extension(filename);
  if (!extension) return false;
  char* text = reinterpret_cast<char*>(rom_sector_buffer);
  size_t length = (size_t)(extension - filename);
  memcpy(text, filename, length);
  text[length] = '\0';
  const size_t maximum = MAX_PATH_LENGTH - strlen(extension) - 1;
  RomNameEditor editor(text, maximum);
  size_t view_start = 0;
  const char keys[] = "1234567890qwertyuiopasdfghjkl'zxcvbnm._-";
  static_assert(sizeof(keys) == 41, "Keyboard has four rows of ten keys");
  const char* controls[] = {"SHIFT", "SPACE", "DEL", "CLEAR", "SAVE", "BACK"};
  const int control_x[] = {8, 52, 105, 149, 198, 253};
  const int control_w[] = {40, 49, 40, 45, 51, 59};
  bool upper = false;
  int focus = 10;
  auto draw_key = [&](int key) {
    if (key < 40) {
      char label[] = {keys[key], '\0'};
      if (upper && label[0] >= 'a' && label[0] <= 'z') label[0] -= 'a' - 'A';
      rom_action_button(11 + (key % 10) * 30, 98 + (key / 10) * 25,
          28, 22, label, key == focus);
    } else {
      const int i = key - 40;
      rom_action_button(control_x[i], 202, control_w[i], 30, controls[i], key == focus);
    }
  };
  auto draw_text = [&](const char* message) {
    rom_action_button(12, 46, 28, 28, "<", false);
    rom_action_button(280, 46, 28, 28, ">", false);
    tft.fillRoundRect(44, 46, 232, 28, 10, ui_panel());
    tft.drawRoundRect(44, 46, 232, 28, 10, ui_cyan());
    tft.setTextColor(TFT_WHITE, ui_panel());
    if (editor.cursor < view_start) view_start = editor.cursor;
    if (editor.cursor > view_start + 36) view_start = editor.cursor - 36;
    char visible[37];
    const size_t shown = min(editor.length - view_start, (size_t)36);
    memcpy(visible, text + view_start, shown);
    visible[shown] = '\0';
    tft.drawString(visible, 50, 56, 1);
    tft.drawFastVLine(50 + (editor.cursor - view_start) * 6, 54, 11, ui_cyan());
    ui_restore_background_rect(8, 77, 304, 17);
    tft.setTextColor(message ? ui_pink() : ui_green());
    char hint[48];
    snprintf(hint, sizeof(hint), "%s KEPT // TAP NAME OR < > TO MOVE", extension);
    tft.drawCentreString(message ? message : hint, 160, 82, 1);
  };
  ui_draw_background();
  ui_draw_header("GAME SETTINGS // RENAME");
  draw_text(nullptr);
  for (int i = 0; i < 46; ++i) draw_key(i);
  rom_wait_input_released();
  while (true) {
    uint16_t x = 0, y = 0;
    const auto event = rom_menu_input(&x, &y);
    if (event == RomMenuEvent::BACK) return false;
#if ENABLE_INPUT == INPUT_CROWPANEL
    if (event == RomMenuEvent::NONE &&
        (!readJoypad(PIN_BUTTON_X) || !readJoypad(PIN_BUTTON_Y))) {
      if (!readJoypad(PIN_BUTTON_X)) editor.left(); else editor.right();
      draw_text(nullptr);
      sleep_ms(130);
      continue;
    }
#endif
    if (event == RomMenuEvent::TOUCH && y >= 46 && y < 74) {
      if (x >= 12 && x < 40) editor.left();
      else if (x >= 280 && x < 308) editor.right();
      else if (x >= 44 && x < 276)
        editor.place(view_start + (x > 50 ? (x - 50 + 3) / 6 : 0));
      draw_text(nullptr);
      chriscade_ui_click(480);
      continue;
    }
    int activate = -1;
    int next_focus = focus;
    if (event == RomMenuEvent::A) activate = focus;
    if (event == RomMenuEvent::START) activate = 44;
    if (event == RomMenuEvent::UP) next_focus = focus < 10 ? 40 + min(focus, 5) : focus - 10;
    if (event == RomMenuEvent::DOWN) next_focus = focus >= 40 ? focus - 40 :
        focus >= 30 ? 40 + min(focus - 30, 5) : focus + 10;
    if (event == RomMenuEvent::LEFT || event == RomMenuEvent::RIGHT) {
      const int row_start = focus >= 40 ? 40 : (focus / 10) * 10;
      const int count = focus >= 40 ? 6 : 10;
      next_focus = row_start + (focus - row_start + count +
          (event == RomMenuEvent::RIGHT ? 1 : -1)) % count;
    }
    if (event == RomMenuEvent::TOUCH) {
      if (x >= 11 && x < 309 && y >= 98 && y < 195 &&
          (x - 11) % 30 < 28 && (y - 98) % 25 < 22) {
        activate = ((y - 98) / 25) * 10 + (x - 11) / 30;
      } else if (y >= 202 && y < 232) {
        for (int i = 0; i < 6; ++i)
          if (x >= control_x[i] && x < control_x[i] + control_w[i]) activate = 40 + i;
      }
      if (activate >= 0) next_focus = activate;
    }
    if (next_focus != focus) {
      const int old = focus;
      focus = next_focus;
      draw_key(old); draw_key(focus);
      if (activate < 0) chriscade_ui_click(480);
    }
    if (activate < 0) continue;
    chriscade_ui_click(620);
    if (activate == 45) return false;
    if (activate == 44) {
      if (!RomFileActions::renamed_leaf(filename, text, rom_renamed_filename())) {
        draw_text("ENTER A VALID NAME (NO EXTENSION)");
        continue;
      }
      const auto result = rom_apply_action(RomFileActions::Action::RENAME,
          filename, rom_renamed_filename());
      if (result == RomFileActions::Result::OK) return true;
      draw_text(rom_action_message(result));
      continue;
    }
    if (activate == 40) {
      upper = !upper;
      for (int i = 10; i < 40; ++i) draw_key(i);
    } else if (activate == 42) {
      editor.backspace();
    } else if (activate == 43) {
      editor.clear();
    } else if (activate < 40 || activate == 41) {
      char c = activate == 41 ? ' ' : keys[activate];
      if (upper && c >= 'a' && c <= 'z') c -= 'a' - 'A';
      if (!editor.insert(c)) { draw_text("NAME IS FULL // DELETE A CHARACTER"); continue; }
    }
    draw_text(nullptr);
  }
}

enum class RomEdit { NONE, RENAMED, REMOVED };
static RomEdit rom_game_settings(const char* filename) {
  uint8_t selected = 0;
  auto buttons = [&]() {
    rom_action_button(22, 124, 276, 32, "RENAME GAME", selected == 0);
    rom_action_button(22, 166, 276, 32, "DELETE GAME FROM SD", selected == 1, true);
  };
  auto draw = [&]() {
    ui_draw_background();
    ui_draw_header("GAME SETTINGS");
    rom_settings_filename(*filename ? filename : "NO GAME SELECTED");
    if (*filename) buttons();
    ui_draw_footer("UP/DOWN CHOOSE // A OPEN", "B / TAP HERE BACK // SAVES KEPT");
  };
  draw();
  rom_wait_input_released();
  while (true) {
    uint16_t x = 0, y = 0;
    const auto event = rom_menu_input(&x, &y);
    if (event == RomMenuEvent::BACK || (event == RomMenuEvent::TOUCH &&
        x >= 12 && x < 308 && y >= 210 && y < 236)) return RomEdit::NONE;
    if (!*filename) continue;
    if (event == RomMenuEvent::UP || event == RomMenuEvent::DOWN) {
      selected ^= 1;
      buttons();
      chriscade_ui_click(500);
    }
    bool activate = event == RomMenuEvent::A;
    if (event == RomMenuEvent::TOUCH && x >= 22 && x < 298) {
      if (y >= 124 && y < 156) { selected = 0; activate = true; }
      if (y >= 166 && y < 198) { selected = 1; activate = true; }
    }
    if (!activate) continue;
    chriscade_ui_click(680);
    if (selected == 0) {
      if (rom_rename_keyboard(filename)) {
        chriscade_ui_click(880);
        return RomEdit::RENAMED;
      }
    } else if (rom_confirm_delete(filename)) {
      auto result = rom_apply_action(RomFileActions::Action::REMOVE, filename);
      // The one Delete confirmation is enough. A read-only attribute is a
      // Windows/FAT file flag, not a hardware lock, so clear it only on this
      // exact selected ROM and remove it. Save/settings filenames are never
      // derived from this menu action.
      if (result == RomFileActions::Result::READ_ONLY) {
        result = rom_apply_action(RomFileActions::Action::FORCE_REMOVE, filename);
      }
      ui_draw_background();
      ui_draw_header("GAME SETTINGS // DELETE");
      rom_settings_filename(filename);
      const bool removed = result == RomFileActions::Result::OK ||
          result == RomFileActions::Result::REMOVED_DAMAGED;
      tft.setTextColor(removed ? ui_green() : ui_pink());
      tft.drawCentreString(rom_action_message(result), 160, 136, 1);
      tft.setTextColor(TFT_WHITE);
      tft.drawCentreString(result == RomFileActions::Result::REMOVED_DAMAGED ?
          "SPACE MAY NEED SD REPAIR ON PC" : result == RomFileActions::Result::OK ?
          "LAST FLASHED GAME IS STILL ON PICO" : "NO OTHER FILES WERE TARGETED", 160, 155, 1);
      rom_action_button(80, 174, 160, 29, "BACK TO GAMES", true);
      ui_draw_footer("A / B BACK", "");
      chriscade_ui_click(removed ? 760 : 260);
      rom_wait_input_released();
      while (true) {
        uint16_t bx = 0, by = 0;
        const auto back = rom_menu_input(&bx, &by);
        if (back == RomMenuEvent::A || back == RomMenuEvent::BACK ||
            (back == RomMenuEvent::TOUCH && bx >= 80 && bx < 240 && by >= 174 && by < 203)) break;
      }
      if (removed) return RomEdit::REMOVED;
    }
    draw();
    rom_wait_input_released();
  }
}

static void rom_find_renamed_position(uint16_t* page, uint8_t* selected) {
  auto scope = UseSDPinFunctionScope();
  FsFile dir, file;
  uint32_t before = 0;
  if (!dir.open("/")) { *page = 0; *selected = 0; return; }
  while (file.openNext(&dir, O_RDONLY)) {
    char* name = reinterpret_cast<char*>(rom_sector_buffer) + 4 * MAX_PATH_LENGTH;
    file.getName(name, MAX_PATH_LENGTH);
    if (file.isFile() && rom_filename_supported(name) &&
        rom_filename_compare(name, rom_renamed_filename()) < 0) ++before;
    file.close();
  }
  *page = before >= FILES_PER_PAGE ? (uint16_t)(before - FILES_PER_PAGE + 1) : 0;
  *selected = (uint8_t)(before - *page);
}

enum class LauncherChoice {
  LAST_GAME,
  GAME_LIBRARY,
  APP_LIBRARY,
  SETTINGS,
};

static LauncherChoice chriscade_app_launcher() {
  const uint16_t panel = ui_panel();
  const uint16_t cyan = ui_cyan();
  const uint16_t pink = ui_pink();
  const uint16_t green = ui_green();

  auto draw_icon = [&](uint8_t type, int cx, int cy, bool large) {
    const int radius = large ? 23 : 11;
    tft.fillCircle(cx, cy, radius, tft.color565(9, 20, 47));
    tft.drawCircle(cx, cy, radius, type == 1 ? pink : cyan);
    if (type == 0) {
      const int w = large ? 34 : 18;
      const int h = large ? 17 : 10;
      tft.fillRoundRect(cx - w / 2, cy - h / 2, w, h,
          large ? 6 : 3, tft.color565(190, 232, 240));
      tft.fillRect(cx - w / 4, cy - 1, large ? 9 : 5, 3,
          tft.color565(15, 38, 65));
      tft.fillRect(cx - w / 4 + 3, cy - (large ? 5 : 3), 3,
          large ? 10 : 7, tft.color565(15, 38, 65));
      tft.fillCircle(cx + w / 4, cy - 2, large ? 2 : 1, pink);
      tft.fillCircle(cx + w / 3, cy + 3, large ? 2 : 1, cyan);
    } else if (type == 1) {
      const int d = large ? 9 : 5;
      tft.fillCircle(cx - d, cy - d, large ? 4 : 2, cyan);
      tft.fillCircle(cx + d, cy - d, large ? 4 : 2, pink);
      tft.fillCircle(cx - d, cy + d, large ? 4 : 2, green);
      tft.drawCircle(cx + d, cy + d, large ? 5 : 3,
          tft.color565(255, 190, 55));
    } else {
      const int r = large ? 9 : 5;
      tft.drawCircle(cx, cy, r, green);
      tft.fillCircle(cx, cy, large ? 4 : 2, pink);
      tft.drawFastHLine(cx - radius + 4, cy, (radius - 4) * 2, cyan);
      tft.drawFastVLine(cx, cy - radius + 4, (radius - 4) * 2, cyan);
      tft.drawLine(cx - r - 5, cy - r - 5, cx - r, cy - r, cyan);
      tft.drawLine(cx + r, cy + r, cx + r + 5, cy + r + 5, cyan);
    }
  };

  auto draw_card = [&](uint8_t type, int y, bool large) {
    const uint16_t card = chriscade_theme_card(type);
    const uint16_t edge = type == 1 ? pink : type == 2 ? green : cyan;
    const int x = large ? 17 : 35;
    const int width = large ? 286 : 250;
    const int height = large ? 76 : 31;
    const int radius = large ? 20 : 14;
    tft.fillRoundRect(x + (large ? 3 : 0), y + (large ? 3 : 0),
        width, height, radius, large ? tft.color565(7, 20, 48) : card);
    tft.fillRoundRect(x, y, width, height, radius, card);
    tft.drawRoundRect(x, y, width, height, radius,
        large ? edge : tft.color565(43, 51, 86));
    const int icon_x = large ? 51 : 53;
    const int icon_y = y + height / 2;
    draw_icon(type, icon_x, icon_y, large);
    const char* title = type == 0 ? "GAME LIBRARY" :
        type == 1 ? "APP LIBRARY" : "SETTINGS";
    const char* subtitle = type == 0 ? "GB + GBC COLLECTION" :
        type == 1 ? "DRAW  •  CALC  •  TIME  •  METRO" :
                    "DISPLAY  •  BOOT  •  SYSTEM";
    tft.setTextColor(large ? TFT_WHITE : tft.color565(190, 215, 225), card);
    tft.drawString(title, large ? 85 : 73, y + (large ? 13 : 8),
        large ? 2 : 1);
    if (large) {
      tft.setTextColor(tft.color565(185, 220, 230), card);
      tft.drawString(subtitle, 86, y + 39, 1);
      tft.fillRoundRect(85, y + 55, type == 0 ? 61 : type == 1 ? 58 : 72,
          13, 6, tft.color565(8, 38, 51));
      tft.setTextColor(edge, tft.color565(8, 38, 51));
      tft.drawCentreString(type == 0 ? "ONLINE" : type == 1 ? "5 APPS" : "12 OPTIONS",
          type == 0 ? 115 : type == 1 ? 114 : 121, y + 58, 1);
      tft.fillTriangle(284, y + 30, 284, y + 44, 294, y + 37, TFT_WHITE);
    }
  };

  auto draw_launcher = [&](uint8_t selected, bool full_redraw) {
    if (full_redraw) {
      ui_draw_background();
      ui_draw_header("APPS // SYSTEM READY");
    }
    if (selected == 0) {
      draw_card(0, 48, true); draw_card(1, 132, false); draw_card(2, 171, false);
    } else if (selected == 1) {
      draw_card(0, 48, false); draw_card(1, 87, true); draw_card(2, 171, false);
    } else {
      draw_card(0, 48, false); draw_card(1, 87, false); draw_card(2, 126, true);
    }
    ui_draw_footer("UP/DOWN  •  A OPEN", "START  LAST GAME");
  };

  auto draw_launcher_transition = [&](uint8_t previous, uint8_t next) {
    // Draw the incoming large card first, restore only the 39-pixel remainder
    // of the outgoing large card, then place the new small card over clean
    // background. This removes stale square/rounded-corner pixels without a
    // full content-area clear or visible background flash.
    if (previous == 0 && next == 1) {
      // The outgoing game card overlaps the incoming app card's rounded top.
      ui_restore_background_rect(17, 87, 289, 23);
      draw_card(1, 87, true);
      ui_restore_background_rect(17, 48, 289, 39);
      draw_card(0, 48, false);
    } else if (previous == 1 && next == 0) {
      // The outgoing app card overlaps the incoming game card's rounded bottom.
      ui_restore_background_rect(17, 104, 289, 23);
      draw_card(0, 48, true);
      ui_restore_background_rect(17, 127, 289, 39);
      draw_card(1, 132, false);
    } else if (previous == 1 && next == 2) {
      // The outgoing app card overlaps the incoming settings card's rounded top.
      ui_restore_background_rect(17, 126, 289, 23);
      draw_card(2, 126, true);
      ui_restore_background_rect(17, 87, 289, 39);
      draw_card(1, 87, false);
    } else if (previous == 2 && next == 1) {
      // The outgoing settings card overlaps the incoming app card's rounded bottom.
      ui_restore_background_rect(17, 143, 289, 23);
      draw_card(1, 87, true);
      ui_restore_background_rect(17, 166, 289, 39);
      draw_card(2, 171, false);
    }
  };

  uint8_t selected = 0;
  chriscade_touch_init();
  draw_launcher(selected, true);

  wait_launcher_buttons_released();
  bool touch_was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_START)) {
      chriscade_ui_click(700);
      wait_launcher_buttons_released();
      return LauncherChoice::LAST_GAME;
    }
    if (!readJoypad(PIN_DOWN) && selected < 2) {
      chriscade_ui_click(560);
      uint8_t previous = selected;
      selected++;
      draw_launcher_transition(previous, selected);
      sleep_ms(170);
    }
    if (!readJoypad(PIN_UP) && selected > 0) {
      chriscade_ui_click(480);
      uint8_t previous = selected;
      selected--;
      draw_launcher_transition(previous, selected);
      sleep_ms(170);
    }
    if (!readJoypad(PIN_A)) {
      chriscade_ui_click(780);
      wait_launcher_buttons_released();
      return selected == 0 ? LauncherChoice::GAME_LIBRARY :
          selected == 1 ? LauncherChoice::APP_LIBRARY : LauncherChoice::SETTINGS;
    }

    uint16_t tx, ty;
    bool pressed = chriscade_main_menu_touch_enabled() && chriscade_touch_read(&tx, &ty);
    if (pressed && !touch_was_pressed && ty >= 45 && ty <= 205) {
      int touched = selected == 0 ? (ty < 128 ? 0 : ty < 168 ? 1 : 2) :
          selected == 1 ? (ty < 83 ? 0 : ty < 167 ? 1 : 2) :
                          (ty < 83 ? 0 : ty < 122 ? 1 : 2);
      chriscade_ui_click(780);
      while (chriscade_touch_read(&tx, &ty)) sleep_ms(5);
      return touched == 0 ? LauncherChoice::GAME_LIBRARY :
          touched == 1 ? LauncherChoice::APP_LIBRARY : LauncherChoice::SETTINGS;
    }
    touch_was_pressed = pressed;
    tight_loop_contents();
  }
}

// A loads, B returns Home, and SELECT edits the highlighted SD game.
// Only eight names are resident; moving past an edge reveals one new entry.
static bool rom_library_selector() {
  RomLibraryWindow window;
  uint16_t& first_file = window.first;
  char (*filename)[MAX_PATH_LENGTH] = rom_menu_filenames;
  uint16_t& num_files = window.count;
  uint8_t& selected = window.selected;
  bool& add_selected = window.add_selected;
  num_files = rom_file_selector_display_page(filename, first_file);
  add_selected = num_files == 0;
  print_file_entry(filename[selected], selected, num_files, true);
  bool touch_was_pressed = false;
  auto refresh = [&]() {
    num_files = read_file_page_from_card(filename, first_file);
    while (!num_files && first_file) {
      --first_file;
      num_files = read_file_page_from_card(filename, first_file);
    }
    selected = num_files ? min((uint16_t)selected, (uint16_t)(num_files - 1)) : 0;
    add_selected = num_files == 0;
    ui_draw_background();
    ui_draw_header("GAME LIBRARY // A-Z");
    draw_library_range(first_file, num_files);
    draw_library_actions(add_selected);
    for (uint8_t i = 0; i < num_files; ++i)
      print_file_entry(filename[i], i, num_files, i == selected);
    if (!num_files) print_file_entry(filename[0], 0, 0);
    rom_wait_input_released();
    touch_was_pressed = false;
  };
  auto open_game_settings = [&]() {
    chriscade_ui_click(640);
    const RomEdit result = rom_game_settings(num_files && !add_selected ? filename[selected] : "");
    if (result == RomEdit::RENAMED) rom_find_renamed_position(&first_file, &selected);
    refresh();
  };
  auto add_game = [&]() {
    chriscade_ui_click(780);
    rom_wait_input_released();
    rom_usb_upload(rom_sector_buffer, sizeof(rom_sector_buffer));
    refresh();
  };
  auto redraw_window = [&]() {
    for (uint8_t i = 0; i < num_files; ++i)
      print_file_entry(filename[i], i, num_files, i == selected);
    draw_library_range(first_file, num_files);
  };
  auto scroll = [&](bool backwards) {
    const uint8_t old_selected = selected;
    const bool old_add = add_selected;
    const auto change = window.move(backwards, rom_menu_filenames,
        reinterpret_cast<char*>(rom_sector_buffer), [](const char* anchor, char* next, bool back) {
          auto scope = UseSDPinFunctionScope();
          return find_next_rom_filename(anchor, next, back);
        });
    if (change == RomScrollChange::NONE) return;
    if (change == RomScrollChange::WINDOW) redraw_window();
    else {
      if (!old_add) print_file_entry(filename[old_selected], old_selected, num_files);
      if (!add_selected) print_file_entry(filename[selected], selected, num_files, true);
      if (change == RomScrollChange::FOOTER) draw_library_actions(add_selected);
    }
    chriscade_ui_click(backwards ? 480 : 540);
  };
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B)) {
      chriscade_ui_click(360);
      wait_launcher_buttons_released();
      return false;
    }
    if (!readJoypad(PIN_SELECT)) {
      open_game_settings();
      continue;
    }
    if (!readJoypad(PIN_START)) {
      /* re-start the last game (no need to reprogram flash) */
      chriscade_ui_click(700);
      wait_launcher_buttons_released();
      return true;
    }
    if (!readJoypad(PIN_A) && add_selected) {
      add_game();
      continue;
    }
    if (num_files > 0 && !readJoypad(PIN_A)) {
      /* copy the rom from the SD card to flash and start the game */
      chriscade_ui_click(780);
      load_cart_rom_file(filename[selected]);
      wait_launcher_buttons_released();
      return true;
    }
    if (!readJoypad(PIN_DOWN)) {
      scroll(false);
      sleep_ms(150);
    }
    if (!readJoypad(PIN_UP)) {
      scroll(true);
      sleep_ms(150);
    }

    uint16_t tx, ty;
    bool pressed = chriscade_touch_read(&tx, &ty);
    if (pressed && !touch_was_pressed && tx >= 12 && tx < 124 &&
        ty >= 210 && ty < 236) {
      add_game();
      continue;
    }
    if (pressed && !touch_was_pressed && tx >= 132 && tx < 308 &&
        ty >= 210 && ty < 236) {
      open_game_settings();
      continue;
    }
    if (pressed && !touch_was_pressed && num_files > 0 &&
        tx >= 8 && tx < DISPLAY_WIDTH - 8 && ty >= ROM_LIST_TOP) {
      int row = ((int)ty - ROM_LIST_TOP) / ROM_ENTRY_HEIGHT;
      if (row >= 0 && row < num_files) {
        selected = (uint8_t)row;
        chriscade_ui_click(780);
        print_file_entry(filename[selected], selected, num_files, true);
        while (chriscade_touch_read(&tx, &ty)) sleep_ms(5);
        load_cart_rom_file(filename[selected]);
        wait_launcher_buttons_released();
        return true;
      }
    }
    touch_was_pressed = pressed;
    tight_loop_contents();
  }
}

void rom_file_selector() {
  while (true) {
    LauncherChoice choice = chriscade_app_launcher();
    if (choice == LauncherChoice::LAST_GAME) return;
    if (choice == LauncherChoice::GAME_LIBRARY) {
      if (rom_library_selector()) return;
    } else if (choice == LauncherChoice::APP_LIBRARY) {
      chriscade_app_library();
    } else {
      chriscade_settings_menu();
    }
  }
}

#endif
