#pragma once

#include <stdint.h>

// Touch-friendly built-in utility library. Returns to the CHRISCADE launcher
// when the user exits an app.
void chriscade_app_library();

// Shared top-row capsule: identical vertical alignment on apps and settings.
void chriscade_header_button(int x, int width, const char* label,
    uint16_t background, uint16_t border);

// Call on core 0 between game frames. Captures RAM pixels, with a temporary
// cooperative SPI handoff; never resets the LCD core or reads display GRAM.
bool chriscade_capture_game_screen(const char* source);
void chriscade_touch_init();
bool chriscade_touch_read(uint16_t* x, uint16_t* y);
// Discard the saved corner calibration and immediately guide the user through
// a fresh calibration. Game saves and all other settings are untouched.
void chriscade_touch_recalibrate();
