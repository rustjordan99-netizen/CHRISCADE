#pragma once

#include <stdint.h>

namespace GameplayAudioCurve {
static constexpr uint16_t max_magnitude = 320;
static constexpr uint16_t gain_numerator = 3;
static constexpr uint16_t gain_denominator = 2;

// Used only when building the existing lookup table at audio initialization.
// Boost the body of all games' audio by 1.5x before the soft knee; loud peaks
// remain limited. No extra arithmetic, buffers or table entries per sample.
inline uint8_t magnitude_level(uint16_t magnitude) {
  constexpr uint32_t knee = 94;
  constexpr uint32_t peak = 127;
  if (magnitude > max_magnitude) magnitude = max_magnitude;
  const uint32_t boosted =
      ((uint32_t)magnitude * gain_numerator) / gain_denominator;
  if (boosted <= knee) return (uint8_t)boosted;
  const uint32_t excess = boosted - knee;
  return (uint8_t)(knee + (excess * (peak - knee)) / (excess + (peak - knee)));
}
} // namespace GameplayAudioCurve
