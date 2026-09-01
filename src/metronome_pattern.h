#pragma once
#include <stdint.h>

struct MetronomePattern {
  uint8_t mode = 1; // Retain the original 4/4 default.
  uint8_t beat = 0;
  bool accent_first = true;
  uint8_t beats() const { return mode == 0 ? 3 : mode == 1 ? 4 : 6; }
  const char* label() const { return mode == 0 ? "3/4" : mode == 1 ? "4/4" : "6/8"; }
  void cycle() { mode = (mode + 1) % 3; beat = 0; }
  void toggle_accent() { accent_first = !accent_first; }
  bool accented(uint8_t current) const { return accent_first && current == 0; }
  uint8_t next() { const uint8_t current = beat; beat = (beat + 1) % beats(); return current; }
};
