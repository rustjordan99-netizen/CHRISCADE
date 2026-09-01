#pragma once

#include <stdint.h>
#include "SdFat.h"

#include "common.h"

#define FILES_PER_PAGE 8
#define MAX_PATH_LENGTH 256

extern SdFs sd;

void init_sdcard();

void read_cart_ram_file(struct gb_s* gb);

bool write_cart_ram_file(struct gb_s* gb);

void rom_file_selector();

class UseSDPinFunctionScope {
public:
  UseSDPinFunctionScope() :
    old_sck_pin_func(gpio_get_function(SD_SCK_PIN)),
    old_mosi_pin_func(gpio_get_function(SD_MOSI_PIN))
  {
    if (sd_sck_pin_func != GPIO_FUNC_NULL) {
      gpio_set_function(SD_SCK_PIN, sd_sck_pin_func);
    }
    if (sd_mosi_pin_func != GPIO_FUNC_NULL) {
      gpio_set_function(SD_MOSI_PIN, sd_mosi_pin_func);
    }
  }

  ~UseSDPinFunctionScope() {
    close();
  }

  static void init() {
    sd_sck_pin_func = gpio_get_function(SD_SCK_PIN);
    sd_mosi_pin_func = gpio_get_function(SD_MOSI_PIN);    
  }

  void close() {
    gpio_set_function(SD_SCK_PIN, old_sck_pin_func);
    gpio_set_function(SD_MOSI_PIN, old_mosi_pin_func);
  }

private:
  static gpio_function_t sd_sck_pin_func;
  static gpio_function_t sd_mosi_pin_func;

  gpio_function_t old_sck_pin_func = gpio_get_function(SD_SCK_PIN);
  gpio_function_t old_mosi_pin_func = gpio_get_function(SD_MOSI_PIN);
};
