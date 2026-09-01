#include <Arduino.h>
#include <stdlib.h>

#include "common.h"
#include "input.h"
#include "card_loader.h"
#include "chriscade_apps.h"
#include "chriscade_boot.h"
#include "chriscade_settings.h"
#include "metronome_pattern.h"
#include "rom_ui_helpers.h"
#include "drawing_picture.h"
#include "game_picture.h"

static constexpr uint32_t TOUCH_CAL_MAGIC = 0x43485443u; // "CHTC"
static constexpr char TOUCH_CAL_FILE[] = "TOUCH.CAL";
static bool touch_ready;

struct TouchCalibrationFile {
  uint32_t magic;
  uint16_t data[5];
};

static uint16_t app_bg() { return chriscade_theme_bg(); }
static uint16_t app_panel() { return chriscade_theme_panel(); }
static uint16_t app_cyan() { return chriscade_theme_primary(); }
static uint16_t app_pink() { return chriscade_theme_secondary(); }
static uint16_t app_green() { return chriscade_theme_accent(); }

static void app_button_sound(unsigned int frequency) {
  chriscade_ui_click(frequency);
}

static uint16_t app_canvas_at(int y) {
  return chriscade_theme_canvas_at(y);
}

static void app_background() {
  tft.fillScreen(app_bg());
  for (int y = 0; y < DISPLAY_HEIGHT; y += 8)
    tft.fillRect(0, y, DISPLAY_WIDTH, 8, app_canvas_at(y));
  tft.drawCircle(298, 70, 48, app_panel());
  tft.drawCircle(298, 70, 37, app_pink());
  tft.drawLine(0, 192, 94, 154, app_cyan());
  for (int i = 0; i < 10; ++i) {
    int x = (i * 73 + 17) % DISPLAY_WIDTH;
    int y = 43 + (i * 41) % 154;
    tft.fillCircle(x, y, (i % 4 == 0) ? 2 : 1,
        (i & 1) ? app_pink() : app_cyan());
  }
}

void chriscade_header_button(int x, int width, const char* label,
    uint16_t background, uint16_t border) {
  tft.fillRoundRect(x, 8, width, 23, 11, background);
  tft.drawRoundRect(x, 8, width, 23, 11, border);
  const uint8_t old_datum = tft.getTextDatum();
  const uint16_t old_padding = tft.getTextPadding();
  tft.setTextPadding(0);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, background);
  // Font 1's visible glyph is 5x7 in a 6x8 cell. The extra pixel places
  // the visible lettering, not its trailing spacing, at the capsule center.
  tft.drawString(label, x + (width + 1) / 2, 20, 1);
  tft.setTextDatum(old_datum);
  tft.setTextPadding(old_padding);
}

static void app_header(const char* title, bool home_button = true) {
  tft.setTextColor(app_pink());
  tft.drawString("CHRISCADE", 14, 8, 2);
  tft.setTextColor(app_cyan());
  tft.drawString("CHRISCADE", 10, 6, 2);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("CHRISCADE", 12, 7, 2);
  tft.setTextColor(app_pink());
  tft.drawString(title, 14, 28, 1);
  tft.fillCircle(7, 33, 2, app_cyan());

  if (home_button) {
    const uint16_t panel = tft.color565(10, 18, 39);
    chriscade_header_button(258, 51, "HOME", panel, app_cyan());
  }
}

static bool app_home_hit(uint16_t x, uint16_t y) {
  return x >= 252 && y <= 36;
}

static void wait_app_buttons_released() {
  while (!readJoypad(PIN_A) || !readJoypad(PIN_B) ||
         !readJoypad(PIN_SELECT) || !readJoypad(PIN_START) ||
         !readJoypad(PIN_BUTTON_X) || !readJoypad(PIN_BUTTON_Y)) {
    chriscade_power_poll(false);
    delay(5);
  }
}

static bool load_touch_calibration(uint16_t data[5]) {
  TouchCalibrationFile calibration = {};
  FsFile file;
  bool valid = false;
  {
    auto scope = UseSDPinFunctionScope();
    if (file.open(TOUCH_CAL_FILE, O_RDONLY)) {
      valid = file.read(&calibration, sizeof(calibration)) == sizeof(calibration) &&
          calibration.magic == TOUCH_CAL_MAGIC;
      file.close();
    }
  }
  if (valid) memcpy(data, calibration.data, sizeof(calibration.data));
  return valid;
}

static void save_touch_calibration(const uint16_t data[5]) {
  TouchCalibrationFile calibration = {};
  calibration.magic = TOUCH_CAL_MAGIC;
  memcpy(calibration.data, data, sizeof(calibration.data));
  FsFile file;
  auto scope = UseSDPinFunctionScope();
  if (file.open(TOUCH_CAL_FILE, O_WRONLY | O_CREAT | O_TRUNC)) {
    file.write(&calibration, sizeof(calibration));
    file.close();
  }
}

static void app_touch_init() {
  if (touch_ready) return;

  uint16_t calibration[5];
  if (!load_touch_calibration(calibration)) {
    gpio_put(SD_CS_PIN, 1);
    tft.fillScreen(app_bg());
    tft.setTextColor(TFT_WHITE, app_bg());
    tft.drawCentreString("TOUCH SETUP", DISPLAY_WIDTH / 2, 74, 2);
    tft.setTextColor(tft.color565(150, 185, 210), app_bg());
    tft.drawCentreString("TAP EACH HIGHLIGHTED CORNER", DISPLAY_WIDTH / 2, 104, 1);
    tft.calibrateTouch(calibration, app_cyan(), app_bg(), 15);
    save_touch_calibration(calibration);
  }
  tft.setTouch(calibration);
  touch_ready = true;
}

static bool app_touch(uint16_t* x, uint16_t* y) {
  gpio_put(SD_CS_PIN, 1);
  return tft.getTouch(x, y, 300);
}

void chriscade_touch_init() {
  app_touch_init();
}

bool chriscade_touch_read(uint16_t* x, uint16_t* y) {
  return app_touch(x, y);
}

void chriscade_touch_recalibrate() {
  touch_ready = false;
  {
    auto scope = UseSDPinFunctionScope();
    sd.remove(TOUCH_CAL_FILE);
  }
  app_touch_init();
}

static void draw_app_card(int y, const char* title, const char* subtitle,
    int icon, bool selected) {
  const uint16_t bg = selected ? chriscade_theme_card((uint8_t)icon) : app_panel();
  const int cy = y + 18;
  tft.fillRoundRect(17, y, 286, 37, 15, bg);
  tft.drawRoundRect(17, y, 286, 37, 15,
      selected ? app_cyan() : tft.color565(41, 48, 83));
  tft.fillCircle(43, cy, 12, tft.color565(9, 24, 53));
  tft.drawCircle(43, cy, 12, icon == 1 ? app_pink() : icon == 3 ? app_green() : app_cyan());

  if (icon == 0) {
    const uint16_t colors[] = {app_cyan(), app_pink(), app_green(),
        tft.color565(255, 190, 55)};
    for (int i = 0; i < 4; ++i)
      tft.fillCircle(38 + (i & 1) * 10, cy - 5 + (i >> 1) * 10, 3, colors[i]);
  } else if (icon == 1) {
    for (int row = 0; row < 2; ++row)
      for (int col = 0; col < 2; ++col)
        tft.fillRoundRect(35 + col * 9, cy - 8 + row * 9, 7, 7, 2,
            (row + col) & 1 ? app_pink() : app_cyan());
  } else if (icon == 2) {
    tft.drawCircle(43, cy, 9, app_green());
    tft.drawLine(43, cy, 43, cy - 6, TFT_WHITE);
    tft.drawLine(43, cy, 49, cy + 3, TFT_WHITE);
  } else if (icon == 3) {
    // Metronome pendulum.
    tft.drawLine(36, cy + 7, 50, cy + 7, app_green());
    tft.drawLine(38, cy + 7, 43, cy - 7, TFT_WHITE);
    tft.drawLine(48, cy + 7, 43, cy - 7, TFT_WHITE);
    tft.drawLine(43, cy + 4, 48, cy - 5, app_pink());
    tft.fillCircle(48, cy - 5, 2, app_green());
  } else {
    // Screenshot gallery camera.
    tft.drawRoundRect(34, cy - 7, 18, 14, 3, app_cyan());
    tft.fillCircle(43, cy, 4, app_pink());
    tft.drawFastHLine(38, cy - 10, 9, app_green());
  }

  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(title, 69, y + 4, 2);
  tft.setTextColor(tft.color565(155, 190, 210), bg);
  tft.drawString(subtitle, 70, y + 24, 1);
  if (selected)
    tft.fillTriangle(282, y + 11, 282, y + 25, 292, y + 18, TFT_WHITE);
}

static constexpr uint32_t SCREENSHOT_MAGIC = DrawingPicture::MAGIC;
static constexpr uint8_t SCREENSHOT_SLOTS = 30;
static constexpr uint8_t SCREENSHOT_LEGACY_SLOTS = 4;
enum class ScreenshotFolder : uint8_t { LEGACY = 0, POKEMON = 1, CRYSTAL = 2, MARIO = 3, ZELDA = 4,
  GAMES = 5, DRAWINGS = 6, EDITS = 7 };
using ScreenshotHeader = DrawingPicture::Header;
static_assert(sizeof(ScreenshotHeader) == 28, "Screenshot header must be stable");

