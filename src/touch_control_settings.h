#pragma once
#include <stdint.h>

namespace TouchControlSettings {
constexpr uint8_t GAME = 1u;
constexpr uint8_t MAIN_MENU = 2u;
constexpr uint8_t DEFAULTS = GAME | MAIN_MENU;

inline bool valid(uint8_t version, uint8_t flags) {
  return version < 4 || (version == 4 ? flags < 2 : flags <= DEFAULTS);
}
inline uint8_t migrate(uint8_t version, uint8_t flags) {
  // v1-3 had no gameplay toggle; v4 stored one boolean in this byte.
  // Keep the persisted record 16 bytes and preserve v4's gameplay choice.
  return version < 4 ? DEFAULTS : version == 4 ? flags | MAIN_MENU : flags;
}
inline bool game_enabled(uint8_t flags) { return (flags & GAME) != 0; }
inline bool menu_enabled(uint8_t flags) { return (flags & MAIN_MENU) != 0; }
} // namespace TouchControlSettings
