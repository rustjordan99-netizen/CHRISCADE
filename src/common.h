#pragma once

#include <Arduino.h>
#if ENABLE_LCD
#include <TFT_eSPI.h>
#endif
#include <SdFat.h>
#include "gb.h"

#if defined(CROWPANEL_ILI9341)
#if !defined(ILI9341_DRIVER) || TFT_MISO != 12 || TFT_MOSI != 11 || \
    TFT_SCLK != 10 || TFT_CS != 9 || TFT_DC != 8 || TFT_RST != 15 || \
    TFT_BL != 18 || TFT_SPI_PORT != 1
#error "CrowPanel TFT_eSPI setup was not loaded or has the wrong pinout"
#endif
#endif

#define INPUT_GPIO 1
#define INPUT_PCF8574 2
#define INPUT_CROWPANEL 3

/* Joypad Pins. */
//#define USE_JOYPAD_I2C_IO_EXPANDER
#if ENABLE_INPUT == INPUT_PCF8574
// Use PCF8574 for Joypad. Only required if an LCD with 16-bit parallel bus is used,
// as Pico does not have enough pins for all peripherals. With 8-bit parallel or SPI LCDs this should not be necessary.
#define PCF8574_ADDR 0x20
#define PCF8574_SDA 20
#define PCF8574_SCL 21
// pins below are on the IO expander
#define PIN_UP		0
#define PIN_DOWN	1 
#define PIN_LEFT	2
#define PIN_RIGHT	3
#define PIN_A		5
#define PIN_B		4
#define PIN_SELECT	6
#define PIN_START	7
#elif ENABLE_INPUT == INPUT_GPIO
// Use GPIOs directly on Pico for Joypad
#define PIN_UP		2
#define PIN_DOWN	3
#define PIN_LEFT	4
#define PIN_RIGHT	5
#define PIN_A		6
#define PIN_B		7
#define PIN_SELECT	8
#define PIN_START	9
#elif ENABLE_INPUT == INPUT_CROWPANEL
// Elecrow CrowPanel 2.4: six active-low buttons and a two-axis analogue stick.
// Direction identifiers are virtual values interpreted by readJoypad().
#define PIN_UP          100
#define PIN_DOWN        101
#define PIN_LEFT        102
#define PIN_RIGHT       103
#define PIN_A           0
#define PIN_B           1
#define PIN_SELECT      5
#define PIN_START       4
#define PIN_BUTTON_X    2
#define PIN_BUTTON_Y    3
#define PIN_LOW_POWER   20
#define PIN_JOYSTICK_X  26
#define PIN_JOYSTICK_Y  27
#define JOYSTICK_LOW    1400
#define JOYSTICK_HIGH   2700
#endif

#if ENABLE_SOUND
#if defined(CROWPANEL_PWM_AUDIO)
#define CROWPANEL_SPEAKER_PIN 19
#define CROWPANEL_VOLUME_PIN 28
#else
#define I2S_DIN_PIN 26
#define I2S_BCLK_LRC_PIN_BASE 27  // BCLK + LRC (28)
#endif
#endif

#if ENABLE_SDCARD
#define SD_SPI SPI1
#define SD_CS_PIN 22
#define SD_SCK_PIN 10
#define SD_MOSI_PIN 11
#define SD_MISO_PIN 12
#define TOUCH_CS_PIN 16

extern uint8_t _FS_start;
extern uint8_t _FS_end;
#define MAX_ROM_SIZE (&_FS_end - &_FS_start)
#endif

// display is rotated, so TFT_WIDTH/HEIGHT cannot be used
#define DISPLAY_WIDTH TFT_HEIGHT
#define DISPLAY_HEIGHT TFT_WIDTH

#define FONT_HEIGHT 8
#define ERROR_TEXT_OFFSET FONT_HEIGHT
#define FONT_ID 1

enum class ScalingMode {
  NORMAL = 0,
  STRETCH,
  STRETCH_KEEP_ASPECT,
  COUNT
};

extern volatile ScalingMode scalingMode; 

extern uint_fast32_t frames;
extern TFT_eSPI tft;

/* Multicore command structure. */
union core_cmd {
  struct
  {
    /* Does nothing. */
#define CORE_CMD_NOP 0
    /* Set line "data" on the LCD. Pixel data is in pixels_buffer. */
#define CORE_CMD_LCD_LINE 1
    /* Control idle mode on the LCD. Limits colours to 2 bits. */
#define CORE_CMD_IDLE_SET 2
    /* Set a specific pixel. For debugging. */
#define CORE_CMD_SET_PIXEL 3
    /* Draw a completed Game Boy frame. "data" selects its source buffer. */
#define CORE_CMD_LCD_FRAME 4
#define CORE_CMD_SCREENSHOT_PAUSE 5
    uint8_t cmd;
    uint8_t unused1;
    uint8_t unused2;
    uint8_t data;
  };
  uint32_t full;
};

void lcd_init(bool isCore1);
void lcd_draw_line(struct gb_s* gb, const uint8_t* pixels, const uint_fast8_t line);
bool lcd_frame_available();
void lcd_present_pending_frame();
void lcd_restart_after_core_reset();
uint8_t* lcd_boot_work_area(uint32_t* size);
struct GamePicture;
// Core 0 only, between emulated frames. Never reset/reinitialize core 1.
bool lcd_begin_screenshot(GamePicture& picture, uint16_t*& row);
void lcd_end_screenshot();

void core1_init();

void reset(uint32_t sleepMs = 0);

void error(String message);