static uint8_t screenshot_ref(ScreenshotFolder folder, uint8_t slot) {
  return ((uint8_t)folder << 5) | slot;
}
static ScreenshotFolder screenshot_folder(uint8_t ref) { return (ScreenshotFolder)(ref >> 5); }
static uint8_t screenshot_slot(uint8_t ref) { return ref & 31u; }
static void screenshot_name(uint8_t ref, char name[32]) {
  const unsigned n = screenshot_slot(ref) + 1;
  switch (screenshot_folder(ref)) {
    case ScreenshotFolder::POKEMON: snprintf(name, 32, "GAMES/POKEMON/P%03u.565", n); break;
    case ScreenshotFolder::CRYSTAL: snprintf(name, 32, "GAMES/CRYSTAL/C%03u.565", n); break;
    case ScreenshotFolder::MARIO: snprintf(name, 32, "GAMES/MARIO/M%03u.565", n); break;
    case ScreenshotFolder::ZELDA: snprintf(name, 32, "GAMES/ZELDA/Z%03u.565", n); break;
    case ScreenshotFolder::GAMES: snprintf(name, 32, "GAMES/OTHER/G%03u.565", n); break;
    case ScreenshotFolder::DRAWINGS: snprintf(name, 32, "DRAWINGS/D%03u.565", n); break;
    case ScreenshotFolder::EDITS: snprintf(name, 32, "EDITS/E%03u.565", n); break;
    default: snprintf(name, 32, "SHOT%03u.565", n); break;
  }
}
static void screenshot_display_name(uint8_t ref, char name[18]) {
  static const char* prefixes[] = { "LEGACY", "POKEMON", "CRYSTAL", "MARIO", "ZELDA", "GAME", "DRAW", "EDIT" };
  snprintf(name, 18, "%s %03u", prefixes[(uint8_t)screenshot_folder(ref)], screenshot_slot(ref) + 1);
}
static void screenshot_folder_make(ScreenshotFolder folder) {
  if (folder == ScreenshotFolder::POKEMON) sd.mkdir("GAMES/POKEMON", true);
  else if (folder == ScreenshotFolder::CRYSTAL) sd.mkdir("GAMES/CRYSTAL", true);
  else if (folder == ScreenshotFolder::MARIO) sd.mkdir("GAMES/MARIO", true);
  else if (folder == ScreenshotFolder::ZELDA) sd.mkdir("GAMES/ZELDA", true);
  else if (folder == ScreenshotFolder::GAMES) sd.mkdir("GAMES/OTHER", true);
  else if (folder == ScreenshotFolder::DRAWINGS) sd.mkdir("DRAWINGS", true);
  else if (folder == ScreenshotFolder::EDITS) sd.mkdir("EDITS", true);
}
static bool screenshot_header(uint8_t slot, ScreenshotHeader* header);
static bool screenshot_exists(uint8_t ref) {
  char name[32]; screenshot_name(ref, name);
  FsFile file;
  auto scope = UseSDPinFunctionScope();
  if (!file.open(name, O_RDONLY)) return false;
  ScreenshotHeader header = {};
  const bool valid = file.read(&header, sizeof(header)) == sizeof(header) &&
      DrawingPicture::valid(header, file.fileSize());
  file.close();
  return valid;
}

// The first four captures used the original root SHOT###.565 layout.  Keep
// them visible without exposing a separate "Legacy" page: their saved source
// label places PM_CRYSTAL in Crystal, DRAW in Drawings, and any remaining old
// game capture in Other Games.
static bool old_screenshot_belongs_to(ScreenshotFolder folder, uint8_t slot) {
  const uint8_t old_ref = screenshot_ref(ScreenshotFolder::LEGACY, slot);
  ScreenshotHeader header = {};
  if (!screenshot_header(old_ref, &header)) return false;
  char source[sizeof(header.source)] = {};
  strncpy(source, header.source, sizeof(source) - 1);
  for (char* p = source; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
  if (strstr(source, "PM_CRYSTAL")) return folder == ScreenshotFolder::CRYSTAL;
  if (strstr(source, "DRAW")) return folder == ScreenshotFolder::DRAWINGS;
  return folder == ScreenshotFolder::GAMES;
}

bool chriscade_capture_game_screen(const char* source) {
  GamePicture picture = {};
  uint16_t* row = nullptr;
  if (!lcd_begin_screenshot(picture, row)) {
    Serial.println("Screenshot failed: display did not yield a complete frame");
    return false;
  }
  // Destruction order matters: restore the SD pins before releasing core 1.
  struct ResumeLCD { ~ResumeLCD() { lcd_end_screenshot(); } } resume;
  auto scope = UseSDPinFunctionScope();
  ScreenshotFolder folder = ScreenshotFolder::GAMES;
  char normalized[17] = {}; strncpy(normalized, source, sizeof(normalized) - 1);
  for (char* p = normalized; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
  if (strstr(normalized, "CRYSTAL")) folder = ScreenshotFolder::CRYSTAL;
  else if (strstr(normalized, "POKEMON")) folder = ScreenshotFolder::POKEMON;
  else if (strstr(normalized, "MARIO")) folder = ScreenshotFolder::MARIO;
  else if (strstr(normalized, "ZELDA")) folder = ScreenshotFolder::ZELDA;
  screenshot_folder_make(folder);
  for (uint8_t slot = 0; slot < SCREENSHOT_SLOTS; ++slot) {
    const uint8_t ref = screenshot_ref(folder, slot);
    char name[32], temporary[32]; screenshot_name(ref, name);
    const char prefix = folder == ScreenshotFolder::POKEMON ? 'P' : folder == ScreenshotFolder::CRYSTAL ? 'C' : folder == ScreenshotFolder::MARIO ? 'M' : folder == ScreenshotFolder::ZELDA ? 'Z' : 'G';
    const char* directory = folder == ScreenshotFolder::POKEMON ? "GAMES/POKEMON" : folder == ScreenshotFolder::CRYSTAL ? "GAMES/CRYSTAL" : folder == ScreenshotFolder::MARIO ? "GAMES/MARIO" : folder == ScreenshotFolder::ZELDA ? "GAMES/ZELDA" : "GAMES/OTHER";
    snprintf(temporary, sizeof(temporary), "%s/%c%03u.TMP", directory, prefix, slot + 1);
    // Existing pictures (including damaged ones) are never overwritten.
    if (sd.exists(name) || sd.exists(temporary)) continue;
    ScreenshotHeader header = {SCREENSHOT_MAGIC, DISPLAY_WIDTH, DISPLAY_HEIGHT,
        DISPLAY_WIDTH * DISPLAY_HEIGHT * 2u, {}};
    strncpy(header.source, source, sizeof(header.source) - 1);
    FsFile file;
    const bool saved = DrawingPicture::publish_rows(sd, file, temporary, name,
        header, [&](int y, uint16_t* pixels) { picture.row(y, pixels); }, row,
        O_WRONLY | O_CREAT | O_EXCL, O_RDONLY);
    Serial.printf("Screenshot %s: %s\n", saved ? "saved" : "SD error", name);
    return saved;
  }
  Serial.println("Screenshot gallery full: delete a picture in Apps > Screenshots");
  return false;
}

enum class DrawingSave { SAVED, FULL, SD_ERROR };

static DrawingSave save_drawing(const DrawingCanvas& canvas, const uint16_t colors[16],
    uint16_t* line, char saved_name[32]) {
  auto scope = UseSDPinFunctionScope();
  screenshot_folder_make(ScreenshotFolder::DRAWINGS);
  for (uint8_t slot = 0; slot < SCREENSHOT_SLOTS; ++slot) {
    const uint8_t ref = screenshot_ref(ScreenshotFolder::DRAWINGS, slot);
    char name[32], temporary[32]; screenshot_name(ref, name);
    snprintf(temporary, sizeof(temporary), "DRAWINGS/D%03u.TMP", slot + 1);
    // Preserve every existing file, including damaged/legacy captures.
    if (sd.exists(name) || sd.exists(temporary)) continue;
    FsFile file;
    if (!DrawingPicture::publish(sd, file, temporary, name, canvas, colors, line,
        O_WRONLY | O_CREAT | O_EXCL, O_RDONLY)) return DrawingSave::SD_ERROR;
    strncpy(saved_name, name, 13);
    return DrawingSave::SAVED;
  }
  return DrawingSave::FULL;
}

static DrawingSave save_edited_screenshot(const char* source_name, const DrawingCanvas& canvas,
    const uint16_t colors[16], uint16_t* row, char saved_name[32]) {
  auto scope = UseSDPinFunctionScope();
  FsFile source;
  ScreenshotHeader original = {};
  if (!source.open(source_name, O_RDONLY) ||
      source.read(&original, sizeof(original)) != sizeof(original) ||
      !DrawingPicture::valid(original, source.fileSize())) { source.close(); return DrawingSave::SD_ERROR; }
  screenshot_folder_make(ScreenshotFolder::EDITS);
  for (uint8_t slot = 0; slot < SCREENSHOT_SLOTS; ++slot) {
    const uint8_t ref = screenshot_ref(ScreenshotFolder::EDITS, slot);
    char name[32], temporary[32]; screenshot_name(ref, name);
    snprintf(temporary, sizeof(temporary), "EDITS/E%03u.TMP", slot + 1);
    if (sd.exists(name) || sd.exists(temporary)) continue;
    ScreenshotHeader header = original;
    memset(header.source, 0, sizeof(header.source));
    strncpy(header.source, "EDITED COPY", sizeof(header.source) - 1);
    FsFile destination;
    const bool saved = DrawingPicture::publish_rows(sd, destination, temporary, name, header,
        [&](int y, uint16_t* pixels) {
          source.read(pixels, DISPLAY_WIDTH * sizeof(uint16_t));
          for (int x = 0; x < DISPLAY_WIDTH; ++x) {
            const uint8_t ink = canvas.at(x, y);
            if (ink != DrawingCanvas::PAPER) pixels[x] = colors[ink];
          }
        }, row, O_WRONLY | O_CREAT | O_EXCL, O_RDONLY);
    source.close();
    if (!saved) return DrawingSave::SD_ERROR;
    strncpy(saved_name, name, 13); return DrawingSave::SAVED;
  }
  source.close(); return DrawingSave::FULL;
}

static bool screenshot_header(uint8_t slot, ScreenshotHeader* header) {
  char name[32]; screenshot_name(slot, name);
  FsFile file;
  auto scope = UseSDPinFunctionScope();
  if (!file.open(name, O_RDONLY)) return false;
  const bool valid = file.read(header, sizeof(*header)) == sizeof(*header) &&
      DrawingPicture::valid(*header, file.fileSize());
  file.close();
  header->source[sizeof(header->source) - 1] = 0;
  return valid;
}

static bool screenshot_relabel(uint8_t slot, const char* label) {
  char name[32]; screenshot_name(slot, name);
  ScreenshotHeader header = {};
  if (!screenshot_header(slot, &header)) return false;
  memset(header.source, 0, sizeof(header.source));
  strncpy(header.source, label, sizeof(header.source) - 1);
  auto scope = UseSDPinFunctionScope();
  FsFile file;
  if (!file.open(name, O_RDWR)) return false;
  const bool saved = file.seekSet(0) &&
      file.write(&header, sizeof(header)) == sizeof(header) && file.sync();
  file.close();
  return saved;
}

// A label is saved inside the established image header: no ROM, drawing, or
// pixel data is moved or overwritten when a screenshot is renamed.
static bool screenshot_rename_keyboard(uint8_t slot) {
  static constexpr char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_";
  char label[16] = {};
  ScreenshotHeader header = {};
  if (!screenshot_header(slot, &header)) return false;
  strncpy(label, header.source, sizeof(label) - 1);
  uint8_t key = 0, cursor = (uint8_t)strlen(label);
  bool changed = false;
  auto redraw = [&]() {
    app_background(); app_header("SCREENSHOT // RENAME");
    const uint16_t panel = app_panel();
    tft.fillRoundRect(14, 43, 292, 31, 12, panel);
    tft.drawRoundRect(14, 43, 292, 31, 12, app_cyan());
    tft.setTextColor(TFT_WHITE, panel); tft.drawString(label[0] ? label : "NEW SCREENSHOT", 23, 52, 2);
    const int cx = 23 + cursor * 12; tft.drawFastHLine(cx, 69, 9, app_green());
    for (uint8_t i = 0; i < sizeof(keys) - 1; ++i) {
      const int x = 12 + (i % 8) * 37, y = 85 + (i / 8) * 29;
      const uint16_t card = i == key ? chriscade_theme_card(2) : panel;
      tft.fillRoundRect(x, y, 31, 23, 7, card);
      tft.drawRoundRect(x, y, 31, 23, 7, i == key ? app_green() : tft.color565(50, 57, 92));
      tft.setTextColor(TFT_WHITE, card); char c[2] = {keys[i], 0};
      tft.drawCentreString(c, x + 15, y + 7, 1);
    }
    tft.setTextColor(tft.color565(165, 200, 220), app_bg());
    tft.drawCentreString("A ADD  X DELETE  Y CURSOR  SELECT SAVE  B CANCEL", 160, 219, 1);
  };
  redraw(); wait_app_buttons_released();
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B)) { wait_app_buttons_released(); return false; }
    if (!readJoypad(PIN_SELECT)) {
      wait_app_buttons_released();
      return label[0] && changed && screenshot_relabel(slot, label);
    }
    if (!readJoypad(PIN_A)) {
      if (strlen(label) < sizeof(label) - 1) {
        memmove(label + cursor + 1, label + cursor, strlen(label) - cursor + 1);
        label[cursor++] = keys[key]; changed = true; app_button_sound(620);
      } else app_button_sound(250);
      redraw(); wait_app_buttons_released(); continue;
    }
    if (!readJoypad(PIN_BUTTON_X)) {
      if (cursor) { memmove(label + cursor - 1, label + cursor, strlen(label) - cursor + 1); --cursor; changed = true; app_button_sound(330); }
      else app_button_sound(250);
      redraw(); wait_app_buttons_released(); continue;
    }
    if (!readJoypad(PIN_BUTTON_Y)) {
      if (cursor < strlen(label)) ++cursor; else cursor = 0;
      app_button_sound(480); redraw(); wait_app_buttons_released(); continue;
    }
    if (!readJoypad(PIN_LEFT) || !readJoypad(PIN_RIGHT) || !readJoypad(PIN_UP) || !readJoypad(PIN_DOWN)) {
      if (!readJoypad(PIN_LEFT)) key = key ? key - 1 : sizeof(keys) - 2;
      else if (!readJoypad(PIN_RIGHT)) key = (key + 1) % (sizeof(keys) - 1);
      else if (!readJoypad(PIN_UP)) key = key >= 8 ? key - 8 : key + ((sizeof(keys) - 2) / 8) * 8;
      else key = (key + 8) % (sizeof(keys) - 1);
      app_button_sound(430); redraw(); delay(120); continue;
    }
    delay(5);
  }
}

