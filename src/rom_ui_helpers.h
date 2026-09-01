#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class PokeballPixel : uint8_t { CLEAR, OUTLINE, RED, WHITE };
enum class MushroomPixel : uint8_t { CLEAR, OUTLINE, RED, WHITE };

inline MushroomPixel mushroom_pixel(int x, int y, int half_width) {
  if (half_width <= 0) return MushroomPixel::CLEAR;
  // Wide, low cap and a broad stem, shared by the small and loading icons.
  const int nx = x * 18 / half_width;
  const int ny = y * 18 / half_width;
  const int ax = nx < 0 ? -nx : nx;
  const int cap_y = ny + 2;
  const int rim_y = half_width < 9 ? 0 : 2;
  if (ny <= 2 && nx * nx * 196 + cap_y * cap_y * 324 <= 324 * 196) {
    if (nx * nx * 169 + cap_y * cap_y * 289 > 289 * 169 || ny >= rim_y)
      return MushroomPixel::OUTLINE;
    const int center_y = ny + 10;
    // Three large, separate white spots: one at the crown and two on the sides.
    if (nx * nx + center_y * center_y <= 25 ||
        (ax - 11) * (ax - 11) + cap_y * cap_y <= (half_width < 9 ? 25 : 16))
      return MushroomPixel::WHITE;
    return MushroomPixel::RED;
  }
  if (ny > 2 && ny <= 13 && ax <= 9) {
    const int dx = ax > 6 ? ax - 6 : 0;
    const int dy = ny > 10 ? ny - 10 : 0;
    if (dx * dx + dy * dy > 9) return MushroomPixel::CLEAR;
    if (ax >= 8 || ny >= 12 || (ax >= 3 && ax <= 5 && ny >= 6 && ny <= 9))
      return MushroomPixel::OUTLINE;
    return MushroomPixel::WHITE;
  }
  return MushroomPixel::CLEAR;
}

inline PokeballPixel pokeball_pixel(int x, int y, int radius) {
  const int distance = x * x + y * y;
  if (distance > radius * radius) return PokeballPixel::CLEAR;
  const int edge = radius >= 8 ? 2 : 1;
  const int button = radius >= 8 ? 5 : 1;
  if (distance <= (button - 1) * (button - 1)) return PokeballPixel::WHITE;
  if (distance <= button * button ||
      distance > (radius - edge) * (radius - edge) ||
      (y >= -(edge - 1) && y <= edge - 1)) return PokeballPixel::OUTLINE;
  return y < 0 ? PokeballPixel::RED : PokeballPixel::WHITE;
}

// The caller owns the existing menu workspace. Cursor positions are UTF-8 byte
// boundaries; inserting ASCII and backspacing never split existing characters.
struct RomNameEditor {
  char* text;
  size_t maximum, length, cursor;
  RomNameEditor(char* buffer, size_t max_length)
      : text(buffer), maximum(max_length), length(strlen(buffer)), cursor(length) {}
  static bool continuation(char c) { return ((uint8_t)c & 0xC0u) == 0x80u; }
  void left() {
    if (!cursor) return;
    --cursor;
    while (cursor && continuation(text[cursor])) --cursor;
  }
  void right() {
    if (cursor >= length) return;
    ++cursor;
    while (cursor < length && continuation(text[cursor])) ++cursor;
  }
  void place(size_t position) {
    cursor = position < length ? position : length;
    while (cursor && continuation(text[cursor])) --cursor;
  }
  bool insert(char c) {
    if (length >= maximum) return false;
    memmove(text + cursor + 1, text + cursor, length - cursor + 1);
    text[cursor++] = c;
    ++length;
    return true;
  }
  void backspace() {
    const size_t end = cursor;
    left();
    memmove(text + cursor, text + end, length - end + 1);
    length -= end - cursor;
  }
  void clear() { text[0] = '\0'; length = cursor = 0; }
};

enum class RomScrollChange { NONE, ROW, WINDOW, FOOTER };
struct RomLibraryWindow {
  uint16_t first = 0, count = 0;
  uint8_t selected = 0;
  bool add_selected = false;
  template<size_t Rows, size_t Width, class Find>
  RomScrollChange move(bool backwards, char (&names)[Rows][Width], char* next, Find find) {
    if (!count) return RomScrollChange::NONE;
    if (add_selected) {
      if (!backwards) return RomScrollChange::NONE;
      add_selected = false;
      return RomScrollChange::FOOTER;
    }
    if (backwards ? selected > 0 : selected + 1 < count) {
      selected = backwards ? selected - 1 : selected + 1;
      return RomScrollChange::ROW;
    }
    if (backwards && !first) return RomScrollChange::NONE;
    if ((backwards || count == Rows) && find(names[backwards ? 0 : count - 1], next, backwards)) {
      if (backwards) {
        const uint16_t shifted = count < Rows ? count : Rows - 1;
        memmove(names[1], names[0], shifted * Width);
        memcpy(names[0], next, strlen(next) + 1);
        count = shifted + 1;
        --first;
      } else {
        memmove(names[0], names[1], (Rows - 1) * Width);
        memcpy(names[Rows - 1], next, strlen(next) + 1);
        ++first;
      }
      return RomScrollChange::WINDOW;
    }
    if (backwards) return RomScrollChange::NONE;
    add_selected = true;
    return RomScrollChange::FOOTER;
  }
};
