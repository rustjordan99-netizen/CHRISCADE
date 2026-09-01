#pragma once

#include <hardware/pio.h>

#define chriscade_backlight_pwm_wrap_target 0
#define chriscade_backlight_pwm_wrap 6

static const uint16_t chriscade_backlight_pwm_program_instructions[] = {
    0x9080, // pull noblock side 0
    0xa027, // mov x, osr
    0xa046, // mov y, isr
    0x00a5, // jmp x != y, 5
    0x1806, // jmp 6 side 1
    0xa042, // nop
    0x0083, // jmp y--, 3
};

static const struct pio_program chriscade_backlight_pwm_program = {
    .instructions = chriscade_backlight_pwm_program_instructions,
    .length = 7,
    .origin = -1,
};

static inline pio_sm_config
chriscade_backlight_pwm_program_get_default_config(uint offset) {
  pio_sm_config config = pio_get_default_sm_config();
  sm_config_set_wrap(&config, offset + chriscade_backlight_pwm_wrap_target,
      offset + chriscade_backlight_pwm_wrap);
  sm_config_set_sideset(&config, 2, true, false);
  return config;
}