enum class ScreenshotViewAction { BACK, PREVIOUS, NEXT, RENAME, DRAW, DELETE };

static ScreenshotViewAction view_screenshot(uint8_t slot) {
  char name[32]; screenshot_name(slot, name);
  FsFile file;
  {
    auto scope = UseSDPinFunctionScope();
    if (!file.open(name, O_RDONLY)) return ScreenshotViewAction::BACK;
    ScreenshotHeader header = {};
    if (file.read(&header, sizeof(header)) != sizeof(header) ||
        !DrawingPicture::valid(header, file.fileSize())) { file.close(); return ScreenshotViewAction::BACK; }
  }
  static uint16_t line[DISPLAY_WIDTH];
  const bool old_swap = tft.getSwapBytes();
  tft.setSwapBytes(true); // Saved RGB565 words are native little-endian.
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
    size_t read = 0;
    { auto scope = UseSDPinFunctionScope(); read = file.read(line, sizeof(line)); }
    if (read != sizeof(line)) {
      { auto scope = UseSDPinFunctionScope(); file.close(); }
      tft.setSwapBytes(old_swap);
      return ScreenshotViewAction::BACK;
    }
    tft.pushImage(0, y, DISPLAY_WIDTH, 1, line);
  }
  tft.setSwapBytes(old_swap);
  // The edit bar is deliberately shown only after A: image browsing starts
  // clean, but editing actions are never hidden once requested.
  bool edit_menu = false;
  wait_app_buttons_released();
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_A)) {
      edit_menu = !edit_menu; app_button_sound(700);
      if (edit_menu) {
        const uint16_t panel = app_panel();
        tft.fillRoundRect(7, 208, 306, 27, 12, panel);
        tft.drawRoundRect(7, 208, 306, 27, 12, app_cyan());
        tft.setTextColor(TFT_WHITE, panel);
        tft.drawCentreString("X RENAME   Y DRAW COPY   SELECT DELETE", 160, 217, 1);
      } else {
        // Redraw the covered bottom rows from the file, preserving the photo.
        for (uint16_t y = 208; y < DISPLAY_HEIGHT; ++y) {
          { auto scope = UseSDPinFunctionScope(); file.seekSet(sizeof(ScreenshotHeader) + y * sizeof(line)); file.read(line, sizeof(line)); }
          tft.pushImage(0, y, DISPLAY_WIDTH, 1, line);
        }
      }
      wait_app_buttons_released(); continue;
    }
    if (edit_menu && !readJoypad(PIN_BUTTON_X)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::RENAME; }
    if (edit_menu && !readJoypad(PIN_BUTTON_Y)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::DRAW; }
    if (edit_menu && !readJoypad(PIN_SELECT)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::DELETE; }
    if (!readJoypad(PIN_UP)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::PREVIOUS; }
    if (!readJoypad(PIN_DOWN)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::NEXT; }
    if (!readJoypad(PIN_B)) { file.close(); wait_app_buttons_released(); return ScreenshotViewAction::BACK; }
    delay(5);
  }
}

static bool screenshot_confirm_delete(uint8_t slot) {
  char name[32];
  screenshot_name(slot, name);
  app_background();
  app_header("SCREENSHOTS // DELETE");
  const uint16_t panel = app_panel();
  tft.fillRoundRect(27, 72, 266, 99, 19, panel);
  tft.drawRoundRect(27, 72, 266, 99, 19, app_pink());
  tft.setTextColor(TFT_WHITE, panel);
  tft.drawCentreString("DELETE THIS SCREENSHOT?", 160, 88, 2);
  tft.setTextColor(tft.color565(165, 195, 215), panel);
  tft.drawCentreString(name, 160, 113, 1);
  tft.drawCentreString("A DELETE  //  B CANCEL", 160, 151, 1);
  wait_app_buttons_released();
  bool touched = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_A)) { wait_app_buttons_released(); return true; }
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_app_buttons_released(); return false;
    }
    uint16_t x, y;
    const bool pressed = app_touch(&x, &y);
    if (pressed && !touched && y >= 132 && y <= 174) {
      while (app_touch(&x, &y)) delay(5);
      return x < 160;
    }
    touched = pressed;
    delay(5);
  }
}

static void drawing_app(const char* base_name = nullptr);

