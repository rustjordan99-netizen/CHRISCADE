#include "../src/metronome_pattern.h"
#include "../src/touch_control_settings.h"
#include "../src/boot_buttons.h"

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static bool same(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

extern "C" int run_app_controls_tests() {
  CHECK(!BootButtons::pressed(0xFFFFFFFFu));
  for (unsigned pin = 0; pin < 32; ++pin)
    CHECK(BootButtons::pressed(0xFFFFFFFFu ^ (1u << pin)) == (pin < 6));
  for (unsigned held = 0; held < 64; ++held)
    CHECK(BootButtons::pressed(0xFFFFFFFFu ^ held) == (held != 0));
  CHECK(!BootButtons::pressed(0xFFFFFFFFu ^ (1u << 20))); // Sleep is not Start.
  MetronomePattern pattern;
  CHECK(pattern.mode == 1 && pattern.beat == 0 && pattern.accent_first);
  CHECK(pattern.beats() == 4 && same(pattern.label(), "4/4"));
  pattern.next();
  pattern.cycle();
  CHECK(pattern.mode == 2 && pattern.beat == 0);
  CHECK(pattern.beats() == 6 && same(pattern.label(), "6/8"));
  pattern.cycle();
  CHECK(pattern.mode == 0 && pattern.beats() == 3 && same(pattern.label(), "3/4"));
  pattern.cycle();
  CHECK(pattern.mode == 1 && pattern.beats() == 4);

  for (uint8_t mode = 0; mode < 3; ++mode) {
    pattern.mode = mode;
    pattern.beat = 0;
    pattern.accent_first = true;
    const uint8_t expected_beats = mode == 0 ? 3 : mode == 1 ? 4 : 6;
    CHECK(pattern.beats() == expected_beats);
    for (uint8_t bar = 0; bar < 8; ++bar) {
      uint8_t accents = 0;
      for (uint8_t step = 0; step < expected_beats; ++step) {
        const uint8_t beat = pattern.next();
        CHECK(beat == step);
        CHECK(pattern.accented(beat) == (step == 0));
        accents += pattern.accented(beat);
      }
      CHECK(accents == 1 && pattern.beat == 0);
    }
    pattern.next();
    const uint8_t next_beat = pattern.beat;
    pattern.toggle_accent();
    CHECK(!pattern.accent_first && pattern.beat == next_beat && pattern.mode == mode);
    for (uint8_t step = 0; step < expected_beats * 2; ++step)
      CHECK(!pattern.accented(pattern.next()));
    pattern.toggle_accent();
    CHECK(pattern.accented(0) && !pattern.accented(1));
    pattern.cycle();
    CHECK(pattern.beat == 0 && pattern.mode == (mode + 1) % 3);
  }

  using namespace TouchControlSettings;
  CHECK(DEFAULTS == 3 && game_enabled(DEFAULTS) && menu_enabled(DEFAULTS));
  for (uint8_t version = 1; version <= 3; ++version) {
    for (unsigned flags = 0; flags < 256; ++flags) {
      CHECK(valid(version, flags));
      CHECK(migrate(version, flags) == DEFAULTS);
    }
  }
  CHECK(valid(4, 0) && valid(4, 1));
  CHECK(migrate(4, 0) == MAIN_MENU);
  CHECK(migrate(4, 1) == DEFAULTS);
  for (unsigned flags = 2; flags < 256; ++flags) CHECK(!valid(4, flags));
  for (uint8_t flags = 0; flags <= DEFAULTS; ++flags) {
    CHECK(valid(5, flags) && migrate(5, flags) == flags);
    CHECK(game_enabled(flags) == ((flags & GAME) != 0));
    CHECK(menu_enabled(flags) == ((flags & MAIN_MENU) != 0));
    const uint8_t changed_game = flags ^ GAME;
    CHECK(game_enabled(changed_game) != game_enabled(flags));
    CHECK(menu_enabled(changed_game) == menu_enabled(flags));
    const uint8_t changed_menu = flags ^ MAIN_MENU;
    CHECK(menu_enabled(changed_menu) != menu_enabled(flags));
    CHECK(game_enabled(changed_menu) == game_enabled(flags));
    CHECK(migrate(5, changed_game) == changed_game);
    CHECK(migrate(5, changed_menu) == changed_menu);
  }
  for (unsigned flags = 4; flags < 256; ++flags) CHECK(!valid(5, flags));
  return 0;
}
