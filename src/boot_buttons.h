#pragma once
#include <stdint.h>

namespace BootButtons {
// CrowPanel A/B/X/Y/Start/Select are active-low GP0..GP5.
// GP20 (sleep) and all other GPIOs must not dismiss the startup screen.
constexpr uint32_t MASK = 0x3fu;
inline bool pressed(uint32_t gpio_levels) {
  return (gpio_levels & MASK) != MASK;
}
}