static void screenshots_app() {
  uint8_t slots[SCREENSHOT_SLOTS];
  uint8_t count = 0, selected = 0, first = 0;
  ScreenshotFolder active_folder = ScreenshotFolder::POKEMON;
  auto read_slots = [&]() {
    count = 0;
    for (uint8_t i = 0; i < SCREENSHOT_SLOTS; ++i) {
      const uint8_t ref = screenshot_ref(active_folder, i);
      if (screenshot_exists(ref)) slots[count++] = ref;
    }
    // Fold the old root captures into their meaningful folders instead of
    // making the user navigate a separate Legacy category.
    for (uint8_t i = 0; i < SCREENSHOT_LEGACY_SLOTS && count < SCREENSHOT_SLOTS; ++i) {
      if (old_screenshot_belongs_to(active_folder, i))
        slots[count++] = screenshot_ref(ScreenshotFolder::LEGACY, i);
    }
    if (selected >= count) selected = count ? count - 1 : 0;
    if (selected < first) first = selected;
    if (selected >= first + 4) first = selected - 3;
  };
  auto draw_row = [&](uint8_t row, bool active) {
    const int y = 52 + (row - first) * 37;
    const uint16_t panel = app_panel();
    const uint16_t card = active ? chriscade_theme_card(2) : panel;
    char name[18]; screenshot_display_name(slots[row], name);
    ScreenshotHeader header = {};
    screenshot_header(slots[row], &header);
    tft.fillRoundRect(20, y, 280, 29, 12, card);
    tft.drawRoundRect(20, y, 280, 29, 12, active ? app_cyan() : tft.color565(45, 56, 92));
    tft.setTextColor(TFT_WHITE, card); tft.drawString(name, 37, y + 10, 1);
    tft.setTextColor(active ? app_green() : tft.color565(145, 178, 205), card);
    tft.drawRightString(header.source[0] ? header.source : "APP", 280, y + 10, 1);
  };
  auto draw = [&]() {
    app_background();
    char title[40]; snprintf(title, sizeof(title), "SCREENSHOTS // %s", active_folder == ScreenshotFolder::POKEMON ? "POKEMON YELLOW" : active_folder == ScreenshotFolder::CRYSTAL ? "POKEMON CRYSTAL" : active_folder == ScreenshotFolder::MARIO ? "MARIO" : active_folder == ScreenshotFolder::ZELDA ? "ZELDA" : active_folder == ScreenshotFolder::DRAWINGS ? "DRAWINGS" : active_folder == ScreenshotFolder::EDITS ? "EDITS" : active_folder == ScreenshotFolder::GAMES ? "OTHER GAMES" : "LEGACY");
    app_header(title);
    const uint16_t panel = app_panel();
    if (!count) {
      tft.fillRoundRect(23, 80, 274, 76, 18, panel);
      tft.drawRoundRect(23, 80, 274, 76, 18, app_cyan());
      tft.setTextColor(TFT_WHITE, panel);
      tft.drawCentreString("NO SCREENSHOTS YET", 160, 96, 2);
      tft.setTextColor(tft.color565(150, 185, 210), panel);
      tft.drawCentreString("DRAW APP: X OR TAP SAVE", 160, 125, 1);
    }
    for (uint8_t row = first; row < count && row < first + 4; ++row) draw_row(row, row == selected);
    tft.fillRoundRect(12, 210, 296, 26, 12, panel);
    tft.drawRoundRect(12, 210, 296, 26, 12, tft.color565(35, 48, 82));
    tft.setTextColor(tft.color565(165, 200, 220), panel);
    char footer[48];
    if (count) snprintf(footer, sizeof(footer), "%u/%u  A VIEW  X DELETE  B BACK", selected + 1, count);
    else strcpy(footer, "B BACK");
    tft.drawCentreString(footer, 160, 218, 1);
  };
  auto draw_folder_icon = [&](ScreenshotFolder folder, int x, int y) {
    // Reuse the exact Game Library pixels and their fixed cartridge colors.
    const uint16_t outline = tft.color565(30, 35, 49), red = tft.color565(235, 48, 62);
    if (folder == ScreenshotFolder::POKEMON || folder == ScreenshotFolder::CRYSTAL) {
      constexpr int radius = 8;
      for (int py = -radius; py <= radius; ++py) for (int px = -radius; px <= radius;) {
        const auto color = pokeball_pixel(px, py, radius); const int start = px++;
        while (px <= radius && pokeball_pixel(px, py, radius) == color) ++px;
        if (color != PokeballPixel::CLEAR) {
          const uint16_t colors[] = {0, outline, red, TFT_WHITE};
          tft.drawFastHLine(x + start, y + py, px - start, colors[(uint8_t)color]);
        }
      }
    } else if (folder == ScreenshotFolder::MARIO) {
      constexpr int half_width = 8;
      for (int py = -half_width; py <= half_width; ++py) for (int px = -half_width; px <= half_width;) {
        const auto color = mushroom_pixel(px, py, half_width); const int start = px++;
        while (px <= half_width && mushroom_pixel(px, py, half_width) == color) ++px;
        if (color != MushroomPixel::CLEAR) {
          const uint16_t colors[] = {0, outline, tft.color565(235, 55, 55), TFT_WHITE};
          tft.drawFastHLine(x + start, y + py, px - start, colors[(uint8_t)color]);
        }
      }
    } else if (folder == ScreenshotFolder::ZELDA) { const uint16_t gold = tft.color565(255, 198, 45); tft.fillTriangle(x, y - 8, x - 5, y + 1, x + 5, y + 1, gold); tft.fillTriangle(x - 5, y + 1, x - 10, y + 9, x, y + 9, gold); tft.fillTriangle(x + 5, y + 1, x, y + 9, x + 10, y + 9, gold); }
    else if (folder == ScreenshotFolder::DRAWINGS) { tft.fillCircle(x - 3, y - 3, 5, app_cyan()); tft.fillCircle(x + 4, y + 4, 4, app_pink()); }
    else if (folder == ScreenshotFolder::EDITS) { tft.drawLine(x - 7, y + 7, x + 7, y - 7, app_green()); tft.fillCircle(x - 7, y + 7, 2, app_pink()); }
    else { tft.drawRoundRect(x - 8, y - 7, 16, 14, 4, app_cyan()); tft.fillCircle(x, y, 3, app_green()); }
  };
  uint8_t folder_selected = 0;
  static constexpr ScreenshotFolder folders[] = { ScreenshotFolder::POKEMON, ScreenshotFolder::CRYSTAL, ScreenshotFolder::MARIO, ScreenshotFolder::ZELDA,
      ScreenshotFolder::GAMES, ScreenshotFolder::DRAWINGS, ScreenshotFolder::EDITS };
  auto draw_folders = [&]() {
    app_background(); app_header("SCREENSHOTS // FOLDERS");
    for (uint8_t i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i) {
      const int y = 43 + i * 20; const uint16_t card = i == folder_selected ? chriscade_theme_card(2) : app_panel();
      tft.fillRoundRect(16, y, 288, 18, 8, card); tft.drawRoundRect(16, y, 288, 18, 8, i == folder_selected ? app_cyan() : tft.color565(50, 57, 92));
      draw_folder_icon(folders[i], 37, y + 10); const char* labels[] = { "POKEMON YELLOW", "POKEMON CRYSTAL", "MARIO", "ZELDA", "OTHER GAMES", "DRAWINGS", "EDITED COPIES" };
      tft.setTextColor(TFT_WHITE, card); tft.drawString(labels[i], 58, y + 6, 1);
    }
    tft.setTextColor(tft.color565(165, 200, 220), app_bg()); tft.drawCentreString("UP/DOWN SELECT  //  A OPEN  //  B BACK", 160, 218, 1);
  };
  draw_folders(); wait_app_buttons_released();
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B)) { wait_app_buttons_released(); return; }
    if (!readJoypad(PIN_A)) { active_folder = folders[folder_selected]; wait_app_buttons_released(); break; }
    if (!readJoypad(PIN_UP) || !readJoypad(PIN_DOWN)) {
      const uint8_t old = folder_selected;
      constexpr uint8_t folder_count = sizeof(folders) / sizeof(folders[0]);
      folder_selected = !readJoypad(PIN_DOWN) ? (folder_selected + 1) % folder_count : (folder_selected + folder_count - 1) % folder_count;
      app_button_sound(520);
      // Do not clear/redraw the animated backdrop while scrolling folders.
      const int old_y = 43 + old * 20, new_y = 43 + folder_selected * 20;
      const char* labels[] = { "POKEMON YELLOW", "POKEMON CRYSTAL", "MARIO", "ZELDA", "OTHER GAMES", "DRAWINGS", "EDITED COPIES" };
      auto paint_folder = [&](uint8_t i, bool active) {
        const int y = 43 + i * 20; const uint16_t card = active ? chriscade_theme_card(2) : app_panel();
        tft.fillRoundRect(16, y, 288, 18, 8, card); tft.drawRoundRect(16, y, 288, 18, 8, active ? app_cyan() : tft.color565(50, 57, 92));
        draw_folder_icon(folders[i], 37, y + 10); tft.setTextColor(TFT_WHITE, card); tft.drawString(labels[i], 58, y + 6, 1);
      };
      (void)old_y; (void)new_y; paint_folder(old, false); paint_folder(folder_selected, true); delay(140); continue;
    }
    delay(5);
  }
  read_slots(); draw(); wait_app_buttons_released();
  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) { wait_app_buttons_released(); return; }
    if (count && (!readJoypad(PIN_DOWN) || !readJoypad(PIN_UP))) {
      const uint8_t old = selected, old_first = first;
      selected = !readJoypad(PIN_DOWN) ? (selected + 1) % count : (selected + count - 1) % count;
      if (selected < first) first = selected;
      if (selected >= first + 4) first = selected - 3;
      app_button_sound(520);
      // Selection changes repaint only the two cards: the themed backdrop and
      // header remain in place, eliminating the visible full-screen flash.
      if (first == old_first) { draw_row(old, false); draw_row(selected, true); }
      else draw();
      delay(145);
      continue;
    }
    if (count && !readJoypad(PIN_A)) {
      ScreenshotViewAction action = view_screenshot(slots[selected]);
      while (action == ScreenshotViewAction::NEXT || action == ScreenshotViewAction::PREVIOUS) {
        selected = action == ScreenshotViewAction::NEXT ? (selected + 1) % count : (selected + count - 1) % count;
        action = view_screenshot(slots[selected]);
      }
      if (action == ScreenshotViewAction::RENAME) { screenshot_rename_keyboard(slots[selected]); draw(); continue; }
      if (action == ScreenshotViewAction::DRAW) { char name[32]; screenshot_name(slots[selected], name); drawing_app(name); read_slots(); draw(); continue; }
      if (action == ScreenshotViewAction::DELETE && screenshot_confirm_delete(slots[selected])) {
        char name[32]; screenshot_name(slots[selected], name); { auto scope = UseSDPinFunctionScope(); sd.remove(name); }
        app_button_sound(350); read_slots(); draw(); continue;
      }
      draw(); wait_app_buttons_released(); continue;
    }
    if (count && !readJoypad(PIN_BUTTON_X)) {
      if (screenshot_confirm_delete(slots[selected])) {
        char name[32]; screenshot_name(slots[selected], name);
        auto scope = UseSDPinFunctionScope();
        sd.remove(name);
        app_button_sound(350);
      }
      read_slots(); draw(); wait_app_buttons_released(); continue;
    }
    uint16_t x, y;
    const bool pressed = app_touch(&x, &y);
    if (pressed && !was_pressed) {
      if (app_home_hit(x, y)) return;
      const int row = y >= 52 && y < 52 + 4 * 37 ? ((int)y - 52) / 37 : -1;
      if (row >= 0 && first + row < count) {
        selected = first + (uint8_t)row;
        while (app_touch(&x, &y)) delay(5);
        ScreenshotViewAction action = view_screenshot(slots[selected]);
        while (action == ScreenshotViewAction::NEXT || action == ScreenshotViewAction::PREVIOUS) {
          selected = action == ScreenshotViewAction::NEXT ? (selected + 1) % count : (selected + count - 1) % count;
          action = view_screenshot(slots[selected]);
        }
        if (action == ScreenshotViewAction::DRAW) { char name[32]; screenshot_name(slots[selected], name); drawing_app(name); read_slots(); }
        else if (action == ScreenshotViewAction::RENAME) screenshot_rename_keyboard(slots[selected]);
        else if (action == ScreenshotViewAction::DELETE && screenshot_confirm_delete(slots[selected])) {
          char name[32]; screenshot_name(slots[selected], name); { auto scope = UseSDPinFunctionScope(); sd.remove(name); } read_slots();
        }
        draw(); was_pressed = false; continue;
      }
    }
    was_pressed = pressed;
    delay(5);
  }
}

