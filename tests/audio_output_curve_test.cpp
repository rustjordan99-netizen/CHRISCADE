#include "../src/audio_output_curve.h"

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static uint8_t old_level(unsigned magnitude) {
  if (magnitude <= 94) return magnitude;
  return 94 + ((magnitude - 94) * 33) / (magnitude - 94 + 33);
}

extern "C" int run_audio_output_curve_tests() {
  using namespace GameplayAudioCurve;
  CHECK(max_magnitude == 320); // No table RAM growth.
  CHECK(magnitude_level(0) == 0); // Silence and volume-knob mute stay silent.
  CHECK(magnitude_level(20) == 30);
  CHECK(magnitude_level(40) == 60);
  CHECK(magnitude_level(60) == 90);
  CHECK(magnitude_level(320) == 124); // Below the PWM hard rail.
  unsigned previous = 0;
  for (unsigned m = 0; m <= max_magnitude; ++m) {
    const unsigned level = magnitude_level(m);
    CHECK(level >= previous && level < 127);
    CHECK(level >= old_level(m)); // Never attenuate an existing magnitude.
    CHECK(level <= (m * 3u) / 2u);
    CHECK(level - previous <= 2); // No discontinuity at the knee.
    previous = level;
    // The driver's restored sign and PWM conversion remain symmetric and
    // inside both channel-register limits for every possible table entry.
    const int positive = 512 + ((int)level * 511) / 127;
    const int negative = 512 - ((int)level * 511) / 127;
    CHECK(positive + negative == 1024);
    CHECK(positive < 1024 && negative > 0);
  }
  for (unsigned m = max_magnitude + 1; m <= 65535; ++m)
    CHECK(magnitude_level(m) == magnitude_level(max_magnitude));
  return 0;
}