static void drawing_app(const char* base_name) {
  static const uint16_t palette_rgb[][3] = {
    {12, 18, 34}, {35, 210, 225}, {245, 55, 175}, {70, 220, 120}, {255, 180, 35},
    {245, 245, 235}, {235, 70, 70}, {255, 224, 55}, {70, 125, 245}, {160, 90, 230},
    {35, 175, 165}, {155, 235, 80}, {150, 88, 42}, {130, 145, 165}, {18, 24, 42}
  };
  const uint16_t paper = tft.color565(235, 238, 229);
  uint16_t colors[16];
  for (int i = 0; i < 15; ++i)
    colors[i] = tft.color565(palette_rgb[i][0], palette_rgb[i][1], palette_rgb[i][2]);
  colors[DrawingCanvas::PAPER] = paper;
  uint32_t work_size = 0;
  uint8_t* work = lcd_boot_work_area(&work_size);
  hard_assert(work_size >= DrawingCanvas::BYTES + DISPLAY_WIDTH * sizeof(uint16_t));
  DrawingCanvas canvas(work);
  canvas.clear();
  // Use spare space after the packed image for SD export's single RGB565 row.
  uint16_t* save_line = reinterpret_cast<uint16_t*>(work + DrawingCanvas::BYTES);
  struct UndoPixel { uint16_t position; uint8_t previous; };
  // The drawing canvas occupies 38,400 bytes of the two idle GB frame buffers.
  // The remaining space preserves many normal stylus strokes without taking
  // gameplay RAM. Each stroke has a range in this append-only history.
  UndoPixel* undo_pixels = reinterpret_cast<UndoPixel*>(save_line + DISPLAY_WIDTH);
  const uint16_t undo_capacity = (uint16_t)((work_size - DrawingCanvas::BYTES -
      DISPLAY_WIDTH * sizeof(uint16_t)) / sizeof(UndoPixel));
  static constexpr uint8_t UNDO_STROKES = 24;
  uint16_t undo_start[UNDO_STROKES] = {}, undo_end[UNDO_STROKES] = {};
  uint16_t undo_used = 0;
  uint8_t undo_count = 0;
  bool recording_stroke = false;
  bool stroke_overflow = false;
  uint8_t selected_color = 1;
  uint8_t recent_colors[5] = {1, 2, 3, 4, 5};
  uint8_t brush = 3;

  auto begin_stroke = [&]() {
    recording_stroke = undo_count < UNDO_STROKES && undo_used < undo_capacity;
    stroke_overflow = false;
    if (recording_stroke) undo_start[undo_count] = undo_used;
  };
  auto draw_brush_segment = [&](int x0, int y0, int x1, int y1) {
    canvas.segment_tracked(x0, y0, x1, y1, brush, selected_color,
      [&](int x, int y, uint8_t previous) {
        if (!recording_stroke) return;
        if (undo_used >= undo_capacity) { recording_stroke = false; stroke_overflow = true; return; }
        undo_pixels[undo_used++] = {(uint16_t)(y * DISPLAY_WIDTH + x), previous};
      }, [&](int x, int y, int width) {
      tft.drawFastHLine(x, y, width, colors[selected_color]);
    });
  };
  auto finish_stroke = [&]() {
    if (recording_stroke && undo_used > undo_start[undo_count]) {
      undo_end[undo_count++] = undo_used;
    } else if (stroke_overflow) {
      undo_used = undo_start[undo_count]; // never leave a partial undo record
    }
    recording_stroke = false;
  };

  auto use_color = [&](uint8_t color) {
    selected_color = color;
    uint8_t ordered[5] = {color, 0, 0, 0, 0}; unsigned out = 1;
    for (unsigned i = 0; i < 5 && out < 5; ++i)
      if (recent_colors[i] != color) ordered[out++] = recent_colors[i];
    memcpy(recent_colors, ordered, sizeof(recent_colors));
  };

  auto draw_base = [&]() -> bool {
    if (!base_name) return true;
    auto scope = UseSDPinFunctionScope();
    FsFile file; ScreenshotHeader header = {};
    if (!file.open(base_name, O_RDONLY) || file.read(&header, sizeof(header)) != sizeof(header) ||
        !DrawingPicture::valid(header, file.fileSize())) { file.close(); return false; }
    const bool old_swap = tft.getSwapBytes(); tft.setSwapBytes(true);
    for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
      if (file.read(save_line, DISPLAY_WIDTH * sizeof(uint16_t)) != DISPLAY_WIDTH * sizeof(uint16_t)) {
        file.close(); tft.setSwapBytes(old_swap); return false;
      }
      tft.pushImage(0, y, DISPLAY_WIDTH, 1, save_line);
    }
    file.close(); tft.setSwapBytes(old_swap); return true;
  };

  auto save = [&]() {
    const uint16_t bg = app_canvas_at(24);
    tft.fillRect(13, 27, 178, 12, bg);
    tft.setTextColor(app_cyan(), bg);
    tft.drawString("SAVING DRAWING...", 14, 28, 1);
  char name[32] = {};
    const DrawingSave result = base_name ? save_edited_screenshot(base_name, canvas, colors, save_line, name)
                                        : save_drawing(canvas, colors, save_line, name);
    char status[40];
    if (result == DrawingSave::SAVED) snprintf(status, sizeof(status), "SAVED %s", name);
    else if (result == DrawingSave::FULL) snprintf(status, sizeof(status), "GALLERY FULL // DELETE SHOT");
    else snprintf(status, sizeof(status), "SD ERROR // RETRY X");
    // Keep status updates left of SAVE now that it shares HOME's row.
    tft.fillRect(13, 27, 178, 12, bg);
    tft.setTextColor(result == DrawingSave::SAVED ? app_green() : app_pink(), bg);
    tft.drawString(status, 14, 28, 1);
    app_button_sound(result == DrawingSave::SAVED ? 900 : 250);
  };

  auto paint_canvas = [&]() -> bool {
    if (base_name) {
      auto scope = UseSDPinFunctionScope(); FsFile file; ScreenshotHeader header = {};
      if (!file.open(base_name, O_RDONLY) || file.read(&header, sizeof(header)) != sizeof(header) ||
          !DrawingPicture::valid(header, file.fileSize())) { file.close(); return false; }
      const bool old_swap = tft.getSwapBytes(); tft.setSwapBytes(true);
      for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
        if (file.read(save_line, DISPLAY_WIDTH * sizeof(uint16_t)) != DISPLAY_WIDTH * sizeof(uint16_t)) { file.close(); tft.setSwapBytes(old_swap); return false; }
        if (y >= 42 && y <= 200) tft.pushImage(8, y, 304, 1, save_line + 8);
      }
      file.close(); tft.setSwapBytes(old_swap);
    } else {
      tft.fillRoundRect(6, 40, 308, 163, 12, paper);
      tft.drawRoundRect(6, 40, 308, 163, 12, app_cyan());
    }
    // Reapply transparent ink after the base pixels are restored.
    for (int y = 42; y <= 200; ++y) for (int x = 8; x <= 311;) {
      while (x <= 311 && canvas.at(x, y) == DrawingCanvas::PAPER) ++x;
      const int start = x;
      while (x <= 311 && canvas.at(x, y) != DrawingCanvas::PAPER) ++x;
      int run = start;
      while (run < x) {
        const uint8_t ink = canvas.at(run, y); int end = run + 1;
        while (end < x && canvas.at(end, y) == ink) ++end;
        tft.drawFastHLine(run, y, end - run, colors[ink]); run = end;
      }
    }
    return true;
  };

  auto redraw = [&]() {
    app_background();
    paint_canvas();
    // A copied photo is deliberately beneath the app chrome; HOME, SAVE and
    // palette controls remain usable and visually clear while editing.
    app_header(base_name ? "DRAW COPY // X OR SAVE" : "DRAW // X OR TAP SAVE");
    chriscade_header_button(195, 54, "SAVE", app_panel(), app_cyan());
    tft.fillRoundRect(8, 210, 54, 24, 11, app_panel());
    tft.drawRoundRect(8, 210, 54, 24, 11, app_pink());
    tft.setTextColor(TFT_WHITE, app_panel());
    tft.drawCentreString("CLEAR", 35, 217, 1);
    tft.fillRoundRect(66, 210, 52, 24, 11, app_panel());
    tft.drawRoundRect(66, 210, 52, 24, 11, app_cyan());
    tft.drawCentreString("SIZE", 92, 217, 1);
    tft.fillRoundRect(122, 210, 48, 24, 11, app_panel());
    tft.drawRoundRect(122, 210, 48, 24, 11, undo_count ? app_green() : tft.color565(55, 70, 105));
    tft.setTextColor(TFT_WHITE, app_panel()); tft.drawCentreString("UNDO", 146, 217, 1);
    tft.fillRoundRect(174, 210, 48, 24, 11, app_panel());
    tft.drawRoundRect(174, 210, 48, 24, 11, app_green());
    tft.setTextColor(TFT_WHITE, app_panel()); tft.drawCentreString("MORE", 198, 217, 1);
    for (int i = 0; i < 5; ++i) {
      int x = 236 + i * 16;
      tft.fillCircle(x, 222, 8, colors[recent_colors[i]]);
      tft.drawCircle(x, 222, selected_color == recent_colors[i] ? 11 : 9,
          selected_color == recent_colors[i] ? TFT_WHITE : tft.color565(55, 70, 105));
    }
  };

  auto palette_picker = [&]() {
    app_background(); app_header("DRAW // COLOR SPECTRUM");
    const uint16_t panel = app_panel();
    tft.setTextColor(tft.color565(165, 200, 220), app_bg());
    tft.drawCentreString("TAP A COLOR // B BACK", 160, 38, 1);
    for (int i = 0; i < 15; ++i) {
      const int x = 24 + (i % 5) * 59, y = 53 + (i / 5) * 49;
      tft.fillRoundRect(x, y, 49, 39, 11, colors[i]);
      tft.drawRoundRect(x, y, 49, 39, 11, selected_color == i ? TFT_WHITE : panel);
    }
    wait_app_buttons_released(); bool previous = false;
    while (true) {
      chriscade_power_poll(false);
      if (!readJoypad(PIN_B)) { wait_app_buttons_released(); redraw(); return; }
      uint16_t x, y; const bool pressed = app_touch(&x, &y);
      if (pressed && !previous && y >= 53 && y < 200 && x >= 24 && x < 319) {
        const int col = ((int)x - 24) / 59, row = ((int)y - 53) / 49, index = row * 5 + col;
        if (index >= 0 && index < 15 && x <= 24 + col * 59 + 49 && y <= 53 + row * 49 + 39) {
          use_color((uint8_t)index); app_button_sound(680);
          while (app_touch(&x, &y)) delay(5); redraw(); return;
        }
      }
      previous = pressed; delay(5);
    }
  };
  redraw();
  wait_app_buttons_released();

  bool previous_touch = false;
  int previous_x = 0, previous_y = 0;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_app_buttons_released();
      return;
    }
    if (!readJoypad(PIN_BUTTON_X)) {
      wait_app_buttons_released();
      save();
      previous_touch = false;
      continue;
    }
    uint16_t x, y;
    bool pressed = app_touch(&x, &y);
    if (pressed && app_home_hit(x, y)) {
      while (app_touch(&x, &y)) delay(5);
      return;
    }
    if (pressed && x >= 195 && x <= 249 && y >= 8 && y <= 30) {
      while (app_touch(&x, &y)) { chriscade_power_poll(false); delay(5); }
      save();
      previous_touch = false;
      continue;
    }
    if (pressed && y >= 43 && y <= 200 && x >= 9 && x <= 311) {
      if (previous_touch) {
        draw_brush_segment(previous_x, previous_y, x, y);
      } else {
        begin_stroke();
        draw_brush_segment(x, y, x, y);
      }
      previous_x = x;
      previous_y = y;
      previous_touch = true;
    } else if (pressed && y >= 207) {
      // Some touch controllers occasionally miss the pen-up edge. A toolbar
      // press is an unambiguous end of the current stroke, so commit it here
      // before handling Undo/colour controls.
      if (previous_touch) finish_stroke();
      previous_touch = false;
      if (x < 63) {
        canvas.clear();
        undo_used = 0; undo_count = 0; recording_stroke = false;
        if (base_name) redraw();
        else {
          tft.fillRoundRect(6, 40, 308, 163, 12, paper);
          tft.drawRoundRect(6, 40, 308, 163, 12, app_cyan());
        }
      } else if (x < 121) {
        brush = brush == 3 ? 6 : brush == 6 ? 9 : 3;
        tft.fillRoundRect(66, 210, 52, 24, 11, app_panel());
        tft.drawRoundRect(66, 210, 52, 24, 11, app_cyan());
        tft.setTextColor(TFT_WHITE, app_panel());
        tft.drawCentreString(brush == 3 ? "SMALL" : brush == 6 ? "MED" : "LARGE",
            92, 217, 1);
      } else if (x < 172) {
        if (undo_count) {
          const uint16_t start = undo_start[--undo_count], end = undo_end[undo_count];
          for (uint16_t i = end; i-- > start;) {
            const UndoPixel& pixel = undo_pixels[i];
            canvas.set(pixel.position % DISPLAY_WIDTH, pixel.position / DISPLAY_WIDTH, pixel.previous);
          }
          undo_used = start; app_button_sound(410); paint_canvas();
        } else app_button_sound(250);
      } else if (x < 224) {
        while (app_touch(&x, &y)) delay(5);
        palette_picker(); previous_touch = false; continue;
      } else {
        int index = ((int)x - 224) / 16;
        if (index < 0) index = 0; if (index > 4) index = 4;
        use_color(recent_colors[index]);
        app_button_sound(620); redraw();
      }
      while (app_touch(&x, &y)) delay(5);
    } else {
      if (previous_touch) finish_stroke();
      previous_touch = false;
    }
    delay(4);
  }
}

static void calculator_app() {
  static const char* labels[4][5] = {
    {"7", "8", "9", "/", "C"},
    {"4", "5", "6", "*", "DEL"},
    {"1", "2", "3", "-", "+/-"},
    {"0", ".", "=", "+", "HOME"},
  };
  char entry[24] = "0";
  double accumulator = 0.0;
  char pending = 0;
  bool fresh_entry = true;
  bool error_state = false;
  uint8_t cursor_row = 0, cursor_col = 0;

  auto draw_display = [&]() {
    const uint16_t display = tft.color565(7, 13, 31);
    tft.fillRoundRect(10, 42, 300, 37, 14, display);
    tft.drawRoundRect(10, 42, 300, 37, 14, app_cyan());
    tft.setTextColor(error_state ? app_pink() : TFT_WHITE, display);
    tft.drawRightString(entry, 295, 51, 2);
    if (pending) {
      char op[2] = {pending, 0};
      tft.setTextColor(app_pink(), display);
      tft.drawString(op, 22, 54, 2);
    }
  };

  auto draw_key = [&](int row, int col) {
    int x = 7 + col * 61;
    int y = 85 + row * 37;
    uint16_t button = (col == 3 || !strcmp(labels[row][col], "="))
        ? tft.color565(17, 92, 105)
        : (!strcmp(labels[row][col], "C") || !strcmp(labels[row][col], "DEL") ||
           !strcmp(labels[row][col], "HOME"))
            ? tft.color565(72, 24, 79) : app_panel();
    tft.fillRoundRect(x, y, 55, 31, 10, button);
    tft.drawRoundRect(x, y, 55, 31, 10,
        (row == cursor_row && col == cursor_col) ? app_green() :
            (col == 3 ? app_cyan() : tft.color565(50, 57, 92)));
    tft.setTextColor(TFT_WHITE, button);
    tft.drawCentreString(labels[row][col], x + 27, y + 8,
        strlen(labels[row][col]) > 2 ? 1 : 2);
  };

  auto redraw = [&]() {
    app_background();
    app_header("APP LIBRARY // CALCULATOR", false);
    draw_display();
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 5; ++col) {
        draw_key(row, col);
      }
    }
  };

  auto set_number = [&](double value) {
    if (value > 999999999.0 || value < -999999999.0) {
      strcpy(entry, "OVERFLOW");
      error_state = true;
      return;
    }
    snprintf(entry, sizeof(entry), "%.8g", value);
  };

  auto apply_pending = [&](double rhs) {
    if (!pending) { accumulator = rhs; return; }
    if (pending == '+') accumulator += rhs;
    else if (pending == '-') accumulator -= rhs;
    else if (pending == '*') accumulator *= rhs;
    else if (pending == '/') {
      if (rhs == 0.0) {
        strcpy(entry, "DIVIDE BY ZERO");
        error_state = true;
        pending = 0;
        return;
      }
      accumulator /= rhs;
    }
    set_number(accumulator);
  };

  auto activate_key = [&](const char* key) {
    unsigned int click_frequency = 540;
    if (!strcmp(key, "C") || !strcmp(key, "DEL")) click_frequency = 330;
    else if (!strcmp(key, "HOME")) click_frequency = 420;
    else if (!strcmp(key, "=")) click_frequency = 880;
    else if (key[0] == '+' || key[0] == '-' || key[0] == '*' || key[0] == '/')
      click_frequency = 690;
    app_button_sound(click_frequency);
    if (!strcmp(key, "HOME")) return false;
    if (!strcmp(key, "C")) {
      strcpy(entry, "0"); accumulator = 0.0; pending = 0;
      fresh_entry = true; error_state = false;
    } else if (!strcmp(key, "DEL")) {
      if (!error_state && !fresh_entry) {
        size_t length = strlen(entry);
        if (length > 1) entry[length - 1] = 0;
        else strcpy(entry, "0");
      }
    } else if (!strcmp(key, "+/-")) {
      if (!error_state) {
        if (entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
        else if (strcmp(entry, "0")) {
          size_t length = strlen(entry);
          if (length + 1 < sizeof(entry)) {
            memmove(entry + 1, entry, length + 1); entry[0] = '-';
          }
        }
      }
    } else if (key[0] >= '0' && key[0] <= '9') {
      if (error_state || fresh_entry) {
        entry[0] = key[0]; entry[1] = 0;
        fresh_entry = false; error_state = false;
      } else if (strlen(entry) < sizeof(entry) - 2 &&
                 !(entry[0] == '0' && entry[1] == 0)) {
        size_t length = strlen(entry); entry[length] = key[0]; entry[length + 1] = 0;
      } else if (entry[0] == '0' && entry[1] == 0) entry[0] = key[0];
    } else if (key[0] == '.') {
      if (error_state || fresh_entry) {
        strcpy(entry, "0."); fresh_entry = false; error_state = false;
      } else if (!strchr(entry, '.') && strlen(entry) < sizeof(entry) - 2) {
        strcat(entry, ".");
      }
    } else if (key[0] == '=') {
      if (!error_state) { apply_pending(strtod(entry, NULL)); pending = 0; fresh_entry = true; }
    } else if (!error_state) {
      double value = strtod(entry, NULL);
      if (pending && !fresh_entry) apply_pending(value); else accumulator = value;
      pending = key[0]; fresh_entry = true;
    }
    draw_display();
    return true;
  };

  redraw();
  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_app_buttons_released();
      return;
    }
    if (!readJoypad(PIN_A)) {
      const bool keep_open = activate_key(labels[cursor_row][cursor_col]);
      wait_app_buttons_released();
      if (!keep_open) return;
      continue;
    }
    if (!readJoypad(PIN_LEFT) || !readJoypad(PIN_RIGHT) ||
        !readJoypad(PIN_UP) || !readJoypad(PIN_DOWN)) {
      const uint8_t old_row = cursor_row, old_col = cursor_col;
      if (!readJoypad(PIN_LEFT)) cursor_col = (cursor_col + 4) % 5;
      else if (!readJoypad(PIN_RIGHT)) cursor_col = (cursor_col + 1) % 5;
      else if (!readJoypad(PIN_UP)) cursor_row = (cursor_row + 3) % 4;
      else cursor_row = (cursor_row + 1) % 4;
      app_button_sound(500);
      draw_key(old_row, old_col);
      draw_key(cursor_row, cursor_col);
      delay(130); continue;
    }

    uint16_t tx, ty;
    bool pressed = app_touch(&tx, &ty);
    if (pressed && !was_pressed && ty >= 85) {
      int col = ((int)tx - 7) / 61;
      int row = ((int)ty - 85) / 37;
      if (col >= 0 && col < 5 && row >= 0 && row < 4) {
        cursor_row = (uint8_t)row; cursor_col = (uint8_t)col;
        if (!activate_key(labels[row][col])) return;
      }
    }
    was_pressed = pressed;
    delay(5);
  }
}

static void timer_app() {
  bool stopwatch_mode = false;
  uint32_t countdown_seconds = 0;
  uint32_t countdown_end_ms = 0;
  bool countdown_running = false;
  uint32_t stopwatch_elapsed_ms = 0;
  uint32_t stopwatch_started_ms = 0;
  bool stopwatch_running = false;
  uint32_t last_display_key = UINT32_MAX;

  auto draw_time = [&]() {
    uint32_t now = millis();
    uint32_t display_key;
    char value[16];
    const char* status;
    bool active;

    if (stopwatch_mode) {
      uint32_t elapsed = stopwatch_elapsed_ms +
          (stopwatch_running ? now - stopwatch_started_ms : 0u);
      uint32_t hundredths = elapsed / 10u;
      snprintf(value, sizeof(value), "%02lu:%02lu.%02lu",
          (unsigned long)(hundredths / 6000u),
          (unsigned long)((hundredths / 100u) % 60u),
          (unsigned long)(hundredths % 100u));
      display_key = hundredths;
      status = stopwatch_running ? "STOPWATCH RUNNING" : "STOPWATCH READY";
      active = stopwatch_running;
    } else {
      uint32_t shown = countdown_seconds;
      if (countdown_running) {
        int32_t left = (int32_t)(countdown_end_ms - now);
        shown = left > 0 ? ((uint32_t)left + 999u) / 1000u : 0u;
      }
      snprintf(value, sizeof(value), "%02lu:%02lu",
          (unsigned long)(shown / 60u), (unsigned long)(shown % 60u));
      display_key = shown;
      status = countdown_running ? "COUNTING DOWN" : "TIMER READY";
      active = countdown_running;
    }

    const uint16_t panel = tft.color565(7, 13, 31);
    tft.fillRoundRect(35, 45, 250, 57, 25, panel);
    tft.drawRoundRect(35, 45, 250, 57, 25, active ? app_green() : app_cyan());
    tft.setTextColor(TFT_WHITE, panel);
    tft.drawCentreString(value, DISPLAY_WIDTH / 2, 62, 2);
    tft.setTextColor(active ? app_green() : tft.color565(125, 155, 180), panel);
    tft.drawCentreString(status, DISPLAY_WIDTH / 2, 84, 1);
    last_display_key = display_key;
  };

  auto button = [&](int x, int y, int w, int h, const char* label, uint16_t color) {
    tft.fillRoundRect(x, y, w, h, 13, color);
    tft.drawRoundRect(x, y, w, h, 13, tft.color565(60, 73, 110));
    tft.setTextColor(TFT_WHITE, color);
    tft.drawCentreString(label, x + w / 2, y + h / 2 - 5,
        strlen(label) > 7 ? 1 : 2);
  };

  auto redraw = [&](bool full = true) {
    if (full) {
      app_background();
      app_header("APP LIBRARY // TIME");
    }
    draw_time();

    const uint16_t selected_tab = tft.color565(58, 25, 78);
    button(38, 108, 116, 25, "TIMER", stopwatch_mode ? app_panel() : selected_tab);
    button(166, 108, 116, 25, "STOPWATCH", stopwatch_mode ? selected_tab : app_panel());

    // The two modes use different-width controls. Clear their shared row
    // before painting it so a previous mode cannot leave button fragments.
    tft.fillRoundRect(8, 136, 304, 38, 14, app_panel());
    if (!stopwatch_mode) {
      button(13, 139, 92, 31, "+1 SEC", app_panel());
      button(114, 139, 92, 31, "+1 MIN", app_panel());
      button(215, 139, 92, 31, "+5 MIN", app_panel());
    } else {
      tft.fillRoundRect(54, 141, 212, 27, 13, app_panel());
      tft.setTextColor(tft.color565(145, 180, 205), app_panel());
      tft.drawCentreString("PRECISION  //  1/100 SECOND", DISPLAY_WIDTH / 2, 150, 1);
    }
    const bool active = stopwatch_mode ? stopwatch_running : countdown_running;
    button(20, 181, 132, 41, active ? "PAUSE" : "START",
        tft.color565(13, 91, 105));
    button(168, 181, 132, 41, "RESET", tft.color565(72, 24, 79));
  };

  auto ring_alarm_until_stopped = [&]() {
    app_background();
    app_header("APP LIBRARY // TIMER ALARM", false);
    const uint16_t alarm_panel = tft.color565(66, 18, 58);
    tft.fillRoundRect(26, 51, 268, 88, 24, alarm_panel);
    tft.drawRoundRect(26, 51, 268, 88, 24, app_pink());
    tft.setTextColor(TFT_WHITE, alarm_panel);
    tft.drawCentreString("TIME'S UP!", DISPLAY_WIDTH / 2, 70, 2);
    tft.setTextColor(tft.color565(255, 195, 225), alarm_panel);
    tft.drawCentreString("ALARM IS RINGING", DISPLAY_WIDTH / 2, 104, 1);
    button(42, 157, 236, 54, "STOP ALARM", tft.color565(13, 91, 105));
    tft.setTextColor(tft.color565(155, 190, 210));
    tft.drawCentreString("TAP STOP OR PRESS ANY BUTTON",
        DISPLAY_WIDTH / 2, 224, 1);

    static const uint16_t frequencies[] = {1047, 1319, 1568, 0};
    static const uint16_t durations[] = {150, 150, 260, 300};
    uint8_t phase = 0;
    uint32_t next_change = 0;
    bool buttons_armed = readJoypad(PIN_A) && readJoypad(PIN_B) &&
        readJoypad(PIN_START) && readJoypad(PIN_SELECT);
    uint16_t tx = 0, ty = 0;
    bool touch_armed = !app_touch(&tx, &ty);

    chriscade_alarm_sound_begin();
    while (true) {
      chriscade_power_poll(false);
      uint32_t now = millis();
      if ((int32_t)(now - next_change) >= 0) {
        chriscade_alarm_sound_set(frequencies[phase]);
        tft.drawRoundRect(26, 51, 268, 88, 24,
            frequencies[phase] ? app_pink() : app_cyan());
        next_change = now + durations[phase];
        phase = (uint8_t)((phase + 1) % 4);
      }

      bool buttons_released = readJoypad(PIN_A) && readJoypad(PIN_B) &&
          readJoypad(PIN_START) && readJoypad(PIN_SELECT);
      bool stop = false;
      if (!buttons_armed) {
        buttons_armed = buttons_released;
      } else if (!buttons_released) {
        stop = true;
      }

      bool pressed = app_touch(&tx, &ty);
      if (!touch_armed) {
        touch_armed = !pressed;
      } else if (pressed && tx >= 36 && tx <= 284 && ty >= 150 && ty <= 218) {
        stop = true;
      }

      if (stop) break;
      delay(5);
    }
    chriscade_alarm_sound_end();
    wait_app_buttons_released();
    while (app_touch(&tx, &ty)) delay(5);
  };
  redraw();

  bool was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    uint32_t now = millis();
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      wait_app_buttons_released();
      return;
    }

    if (!readJoypad(PIN_A)) {
      app_button_sound(860);
      if (stopwatch_mode) {
        if (stopwatch_running) {
          stopwatch_elapsed_ms += now - stopwatch_started_ms;
          stopwatch_running = false;
        } else { stopwatch_started_ms = now; stopwatch_running = true; }
      } else if (countdown_running) {
        int32_t left = (int32_t)(countdown_end_ms - now);
        countdown_seconds = left > 0 ? ((uint32_t)left + 999u) / 1000u : 0;
        countdown_running = false;
      } else if (countdown_seconds) {
        countdown_end_ms = now + countdown_seconds * 1000u;
        countdown_running = true;
      }
      redraw(false); wait_app_buttons_released(); continue;
    }
    if (!readJoypad(PIN_LEFT) || !readJoypad(PIN_RIGHT)) {
      const bool wanted_stopwatch = !readJoypad(PIN_RIGHT);
      if (wanted_stopwatch != stopwatch_mode) {
        stopwatch_mode = wanted_stopwatch;
        app_button_sound(wanted_stopwatch ? 740 : 620);
        redraw(false);
      }
      delay(145); continue;
    }
    if (!stopwatch_mode && (!readJoypad(PIN_UP) || !readJoypad(PIN_DOWN))) {
      const uint32_t add = !readJoypad(PIN_UP) ? 1 : 60;
      app_button_sound(add == 1 ? 560 : 640);
      if (countdown_running) countdown_end_ms += add * 1000u;
      else countdown_seconds = min(countdown_seconds + add, 5999u);
      draw_time(); delay(145); continue;
    }

    if (countdown_running && !stopwatch_mode) {
      int32_t milliseconds_left = (int32_t)(countdown_end_ms - now);
      uint32_t shown = milliseconds_left > 0 ?
          ((uint32_t)milliseconds_left + 999u) / 1000u : 0u;
      if (shown != last_display_key) draw_time();
      if (milliseconds_left <= 0) {
        countdown_running = false;
        countdown_seconds = 0;
        ring_alarm_until_stopped();
        redraw();
      }
    } else if (stopwatch_mode && stopwatch_running) {
      uint32_t hundredths = (stopwatch_elapsed_ms + now - stopwatch_started_ms) / 10u;
      if (hundredths != last_display_key) draw_time();
    }

    uint16_t tx, ty;
    bool pressed = app_touch(&tx, &ty);
    if (pressed && !was_pressed) {
      if (app_home_hit(tx, ty)) {
        app_button_sound(420);
        return;
      }
      if (ty >= 105 && ty <= 136) {
        bool new_stopwatch_mode = tx >= 160;
        app_button_sound(new_stopwatch_mode ? 740 : 620);
        if (new_stopwatch_mode != stopwatch_mode) {
          stopwatch_mode = new_stopwatch_mode;
          redraw(false);
        }
        was_pressed = pressed;
        delay(5);
        continue;
      }

      uint32_t add = 0;
      if (!stopwatch_mode && ty >= 136 && ty <= 174) {
        if (tx < 109) add = 1;
        else if (tx < 211) add = 60;
        else add = 300;
      }
      if (add) {
        app_button_sound(add == 1 ? 560 : add == 60 ? 640 : 720);
        if (countdown_running) countdown_end_ms += add * 1000u;
        else countdown_seconds = min(countdown_seconds + add, 5999u);
        draw_time();
      } else if (ty >= 177 && ty <= 226 && tx < 160) {
        app_button_sound(860);
        if (stopwatch_mode) {
          if (stopwatch_running) {
            stopwatch_elapsed_ms += now - stopwatch_started_ms;
            stopwatch_running = false;
          } else {
            stopwatch_started_ms = now;
            stopwatch_running = true;
          }
        } else if (countdown_running) {
          int32_t left = (int32_t)(countdown_end_ms - now);
          countdown_seconds = left > 0 ? ((uint32_t)left + 999u) / 1000u : 0;
          countdown_running = false;
        } else if (countdown_seconds) {
          countdown_end_ms = now + countdown_seconds * 1000u;
          countdown_running = true;
        }
        redraw(false);
      } else if (ty >= 177 && ty <= 226 && tx >= 160) {
        app_button_sound(350);
        if (stopwatch_mode) {
          stopwatch_running = false;
          stopwatch_elapsed_ms = 0;
        } else {
          countdown_running = false;
          countdown_seconds = 0;
        }
        redraw(false);
      }
    }
    was_pressed = pressed;
    delay(5);
  }
}

static void metronome_app() {
  uint16_t bpm = 120;
  bool running = false;
  bool tone_engine_active = false;
  bool tone_sounding = false;
  MetronomePattern pattern;
  int last_beat = -1;
  uint32_t next_beat_ms = 0;
  uint32_t tone_off_ms = 0;
  uint32_t previous_tap_ms = 0;
  const uint16_t display_panel = tft.color565(7, 13, 31);

  auto button = [&](int x, int y, int w, int h, const char* label, uint16_t color) {
    tft.fillRoundRect(x, y, w, h, 12, color);
    tft.drawRoundRect(x, y, w, h, 12, tft.color565(60, 73, 110));
    tft.setTextColor(TFT_WHITE, color);
    tft.drawCentreString(label, x + w / 2, y + h / 2 - 5,
        strlen(label) > 6 ? 1 : 2);
  };

  auto draw_beats = [&](int active) {
    tft.fillRect(98, 101, 124, 13, display_panel);
    const int first_x = 160 - (pattern.beats() - 1) * 10;
    for (int i = 0; i < pattern.beats(); ++i) {
      uint16_t color = i == active ? (pattern.accented(i) ? app_pink() : app_green()) :
          tft.color565(50, 65, 92);
      tft.fillCircle(first_x + i * 20, 107, i == active ? 5 : 3, color);
    }
  };

  auto draw_tempo = [&]() {
    tft.fillRoundRect(35, 45, 250, 74, 25, display_panel);
    tft.drawRoundRect(35, 45, 250, 74, 25, running ? app_green() : app_cyan());
    char value[24];
    // In 6/8 each click is an eighth note, not a dotted-quarter beat.
    snprintf(value, sizeof(value), pattern.mode == 2 ? "%u 8TH/MIN" : "%u BPM", bpm);
    tft.setTextColor(TFT_WHITE, display_panel);
    tft.drawCentreString(value, DISPLAY_WIDTH / 2, 62, 2);
    tft.setTextColor(running ? app_green() : tft.color565(125, 155, 180),
        display_panel);
    char status[24];
    snprintf(status, sizeof(status), "%s  //  %s", pattern.label(), running ? "PLAYING" : "READY");
    tft.drawCentreString(status, DISPLAY_WIDTH / 2, 86, 1);
    draw_beats(running ? last_beat : -1);
  };

  auto draw_pattern_controls = [&]() {
    const uint16_t panel = app_panel();
    tft.fillRoundRect(18, 224, 137, 14, 6, panel);
    tft.fillRoundRect(165, 224, 137, 14, 6, panel);
    char mode[16];
    snprintf(mode, sizeof(mode), "X MODE %s", pattern.label());
    tft.setTextColor(app_cyan(), panel);
    tft.drawCentreString(mode, 86, 227, 1);
    tft.setTextColor(pattern.accent_first ? app_pink() : TFT_WHITE, panel);
    tft.drawCentreString(pattern.accent_first ? "Y ACCENT ON" : "Y ACCENT OFF", 233, 227, 1);
  };

  auto draw_controls = [&]() {
    button(13, 129, 67, 34, "-10", app_panel());
    button(88, 129, 67, 34, "-1", app_panel());
    button(164, 129, 67, 34, "+1", app_panel());
    button(239, 129, 67, 34, "+10", app_panel());
    button(18, 177, 137, 43, running ? "STOP" : "START",
        running ? tft.color565(88, 24, 72) : tft.color565(13, 91, 105));
    button(165, 177, 137, 43, "TAP TEMPO", tft.color565(63, 32, 91));
    draw_pattern_controls();
  };

  auto redraw = [&]() {
    app_background();
    app_header("APP LIBRARY // METRONOME");
    draw_tempo();
    draw_controls();
  };

  auto stop_tone_engine = [&]() {
    if (!tone_engine_active) return;
    chriscade_alarm_sound_set(0);
    chriscade_alarm_sound_end();
    tone_engine_active = false;
    tone_sounding = false;
  };

  auto toggle_running = [&]() {
    if (running) {
      running = false;
      stop_tone_engine();
      app_button_sound(360);
    } else {
      app_button_sound(780);
      chriscade_alarm_sound_begin();
      tone_engine_active = true;
      running = true;
      pattern.beat = 0;
      last_beat = -1;
      next_beat_ms = millis();
    }
    draw_tempo();
    draw_controls();
  };

  auto change_bpm = [&](int amount) {
    int changed = (int)bpm + amount;
    if (changed < 40) changed = 40;
    if (changed > 240) changed = 240;
    bpm = (uint16_t)changed;
    if (running) next_beat_ms = millis() + 60000u / bpm;
    else app_button_sound(amount < 0 ? 480 : 650);
    draw_tempo();
  };

  auto tap_tempo = [&]() {
    uint32_t now = millis();
    if (previous_tap_ms) {
      uint32_t interval = now - previous_tap_ms;
      if (interval >= 250 && interval <= 1500) {
        uint32_t tapped = (60000u + interval / 2u) / interval;
        bpm = (uint16_t)min((uint32_t)240, max((uint32_t)40, tapped));
        if (running) next_beat_ms = now;
      }
    }
    previous_tap_ms = now;
    if (!running) app_button_sound(900);
    draw_tempo();
  };

  auto cycle_mode = [&]() {
    pattern.cycle();
    last_beat = -1;
    if (running) {
      chriscade_alarm_sound_set(0);
      tone_sounding = false;
      next_beat_ms = millis();
    } else app_button_sound(700);
    draw_tempo();
    draw_pattern_controls();
  };

  auto toggle_accent = [&]() {
    pattern.toggle_accent();
    if (!running) app_button_sound(pattern.accent_first ? 850 : 500);
    draw_beats(running ? last_beat : -1);
    draw_pattern_controls();
  };

  redraw();
  wait_app_buttons_released();
  bool touch_was_pressed = false;
  bool x_was_pressed = false;
  bool y_was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    uint32_t now = millis();
    if (running && (int32_t)(now - next_beat_ms) >= 0) {
      const uint8_t beat = pattern.next();
      const bool accented = pattern.accented(beat);
      chriscade_alarm_sound_set(accented ? 1568 : 1047);
      tone_sounding = true;
      tone_off_ms = now + (accented ? 42u : 30u);
      last_beat = beat;
      draw_beats(beat);
      next_beat_ms = now + 60000u / bpm;
    }
    if (tone_sounding && (int32_t)(now - tone_off_ms) >= 0) {
      chriscade_alarm_sound_set(0);
      tone_sounding = false;
    }

    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      stop_tone_engine();
      wait_app_buttons_released();
      return;
    }
    // Edge-triggered: holding X/Y must not block beat scheduling or tone cutoff.
    const bool x_pressed = !readJoypad(PIN_BUTTON_X);
    const bool y_pressed = !readJoypad(PIN_BUTTON_Y);
    if (x_pressed && !x_was_pressed) cycle_mode();
    if (y_pressed && !y_was_pressed) toggle_accent();
    x_was_pressed = x_pressed;
    y_was_pressed = y_pressed;
    if (!readJoypad(PIN_A)) {
      wait_app_buttons_released();
      toggle_running();
      delay(100);
    } else if (!readJoypad(PIN_LEFT)) {
      change_bpm(-1); delay(130);
    } else if (!readJoypad(PIN_RIGHT)) {
      change_bpm(1); delay(130);
    } else if (!readJoypad(PIN_DOWN)) {
      change_bpm(-10); delay(130);
    } else if (!readJoypad(PIN_UP)) {
      change_bpm(10); delay(130);
    }

    uint16_t tx, ty;
    bool pressed = app_touch(&tx, &ty);
    if (pressed && !touch_was_pressed) {
      if (app_home_hit(tx, ty)) {
        stop_tone_engine();
        app_button_sound(360);
        return;
      }
      if (ty >= 125 && ty <= 168) {
        if (tx < 84) change_bpm(-10);
        else if (tx < 160) change_bpm(-1);
        else if (tx < 235) change_bpm(1);
        else change_bpm(10);
      } else if (ty >= 173 && ty <= 220) {
        if (tx < 160) toggle_running();
        else tap_tempo();
      } else if (ty >= 224 && ty <= 238 && tx >= 18 && tx <= 302) {
        if (tx < 155) cycle_mode();
        else if (tx >= 165) toggle_accent();
      }
    }
    touch_was_pressed = pressed;
    delay(3);
  }
}

void chriscade_app_library() {
  app_touch_init();
  uint8_t selected = 0;
  static const char* const titles[] = {
    "DRAW", "CALCULATOR", "TIME", "METRONOME", "SCREENSHOTS"
  };
  static const char* const subtitles[] = {
    "TOUCH CANVAS + COLOR PALETTE", "TOUCH KEYPAD",
    "COUNTDOWN + STOPWATCH", "BPM + TAP TEMPO", "VIEW + DELETE SAVED SHOTS"
  };
  static constexpr uint8_t APP_COUNT = 5;
  static constexpr uint8_t APPS_PER_PAGE = 4;

  auto draw_card = [&](uint8_t index, uint8_t local, bool is_selected) {
    draw_app_card(47 + local * 40, titles[index], subtitles[index],
        index, is_selected);
  };

  auto draw_library = [&]() {
    app_background();
    app_header("APP LIBRARY // TOUCH READY");
    const uint8_t first = (selected / APPS_PER_PAGE) * APPS_PER_PAGE;
    const uint8_t visible = min((uint8_t)APPS_PER_PAGE, (uint8_t)(APP_COUNT - first));
    for (uint8_t local = 0; local < visible; ++local)
      draw_card(first + local, local, selected == first + local);
    char page[12];
    snprintf(page, sizeof(page), "PAGE %u/%u", first / APPS_PER_PAGE + 1,
        (APP_COUNT + APPS_PER_PAGE - 1) / APPS_PER_PAGE);
    tft.setTextColor(app_green());
    tft.drawRightString(page, 245, 30, 1);
    const uint16_t footer = app_panel();
    tft.fillRoundRect(12, 210, 296, 26, 12, footer);
    tft.drawRoundRect(12, 210, 296, 26, 12, tft.color565(35, 48, 82));
    tft.setTextColor(tft.color565(165, 200, 220), footer);
    tft.drawCentreString("UP/DOWN SCROLL  //  A OPEN  //  SELECT HOME", DISPLAY_WIDTH / 2, 218, 1);
  };

  auto redraw_selection = [&](uint8_t previous) {
    const uint8_t old_page = previous / APPS_PER_PAGE;
    const uint8_t new_page = selected / APPS_PER_PAGE;
    if (old_page != new_page) { draw_library(); return; }
    const uint8_t first = new_page * APPS_PER_PAGE;
    draw_card(previous, previous - first, false);
    draw_card(selected, selected - first, true);
  };

  auto launch_selected = [&]() {
    if (selected == 0) drawing_app();
    else if (selected == 1) calculator_app();
    else if (selected == 2) timer_app();
    else if (selected == 3) metronome_app();
    else screenshots_app();
  };

  draw_library();
  wait_app_buttons_released();
  bool touch_was_pressed = false;
  while (true) {
    chriscade_power_poll(false);
    if (!readJoypad(PIN_B) || !readJoypad(PIN_SELECT)) {
      app_button_sound(360);
      wait_app_buttons_released();
      return;
    }
    if (!readJoypad(PIN_DOWN)) {
      const uint8_t previous = selected;
      selected = (selected + 1) % APP_COUNT;
      app_button_sound(560);
      redraw_selection(previous);
      delay(150);
    }
    if (!readJoypad(PIN_UP)) {
      const uint8_t previous = selected;
      selected = (selected + APP_COUNT - 1) % APP_COUNT;
      app_button_sound(500);
      redraw_selection(previous);
      delay(150);
    }
    if (!readJoypad(PIN_A)) {
      app_button_sound(780);
      wait_app_buttons_released();
      launch_selected();
      draw_library();
      wait_app_buttons_released();
      touch_was_pressed = false;
      continue;
    }

    uint16_t tx, ty;
    bool pressed = app_touch(&tx, &ty);
    if (pressed && !touch_was_pressed) {
      if (app_home_hit(tx, ty)) {
        app_button_sound(360);
        return;
      }
      const uint8_t first = (selected / APPS_PER_PAGE) * APPS_PER_PAGE;
      int touched = ty >= 45 && ty < 207 ? first + ((int)ty - 47) / 40 : -1;
      if (touched >= APP_COUNT) touched = -1;
      if (touched >= 0) {
        if (selected != touched) {
          const uint8_t previous = selected;
          selected = (uint8_t)touched;
          app_button_sound(560);
          redraw_selection(previous);
        }
        app_button_sound(780);
        while (app_touch(&tx, &ty)) delay(5);
        launch_selected();
        draw_library();
        wait_app_buttons_released();
        touch_was_pressed = false;
        continue;
      }
    }
    touch_was_pressed = pressed;
    delay(5);
  }
}
