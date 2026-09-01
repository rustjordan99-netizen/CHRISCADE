#include <Arduino.h>

#include "chriscade_boot.h"
#include "chriscade_settings.h"
#include "chriscade_apps.h"
#include "boot_buttons.h"
#include "common.h"
#include "gb.h"

#if ENABLE_SDCARD
#include "card_loader.h"
#endif

#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
#include "crowpanel_audio.h"
#endif

#include <hardware/gpio.h>
#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/pll.h>
#include <hardware/pwm.h>
#include <hardware/structs/m0plus.h>
#include <hardware/spi.h>
#include <hardware/watchdog.h>
#include <hardware/xosc.h>
#include <pico/multicore.h>

static constexpr int BOOT_WIDTH = 320;
static constexpr int BOOT_HEIGHT = 240;
static constexpr uint LOW_POWER_BUTTON_PIN = 20;
static constexpr uint STATUS_LED_POWER_PIN = 6;
static constexpr uint STATUS_LED_LOW_POWER_PIN = 7;
static constexpr uint LCD_DC_PIN = 8;
static constexpr uint LCD_CS_PIN = 9;
static constexpr uint LCD_SCK_PIN = 10;
static constexpr uint LCD_MOSI_PIN = 11;
static constexpr uint LCD_MISO_PIN = 12;
static constexpr uint LCD_RESET_PIN = 15;
static constexpr uint LCD_BL_PIN = 18;
static constexpr uint SPEAKER_PIN = 19;
static constexpr uint VOLUME_POT_PIN = 28;
static constexpr uint VOLUME_POT_ADC_INPUT = 2;
static constexpr uint16_t BOOT_VOLUME_MUTE_THRESHOLD = 700;
static constexpr uint16_t BOOT_VOLUME_FULL_THRESHOLD = 4000;
static constexpr uint16_t BOOT_CARRIER_WRAP = 999;
static constexpr uint CP_TOUCH_CS_PIN = 16;
static constexpr uint CP_SD_CS_PIN = 22;
static constexpr uint BATTERY_SENSE_PIN = 29;
static constexpr uint BATTERY_ADC_INPUT = 3;
static constexpr unsigned BATTERY_VALID_MIN_MV = 2500;
static constexpr unsigned BATTERY_LOW_MV = 3300;
static constexpr unsigned BATTERY_RECOVER_MV = 3450;

static unsigned battery_mv_filtered;
static uint32_t battery_last_sample_ms;
static bool battery_low;
static bool battery_sampled;

static void chriscade_battery_update(bool force = false) {
  uint32_t now = millis();
  if (!force && battery_sampled && (uint32_t)(now - battery_last_sample_ms) < 1000u)
    return;

  adc_select_input(BATTERY_ADC_INPUT);
  unsigned measured = ((unsigned)adc_read() * 6600u + 2047u) / 4095u;
  battery_last_sample_ms = now;

  if (!battery_sampled) {
    battery_mv_filtered = measured;
    battery_sampled = true;
  } else {
    battery_mv_filtered = (battery_mv_filtered * 3u + measured + 2u) >> 2;
  }

  if (battery_mv_filtered < BATTERY_VALID_MIN_MV) {
    battery_low = false;
  } else if (!battery_low && battery_mv_filtered <= BATTERY_LOW_MV) {
    battery_low = true;
  } else if (battery_low && battery_mv_filtered >= BATTERY_RECOVER_MV) {
    battery_low = false;
  }

  gpio_put(STATUS_LED_LOW_POWER_PIN, battery_low ? 1 : 0);
}

unsigned int chriscade_battery_millivolts() {
  chriscade_battery_update(false);
  return battery_mv_filtered >= BATTERY_VALID_MIN_MV ? battery_mv_filtered : 0;
}

bool chriscade_battery_is_low() {
  chriscade_battery_update(false);
  return battery_low;
}

// This project uses an Arduino Pico SDK that predates pico/low_power.h. These
// are the RP2040 operations needed by the Doom build's dormant GPIO path.
static void low_power_set_pins_low_leakage(uint32_t exclude_mask) {
  for (uint pin = 0; pin < NUM_BANK0_GPIOS; ++pin) {
    if (exclude_mask & (1u << pin)) continue;
    gpio_disable_pulls(pin);
    gpio_set_input_enabled(pin, false);
    gpio_set_oeover(pin, IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_DISABLE);
  }
}

static void low_power_dormant_until_gpio_falling(uint pin) {
  // Dormant mode only wakes reliably when the CPU is running from the clock
  // source that is being made dormant. Move off the 266 MHz PLL first, then
  // sleep the XOSC; GP20's falling edge restarts it.
  clock_configure_undivided(clk_ref,
      CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, 0, XOSC_HZ);
  clock_configure_undivided(clk_sys,
      CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF, 0, XOSC_HZ);
  clock_stop(clk_adc);
  clock_stop(clk_usb);
  clock_configure(clk_peri, 0,
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, XOSC_HZ, XOSC_HZ);
  pll_deinit(pll_sys);
  pll_deinit(pll_usb);

  gpio_set_input_enabled(pin, true);
  gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_FALL);
  gpio_set_dormant_irq_enabled(pin, GPIO_IRQ_EDGE_FALL, true);
  scb_hw->scr |= M0PLUS_SCR_SLEEPDEEP_BITS;
  xosc_dormant();
  gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_FALL);
  gpio_set_dormant_irq_enabled(pin, GPIO_IRQ_EDGE_FALL, false);
}

void chriscade_power_init() {
  gpio_init(STATUS_LED_POWER_PIN);
  gpio_set_dir(STATUS_LED_POWER_PIN, GPIO_OUT);
  gpio_put(STATUS_LED_POWER_PIN, 1);

  gpio_init(STATUS_LED_LOW_POWER_PIN);
  gpio_set_dir(STATUS_LED_LOW_POWER_PIN, GPIO_OUT);
  gpio_put(STATUS_LED_LOW_POWER_PIN, 0);

  gpio_init(LOW_POWER_BUTTON_PIN);
  gpio_set_dir(LOW_POWER_BUTTON_PIN, GPIO_IN);
  gpio_pull_up(LOW_POWER_BUTTON_PIN);

  // Startup runs before initJoypad(): configure all six buttons here.
  for (uint pin = 0; pin < 6; ++pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }

  adc_init();
  adc_gpio_init(BATTERY_SENSE_PIN);
  adc_gpio_init(VOLUME_POT_PIN);
  chriscade_battery_update(true);
}

// This is the Doom build's CHRISCADE boot renderer: same palette, font,
// terminal hacking sequence, ACCESS screen, and logo reveal.
// It uses the existing LCD source buffers as temporary 4-bit boot memory.
static const uint8_t boot_palettes[5][16][3] = {
  {
    {0,0,0}, {5,5,18}, {12,9,40}, {35,45,105}, {75,80,150},
    {20,210,245}, {190,235,255}, {0,18,15}, {0,90,50}, {0,245,135},
    {150,255,195}, {45,0,0}, {180,25,8}, {255,80,18}, {255,180,40},
    {255,40,190},
  },
  {
    {0,0,0}, {14,4,16}, {38,8,45}, {94,35,94}, {156,62,116},
    {255,140,40}, {255,228,170}, {22,5,28}, {110,20,75}, {255,55,145},
    {255,180,220}, {40,4,28}, {160,15,65}, {255,65,55}, {255,180,60},
    {200,65,255},
  },
  {
    {0,0,0}, {2,8,20}, {5,20,45}, {18,65,120}, {55,115,175},
    {35,210,255}, {220,250,255}, {0,16,30}, {0,75,115}, {30,190,255},
    {165,240,255}, {12,5,45}, {70,35,155}, {110,90,255}, {190,210,255},
    {105,245,225},
  },
  {
    {0,0,0}, {0,8,2}, {0,24,6}, {0,70,18}, {40,130,35},
    {30,255,70}, {210,255,220}, {0,18,3}, {0,90,22}, {65,245,80},
    {175,255,175}, {8,25,0}, {40,120,12}, {100,220,35}, {190,255,85},
    {135,255,80},
  },
  {
    {0,0,0}, {10,3,24}, {31,10,58}, {68,35,125}, {120,70,175},
    {255,205,65}, {255,245,195}, {18,4,38}, {72,20,105}, {185,75,255},
    {235,190,255}, {38,4,34}, {145,25,90}, {255,80,145}, {255,165,75},
    {255,115,175},
  },
};

#if 0
static const uint8_t boot_font_legacy[96][7] = {
  [' ' - 32] = {0,0,0,0,0,0,0}, ['!' - 32] = {4,4,4,4,4,0,4},
  ['\'' - 32] = {4,4,2,0,0,0,0}, ['%' - 32] = {17,2,4,8,17,0,0},
  ['+' - 32] = {0,4,4,31,4,4,0}, ['-' - 32] = {0,0,0,31,0,0,0},
  ['.' - 32] = {0,0,0,0,0,6,6}, ['/' - 32] = {1,2,4,8,16,0,0},
  ['0' - 32] = {14,17,19,21,25,17,14}, ['1' - 32] = {4,12,4,4,4,4,14},
  ['2' - 32] = {14,17,1,2,4,8,31}, ['3' - 32] = {30,1,1,14,1,1,30},
  ['4' - 32] = {2,6,10,18,31,2,2}, ['5' - 32] = {31,16,16,30,1,1,30},
  ['6' - 32] = {14,16,16,30,17,17,14}, ['7' - 32] = {31,1,2,4,8,8,8},
  ['8' - 32] = {14,17,17,14,17,17,14}, ['9' - 32] = {14,17,17,15,1,1,14},
  [':' - 32] = {0,6,6,0,6,6,0}, ['=' - 32] = {0,31,0,31,0,0,0},
  ['>' - 32] = {16,8,4,2,4,8,16}, ['?' - 32] = {14,17,1,2,4,0,4},
  ['A' - 32] = {14,17,17,31,17,17,17}, ['B' - 32] = {30,17,17,30,17,17,30},
  ['C' - 32] = {14,17,16,16,16,17,14}, ['D' - 32] = {30,17,17,17,17,17,30},
  ['E' - 32] = {31,16,16,30,16,16,31}, ['F' - 32] = {31,16,16,30,16,16,16},
  ['G' - 32] = {14,17,16,23,17,17,15}, ['H' - 32] = {17,17,17,31,17,17,17},
  ['I' - 32] = {14,4,4,4,4,4,14}, ['J' - 32] = {7,2,2,2,2,18,12},
  ['K' - 32] = {17,18,20,24,20,18,17}, ['L' - 32] = {16,16,16,16,16,16,31},
  ['M' - 32] = {17,27,21,21,17,17,17}, ['N' - 32] = {17,25,21,19,17,17,17},
  ['O' - 32] = {14,17,17,17,17,17,14}, ['P' - 32] = {30,17,17,30,16,16,16},
  ['Q' - 32] = {14,17,17,17,21,18,13}, ['R' - 32] = {30,17,17,30,20,18,17},
  ['S' - 32] = {15,16,16,14,1,1,30}, ['T' - 32] = {31,4,4,4,4,4,4},
  ['U' - 32] = {17,17,17,17,17,17,14}, ['V' - 32] = {17,17,17,17,17,10,4},
  ['W' - 32] = {17,17,17,21,21,21,10}, ['X' - 32] = {17,17,10,4,10,17,17},
  ['Y' - 32] = {17,17,10,4,4,4,4}, ['Z' - 32] = {31,1,2,4,8,16,31},
  ['[' - 32] = {14,8,8,8,8,8,14}, [']' - 32] = {14,2,2,2,2,2,14},
  ['_' - 32] = {0,0,0,0,0,0,31},
};
#endif

static const uint8_t boot_font[96][7] = {
  {0,0,0,0,0,0,0}, {4,4,4,4,4,0,4}, {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0}, {17,2,4,8,17,0,0}, {0,0,0,0,0,0,0}, {4,4,2,0,0,0,0},
  {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}, {0,0,0,0,0,0,0}, {0,4,4,31,4,4,0},
  {0,0,0,0,0,0,0}, {0,0,0,31,0,0,0}, {0,0,0,0,0,6,6}, {1,2,4,8,16,0,0},
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30}, {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}, {0,6,6,0,6,6,0}, {0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0}, {0,31,0,31,0,0,0}, {16,8,4,2,4,8,16}, {14,17,1,2,4,0,4},
  {0,0,0,0,0,0,0}, {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
  {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15},
  {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17},
  {16,16,16,16,16,16,31}, {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
  {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30},
  {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10},
  {17,17,10,4,10,17,17}, {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}, {14,8,8,8,8,8,14},
  {0,0,0,0,0,0,0}, {14,2,2,2,2,2,14}, {0,0,0,0,0,0,0}, {0,0,0,0,0,0,31},
};

static uint8_t* boot_framebuffer;
static uint16_t boot_scanline[BOOT_WIDTH];
static uint16_t boot_palette_native[16];
static bool boot_button_latched;
static bool boot_touch_latched;

static bool boot_button_pressed() {
  return BootButtons::pressed(gpio_get_all());
}

static uint16_t boot_rgb(uint8_t r, uint8_t g, uint8_t b) {
  unsigned best = 0;
  uint32_t best_error = UINT32_MAX;
  for (unsigned i = 0; i < 16; ++i) {
    // Match against the original logical palette, then render that logical
    // color through the selected theme. This keeps every scene readable.
    int dr = (int)r - boot_palettes[0][i][0];
    int dg = (int)g - boot_palettes[0][i][1];
    int db = (int)b - boot_palettes[0][i][2];
    uint32_t error = (uint32_t)(dr * dr + dg * dg + db * db);
    if (error < best_error) { best_error = error; best = i; }
  }
  return (uint16_t)best;
}

static void boot_pixel(int x, int y, uint16_t color) {
  if ((unsigned)x >= BOOT_WIDTH || (unsigned)y >= BOOT_HEIGHT) return;
  unsigned index = (unsigned)y * BOOT_WIDTH + (unsigned)x;
  uint8_t* packed = &boot_framebuffer[index >> 1];
  if (index & 1u) *packed = (uint8_t)((*packed & 0x0f) | ((color & 0x0f) << 4));
  else *packed = (uint8_t)((*packed & 0xf0) | (color & 0x0f));
}

static void boot_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > BOOT_WIDTH) w = BOOT_WIDTH - x;
  if (y + h > BOOT_HEIGHT) h = BOOT_HEIGHT - y;
  if (w <= 0 || h <= 0) return;
  uint8_t nibble = (uint8_t)(color & 0x0f);
  uint8_t pair = (uint8_t)(nibble | (nibble << 4));
  for (int yy = y; yy < y + h; ++yy) {
    unsigned start = (unsigned)yy * BOOT_WIDTH + (unsigned)x;
    unsigned end = start + (unsigned)w;
    if (start & 1u) {
      uint8_t* p = &boot_framebuffer[start >> 1];
      *p = (uint8_t)((*p & 0x0f) | (nibble << 4)); ++start;
    }
    unsigned pairs = (end - start) >> 1;
    if (pairs) memset(&boot_framebuffer[start >> 1], pair, pairs);
    start += pairs << 1;
    if (start < end) {
      uint8_t* p = &boot_framebuffer[start >> 1];
      *p = (uint8_t)((*p & 0xf0) | nibble);
    }
  }
}

static void boot_line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  int sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  while (true) {
    boot_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int twice = error * 2;
    if (twice >= dy) { error += dy; x0 += sx; }
    if (twice <= dx) { error += dx; y0 += sy; }
  }
}

static const uint8_t* boot_glyph(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c < 32 || c > 127) c = '?';
  return boot_font[(unsigned)c - 32];
}

static void boot_char(int x, int y, char c, int scale, uint16_t color) {
  const uint8_t* rows = boot_glyph(c);
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 5; ++col) {
      if (rows[row] & (1u << (4 - col)))
        boot_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
    }
  }
}

static int boot_text_width(const char* text, int scale) {
  int count = 0; while (*text++) ++count;
  return count ? count * 6 * scale - scale : 0;
}

static void boot_text(int x, int y, const char* text, int scale, uint16_t color) {
  while (*text) { boot_char(x, y, *text++, scale, color); x += 6 * scale; }
}

static void boot_centered(int y, const char* text, int scale, uint16_t color) {
  boot_text((BOOT_WIDTH - boot_text_width(text, scale)) / 2, y, text, scale, color);
}

static void boot_centered_tight(int y, const char* text, int scale, uint16_t color) {
  int count = 0; for (const char* p = text; *p; ++p) ++count;
  int advance = 5 * scale + 1;
  int x = (BOOT_WIDTH - (count ? count * advance - 1 : 0)) / 2;
  while (*text) { boot_char(x, y, *text++, scale, color); x += advance; }
}

static void boot_text_gradient(int x, int y, const char* text, int scale,
    uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1) {
  while (*text) {
    const uint8_t* rows = boot_glyph(*text++);
    for (int row = 0; row < 7; ++row) {
      uint8_t r = (uint8_t)(r0 + ((int)r1 - r0) * row / 6);
      uint8_t g = (uint8_t)(g0 + ((int)g1 - g0) * row / 6);
      uint8_t b = (uint8_t)(b0 + ((int)b1 - b0) * row / 6);
      uint16_t color = boot_rgb(r, g, b);
      for (int col = 0; col < 5; ++col) {
        if (rows[row] & (1u << (4 - col)))
          boot_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
    x += 6 * scale;
  }
}

static void boot_present(void (*renderer)(int), int tick) {
  renderer(tick);
  tft.startWrite();
  tft.setAddrWindow(0, 0, BOOT_WIDTH, BOOT_HEIGHT);
  for (int y = 0; y < BOOT_HEIGHT; ++y) {
    unsigned offset = (unsigned)y * BOOT_WIDTH / 2;
    for (int x = 0; x < BOOT_WIDTH; x += 2) {
      uint8_t packed = boot_framebuffer[offset++];
      boot_scanline[x] = boot_palette_native[packed & 0x0f];
      boot_scanline[x + 1] = boot_palette_native[packed >> 4];
    }
    tft.pushPixels(boot_scanline, BOOT_WIDTH);
  }
  tft.endWrite();
  if (boot_button_pressed()) boot_button_latched = true;
  chriscade_power_poll(false);
}

static const char boot_log_phrases[30][26] = {
  "> CORE 0 ONLINE",
  "> CORE 1 ONLINE",
  "> XIP CACHE SYNC OK",
  "> SPI1 BUS ARBITRATED",
  "> DMA CHANNELS LOCKED",
  "> SRAM TEST PASSED",
  "> APU MIXER PRIMED",
  "> PWM AUDIO CARRIER OK",
  "> FAT VOLUME MOUNTED",
  "> ROM BANK WINDOW READY",
  "> TOUCH MATRIX ALIGNED",
  "> LCD TEARING CONTAINED",
  "> WATCHDOG STANDING BY",
  "> VOLTAGE RAIL STABLE",
  "> CRC TABLE VERIFIED",
  "> PIO BACKLIGHT ARMED",
  "> INPUT DEBOUNCE READY",
  "> BOOT VECTOR DECRYPTED",
  "> FRAME PACER LOCKED",
  "> SAVE DATA STANDING BY",
  "> SNACK BUFFER EMPTY",
  "> BUGS POLITELY IGNORED",
  "> PIXELS FED AND HAPPY",
  "> NO CARTRIDGE GOBLINS",
  "> DO NOT LICK GPIO",
  "> COFFEE DRIVER MISSING",
  "> FUN LEVELS NOMINAL",
  "> CHEAT CODES REDACTED",
  "> HEAP GREMLINS EVICTED",
  "> PROFESSOR OAK PAGED",
};
static uint8_t boot_log_indices[8];

static void choose_boot_log_phrases() {
  adc_select_input(BATTERY_ADC_INPUT);
  uint32_t state = time_us_32() ^ ((uint32_t)adc_read() << 16) ^
      (battery_mv_filtered << 3);
  if (!state) state = 0x43485249u;
  for (unsigned slot = 0; slot < 8; ++slot) {
    uint8_t candidate;
    bool duplicate;
    do {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      candidate = (uint8_t)(state % 30u);
      duplicate = false;
      for (unsigned prior = 0; prior < slot; ++prior)
        if (boot_log_indices[prior] == candidate) duplicate = true;
    } while (duplicate);
    boot_log_indices[slot] = candidate;
  }
}

static void render_terminal(int tick) {
  for (int y = 0; y < BOOT_HEIGHT; ++y)
    boot_fill_rect(0, y, BOOT_WIDTH, 1, boot_rgb(0, (uint8_t)(8 + y / 18), 10));
  for (int column = 0; column < 32; ++column) {
    int head = (tick * (2 + column % 4) + column * 17) % 215;
    for (int trail = 0; trail < 4; ++trail) {
      char value = "0123456789ABCDEF"[(column * 7 + tick + trail) & 15];
      uint8_t glow = (uint8_t)(75 - trail * 16);
      boot_char(column * 10, head - trail * 9, value, 1,
          boot_rgb(0, glow, (uint8_t)(glow / 2)));
    }
  }
  for (int y = 2; y < BOOT_HEIGHT; y += 4)
    boot_fill_rect(0, y, BOOT_WIDTH, 1, boot_rgb(0, 8, 7));
  boot_fill_rect(7, 5, 306, 22, boot_rgb(0, 30, 24));
  boot_fill_rect(7, 27, 306, 1, boot_rgb(0, 210, 120));
  boot_centered(9, "RP2040 // SECURE BOOT", 2, boot_rgb(90, 255, 175));
  int shown = tick / 4 + 1; if (shown > 8) shown = 8;
  for (int i = 0; i < shown; ++i) {
    uint16_t color = i == shown - 1 ? boot_rgb(170, 255, 205) : boot_rgb(25, 190, 105);
    boot_fill_rect(8, 33 + i * 20, 302, 18, boot_rgb(0, 18, 15));
    boot_text(12, 35 + i * 20,
        boot_log_phrases[boot_log_indices[i]], 2, color);
  }
  int progress = tick * 100 / 30; if (progress > 100) progress = 100;
  boot_centered(198, "DECRYPTING BOOT VECTOR", 2, boot_rgb(60, 230, 145));
  boot_fill_rect(11, 218, 298, 11, boot_rgb(0, 35, 25));
  boot_fill_rect(13, 220, progress * 294 / 100, 7, boot_rgb(0, 245, 135));
}

static void render_access(int tick) {
  uint16_t bg = tick & 1 ? boot_rgb(0, 12, 9) : boot_rgb(0, 70, 38);
  boot_fill_rect(0, 0, BOOT_WIDTH, BOOT_HEIGHT, bg);
  boot_centered(80, "ACCESS", 5, boot_rgb(150, 255, 195));
  boot_centered(124, "GRANTED", 4, boot_rgb(0, 255, 135));
  boot_centered(184, "WELCOME CHRISTIAN", 2, boot_rgb(100, 255, 180));
}

static void render_logo(int tick) {
  for (int y = 0; y < BOOT_HEIGHT; ++y)
    boot_fill_rect(0, y, BOOT_WIDTH, 1, boot_rgb((uint8_t)(8 + y / 22),
        (uint8_t)(7 + y / 34), (uint8_t)(25 + y / 5)));
  for (int i = 0; i < 38; ++i) {
    int x = (i * 83 + tick * (1 + i % 3)) % BOOT_WIDTH;
    int y = 28 + (i * 47) % 125;
    uint16_t c = (i + tick) % 5 ? boot_rgb(75, 80, 150) : boot_rgb(130, 250, 255);
    boot_pixel(x, y, c); if (!(i % 7)) boot_pixel(x + 1, y, c);
  }
  uint16_t grid = boot_rgb(35, 45, 105);
  for (int y = 158; y < BOOT_HEIGHT; y += 12) boot_line(0, y, BOOT_WIDTH - 1, y, grid);
  for (int x = -160; x <= 480; x += 32) boot_line(160, 154, x, 239, grid);
  boot_fill_rect(0, 0, BOOT_WIDTH, 22, boot_rgb(12, 9, 40));
  boot_fill_rect(0, 21, BOOT_WIDTH, 2, boot_rgb(20, 210, 245));
  boot_centered_tight(4, "CHRISTIAN'S PORTABLE ARCADE", 2, boot_rgb(165, 235, 255));
  int logo_x = (BOOT_WIDTH - boot_text_width("CHRISCADE", 5)) / 2;
  int logo_y = 75 + ((tick / 5) & 1);
  boot_text(logo_x + 5, logo_y + 7, "CHRISCADE", 5, boot_rgb(80, 8, 95));
  boot_text(logo_x - 2, logo_y, "CHRISCADE", 5, boot_rgb(5, 8, 24));
  boot_text(logo_x + 2, logo_y, "CHRISCADE", 5, boot_rgb(5, 8, 24));
  boot_text(logo_x, logo_y - 2, "CHRISCADE", 5, boot_rgb(5, 8, 24));
  boot_text(logo_x, logo_y + 2, "CHRISCADE", 5, boot_rgb(5, 8, 24));
  boot_text_gradient(logo_x, logo_y, "CHRISCADE", 5, 85, 250, 255, 255, 55, 195);
  boot_fill_rect(logo_x, 116, boot_text_width("CHRISCADE", 5), 3, boot_rgb(30, 240, 255));
  boot_fill_rect(logo_x + 18, 121, boot_text_width("CHRISCADE", 5) - 36, 2, boot_rgb(255, 40, 190));
  boot_centered(137, "RP2040 SYSTEM", 2, boot_rgb(175, 190, 255));
  int spark_x = 22 + (tick * 7) % 276;
  boot_fill_rect(spark_x, 164, 12, 2, boot_rgb(150, 250, 255));
  boot_pixel(spark_x + 15, 164, boot_rgb(255, 255, 255));
  for (int y = 28; y < BOOT_HEIGHT; y += 5) boot_fill_rect(0, y, BOOT_WIDTH, 1, boot_rgb(5, 5, 18));
}

static constexpr char BOOT_PROMPT_TEXT[] = "ANY BUTTON TO START";
static constexpr int BOOT_PROMPT_SCALE = 2;
// Match boot_text_width's six-column glyph advance. Keep drawing and the
// blink transfer on the same bounds so longer labels never leave edge pixels.
static constexpr int BOOT_PROMPT_TEXT_WIDTH =
    (sizeof(BOOT_PROMPT_TEXT) - 1) * 6 * BOOT_PROMPT_SCALE - BOOT_PROMPT_SCALE;
static constexpr int BOOT_PROMPT_WIDTH = BOOT_PROMPT_TEXT_WIDTH + 20;
static constexpr int BOOT_PROMPT_X = (BOOT_WIDTH - BOOT_PROMPT_WIDTH) / 2;
static constexpr int BOOT_PROMPT_Y = 187;
static constexpr int BOOT_PROMPT_HEIGHT = 36;
static_assert(BOOT_PROMPT_WIDTH <= BOOT_WIDTH, "Boot prompt must fit the screen");

static void render_logo_prompt(int tick) {
  render_logo(tick);
  if ((tick % 16) < 11) {
    boot_fill_rect(BOOT_PROMPT_X, BOOT_PROMPT_Y, BOOT_PROMPT_WIDTH, BOOT_PROMPT_HEIGHT,
        boot_rgb(13, 12, 48));
    boot_line(BOOT_PROMPT_X, BOOT_PROMPT_Y, BOOT_PROMPT_X + BOOT_PROMPT_WIDTH - 1,
        BOOT_PROMPT_Y, boot_rgb(70, 225, 255));
    boot_line(BOOT_PROMPT_X, BOOT_PROMPT_Y + BOOT_PROMPT_HEIGHT - 1,
        BOOT_PROMPT_X + BOOT_PROMPT_WIDTH - 1, BOOT_PROMPT_Y + BOOT_PROMPT_HEIGHT - 1,
        boot_rgb(235, 50, 200));
    boot_centered(198, BOOT_PROMPT_TEXT, BOOT_PROMPT_SCALE, boot_rgb(230, 245, 255));
  }
}

static void boot_push_region(int x, int y, int width, int height) {
  if (x < 0) { width += x; x = 0; }
  if (y < 0) { height += y; y = 0; }
  if (x + width > BOOT_WIDTH) width = BOOT_WIDTH - x;
  if (y + height > BOOT_HEIGHT) height = BOOT_HEIGHT - y;
  if (width <= 0 || height <= 0) return;

  tft.setAddrWindow(x, y, width, height);
  for (int yy = y; yy < y + height; ++yy) {
    unsigned pixel = (unsigned)yy * BOOT_WIDTH + (unsigned)x;
    for (int xx = 0; xx < width; ++xx, ++pixel) {
      uint8_t packed = boot_framebuffer[pixel >> 1];
      uint8_t color = (pixel & 1u) ? (packed >> 4) : (packed & 0x0f);
      boot_scanline[xx] = boot_palette_native[color];
    }
    tft.pushPixels(boot_scanline, width);
  }
}

// Render the exact original moving logo scene into its framebuffer, then send
// only the old/new star pixels and the few regions that visibly changed. This
// retains the starfield, logo bob, moving spark and blinking prompt while
// avoiding a full 320x240 SPI transfer for every animation tick.
static void animate_logo_original(int tick) {
  const int previous = tick - 1;
  render_logo_prompt(tick);

  tft.startWrite();
  for (int i = 0; i < 38; ++i) {
    int old_x = (i * 83 + previous * (1 + i % 3)) % BOOT_WIDTH;
    int new_x = (i * 83 + tick * (1 + i % 3)) % BOOT_WIDTH;
    int y = 28 + (i * 47) % 125;
    boot_push_region(old_x, y, 2, 2);
    boot_push_region(new_x, y, 2, 2);
  }

  if (((previous / 5) & 1) != ((tick / 5) & 1))
    boot_push_region(20, 70, 280, 57);

  int old_spark = 22 + (previous * 7) % 276;
  int new_spark = 22 + (tick * 7) % 276;
  boot_push_region(old_spark, 163, 18, 4);
  boot_push_region(new_spark, 163, 18, 4);

  if (((previous % 16) < 11) != ((tick % 16) < 11))
    boot_push_region(BOOT_PROMPT_X, BOOT_PROMPT_Y, BOOT_PROMPT_WIDTH, BOOT_PROMPT_HEIGHT);
  tft.endWrite();
}

static void render_reveal(int step) {
  render_logo(step);
  for (int band = 0; band < 30; ++band)
    if (((band * 11 + 3) % 15) > step)
      boot_fill_rect(0, band * 8, BOOT_WIDTH, 8, boot_rgb(2, 3, 12));
  if (step < 12) {
    int y = (step * 47 + 19) % 220;
    boot_fill_rect(25 + (step * 31) % 95, y, 120 + (step * 17) % 120, 3,
        step & 1 ? boot_rgb(20, 245, 255) : boot_rgb(255, 30, 190));
  }
}

static uint boot_speaker_slice;
static uint boot_speaker_channel;
static uint32_t boot_carrier_hz;
static uint16_t boot_volume_filtered;
static bool boot_volume_initialized;
static volatile uint32_t boot_tone_phase;
static volatile uint32_t boot_tone_phase_step;
static volatile uint16_t boot_tone_amplitude;
static volatile bool boot_tone_active;

static uint16_t boot_read_volume() {
  adc_select_input(VOLUME_POT_ADC_INPUT);
  uint16_t raw = 4095u - adc_read();
  if (!boot_volume_initialized) {
    boot_volume_filtered = raw;
    boot_volume_initialized = true;
  } else {
    boot_volume_filtered =
        (uint16_t)((boot_volume_filtered * 3u + raw + 2u) >> 2);
  }
  if (boot_volume_filtered < 24u) return 0;
  if (boot_volume_filtered > 4070u) return 4095u;
  return boot_volume_filtered;
}

static void boot_tone_irq() {
  uint irq_mask = 1u << boot_speaker_slice;
  if ((pwm_get_irq_status_mask() & irq_mask) == 0) return;
  pwm_clear_irq(boot_speaker_slice);
  if (!boot_tone_active) return;

  boot_tone_phase += boot_tone_phase_step;
  const uint16_t center = (BOOT_CARRIER_WRAP + 1u) / 2u;
  int level = (boot_tone_phase & 0x80000000u)
      ? (int)center + boot_tone_amplitude
      : (int)center - boot_tone_amplitude;
  pwm_set_chan_level(boot_speaker_slice, boot_speaker_channel,
      (uint16_t)level);
}

static void boot_tone_init() {
  gpio_set_function(SPEAKER_PIN, GPIO_FUNC_PWM);
  gpio_set_drive_strength(SPEAKER_PIN, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(SPEAKER_PIN, GPIO_SLEW_RATE_FAST);
  boot_speaker_slice = pwm_gpio_to_slice_num(SPEAKER_PIN);
  boot_speaker_channel = pwm_gpio_to_channel(SPEAKER_PIN);
  pwm_set_clkdiv_int_frac4(boot_speaker_slice, 1, 0);
  pwm_set_wrap(boot_speaker_slice, BOOT_CARRIER_WRAP);
  boot_carrier_hz = clock_get_hz(clk_sys) / (BOOT_CARRIER_WRAP + 1u);
  if (!boot_carrier_hz) boot_carrier_hz = 125000u;
  pwm_clear_irq(boot_speaker_slice);
  pwm_set_irq_enabled(boot_speaker_slice, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, boot_tone_irq);
  irq_set_enabled(PWM_IRQ_WRAP, true);
  pwm_set_enabled(boot_speaker_slice, false);
}

static void boot_tone(uint frequency) {
  if (!frequency) {
    boot_tone_active = false;
    pwm_set_enabled(boot_speaker_slice, false);
    pwm_set_chan_level(boot_speaker_slice, boot_speaker_channel, 0);
    return;
  }

  boot_tone_phase = 0;
  boot_tone_phase_step =
      (uint32_t)(((uint64_t)frequency << 32) / boot_carrier_hz);
  uint16_t volume = boot_read_volume();
  if (volume <= BOOT_VOLUME_MUTE_THRESHOLD) {
    boot_tone_amplitude = 0;
    pwm_set_enabled(boot_speaker_slice, false);
    return;
  }

  if (volume >= BOOT_VOLUME_FULL_THRESHOLD) {
    boot_tone_amplitude = (BOOT_CARRIER_WRAP + 1u) / 2u;
  } else {
    uint32_t normalized =
        ((uint32_t)(volume - BOOT_VOLUME_MUTE_THRESHOLD) *
            BOOT_VOLUME_FULL_THRESHOLD) /
        (BOOT_VOLUME_FULL_THRESHOLD - BOOT_VOLUME_MUTE_THRESHOLD);
    uint32_t tapered =
        (normalized * normalized) / BOOT_VOLUME_FULL_THRESHOLD;
    volume = (uint16_t)((tapered * normalized) /
        BOOT_VOLUME_FULL_THRESHOLD);
    boot_tone_amplitude = (uint16_t)
        (((uint32_t)((BOOT_CARRIER_WRAP + 1u) / 2u) * volume) /
            BOOT_VOLUME_FULL_THRESHOLD);
  }
  boot_tone_active = true;
  pwm_set_enabled(boot_speaker_slice, true);
}

static void boot_loading_tone(uint frequency) {
  boot_tone(frequency);
  // Loading feedback should sit underneath the interface, not compete with
  // the CHRISCADE startup jingle or game audio.
  boot_tone_amplitude = (uint16_t)((boot_tone_amplitude * 3u) / 10u);
}

static void boot_tone_shutdown() {
  boot_tone(0);
  pwm_set_irq_enabled(boot_speaker_slice, false);
  pwm_clear_irq(boot_speaker_slice);
  irq_set_enabled(PWM_IRQ_WRAP, false);
  gpio_set_function(SPEAKER_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(SPEAKER_PIN, GPIO_OUT);
  gpio_put(SPEAKER_PIN, 0);
}

void chriscade_loading_sound_begin() {
  boot_tone_init();
}

void chriscade_loading_sound_set(unsigned int frequency) {
  boot_loading_tone(frequency);
}

void chriscade_loading_sound_end() {
  boot_tone_shutdown();
}

void chriscade_alarm_sound_begin() {
  boot_tone_init();
}

void chriscade_alarm_sound_set(unsigned int frequency) {
  // Unlike loading/interface feedback, alarms use the full volume selected by
  // the physical potentiometer so they remain noticeable away from the unit.
  boot_tone(frequency);
}

void chriscade_alarm_sound_end() {
  boot_tone_shutdown();
}

void chriscade_ui_click(unsigned int frequency) {
  chriscade_loading_sound_begin();
  chriscade_loading_sound_set(frequency);
  delay(20);
  chriscade_loading_sound_end();
}

static void play_boot_notes(const uint16_t* notes, const uint16_t* durations,
    unsigned count, int* animation_tick) {
  for (unsigned i = 0; i < count; ++i) {
    boot_tone(notes[i]);
    uint16_t remaining = durations[i];
    while (remaining) {
      if (animation_tick) animate_logo_original((*animation_tick)++);
      if (animation_tick && chriscade_main_menu_touch_enabled()) {
        uint16_t x, y;
        if (chriscade_touch_read(&x, &y)) boot_touch_latched = true;
      }
      if (boot_button_pressed()) boot_button_latched = true;
      chriscade_power_poll(false);
      uint16_t slice = remaining > 40 ? 40 : remaining;
      delay(slice);
      remaining -= slice;
    }
  }
  boot_tone(0);
}

static void boot_jingle(unsigned sound, int* animation_tick) {
  static const uint16_t arcade_notes[] =
      {523,659,784,1047,0,784,988,1175,1319,1047,1319};
  static const uint16_t arcade_durations[] =
      {110,110,120,210,55,90,90,90,260,100,380};
  static const uint16_t synth_notes[] =
      {220,330,440,660,880,660,990,1320};
  static const uint16_t synth_durations[] =
      {120,120,120,150,220,90,110,340};
  static const uint16_t cyber_notes[] =
      {1568,0,1175,0,1760,880,0,1319,2093};
  static const uint16_t cyber_durations[] =
      {55,25,55,25,70,80,35,100,280};
  static const uint16_t cosmic_notes[] =
      {196,294,392,587,784,1175,1568,1175};
  static const uint16_t cosmic_durations[] =
      {150,130,130,150,180,130,330,180};
  static const uint16_t pixel_notes[] =
      {988,1319,1568,2093,0,1568,2093,2637};
  static const uint16_t pixel_durations[] =
      {60,60,60,100,35,70,80,260};
  static const uint16_t victory_notes[] =
      {523,659,784,1047,1319,1568,0,1319,1568,2093};
  static const uint16_t victory_durations[] =
      {75,75,75,110,90,180,45,80,100,360};
  if (sound == (unsigned)ChriscadeBootSound::ARCADE)
    play_boot_notes(arcade_notes, arcade_durations,
        sizeof(arcade_notes) / sizeof(arcade_notes[0]), animation_tick);
  else if (sound == (unsigned)ChriscadeBootSound::SYNTHWAVE)
    play_boot_notes(synth_notes, synth_durations,
        sizeof(synth_notes) / sizeof(synth_notes[0]), animation_tick);
  else if (sound == (unsigned)ChriscadeBootSound::CYBER)
    play_boot_notes(cyber_notes, cyber_durations,
        sizeof(cyber_notes) / sizeof(cyber_notes[0]), animation_tick);
  else if (sound == (unsigned)ChriscadeBootSound::COSMIC)
    play_boot_notes(cosmic_notes, cosmic_durations,
        sizeof(cosmic_notes) / sizeof(cosmic_notes[0]), animation_tick);
  else if (sound == (unsigned)ChriscadeBootSound::PIXEL)
    play_boot_notes(pixel_notes, pixel_durations,
        sizeof(pixel_notes) / sizeof(pixel_notes[0]), animation_tick);
  else if (sound == (unsigned)ChriscadeBootSound::VICTORY)
    play_boot_notes(victory_notes, victory_durations,
        sizeof(victory_notes) / sizeof(victory_notes[0]), animation_tick);
}

void chriscade_preview_boot_sound(unsigned int sound) {
  if (sound == (unsigned)ChriscadeBootSound::OFF) return;
  boot_tone_init();
  boot_jingle(sound, nullptr);
  boot_tone_shutdown();
}

void chriscade_boot_screen() {
  // Load/calibrate before drawing the logo; calibration must not erase it.
  if (chriscade_main_menu_touch_enabled()) chriscade_touch_init();
  boot_tone_init();
  uint32_t work_size = 0;
  boot_framebuffer = lcd_boot_work_area(&work_size);
  hard_assert(work_size >= (BOOT_WIDTH * BOOT_HEIGHT / 2));
  const unsigned theme = (unsigned)chriscade_boot_theme();
  for (unsigned i = 0; i < 16; ++i) {
    boot_palette_native[i] = (uint16_t)(((boot_palettes[theme][i][0] & 0xf8) << 8) |
        ((boot_palettes[theme][i][1] & 0xfc) << 3) |
        (boot_palettes[theme][i][2] >> 3));
  }
  // TFT_eSPI's RP2040 PIO transport requires native RGB565 pixels to be
  // marked swapped; otherwise red/blue channel bytes are reversed.
  tft.setSwapBytes(true);
  boot_button_latched = false;
  choose_boot_log_phrases();
  if (!chriscade_quick_boot_enabled()) {
    for (int tick = 0; tick <= 30; ++tick) {
      if (chriscade_boot_sound() != ChriscadeBootSound::OFF && (tick & 3) == 0)
        boot_loading_tone(150u + (unsigned)(tick / 4) * 22u);
      boot_present(render_terminal, tick);
      delay(15);
    }
    boot_tone(0);
    for (int flash = 0; flash < 4; ++flash) {
      boot_present(render_access, flash);
      delay(45);
    }
    for (int step = 0; step < 15; ++step) {
      boot_present(render_reveal, step);
      delay(20);
    }
  }
  // Render the finished logo once. Rebuilding and transmitting a complete
  // framebuffer for every jingle note and prompt blink made this screen look
  // jerky on the 27 MHz SPI display.
  boot_present(render_logo_prompt, 18);
  int animation_tick = 19;
  boot_touch_latched = false;
  boot_jingle((unsigned)chriscade_boot_sound(), &animation_tick);

  uint16_t touch_x, touch_y;
  const bool touch_enabled = chriscade_main_menu_touch_enabled();
  uint32_t next_animation_ms = millis();
  while (!(boot_button_latched || boot_button_pressed())) {
    if (touch_enabled && (boot_touch_latched || chriscade_touch_read(&touch_x, &touch_y))) break;
    uint32_t now = millis();
    if ((int32_t)(now - next_animation_ms) >= 0) {
      animate_logo_original(animation_tick++);
      next_animation_ms = now + 40;
    }
    chriscade_power_poll(false);
    delay(5);
  }
  if (chriscade_boot_sound() != ChriscadeBootSound::OFF) {
    boot_tone(1047); delay(55);
    boot_tone(1568); delay(90);
    boot_tone(0);
  }
  // Consume the opening gesture, so it cannot also choose a launcher card.
  while (boot_button_pressed() ||
      (touch_enabled && chriscade_touch_read(&touch_x, &touch_y))) {
    chriscade_power_poll(false);
    delay(10);
  }
  boot_button_latched = false;
  boot_tone_shutdown();
}

static void low_power_lcd_command(uint8_t command) {
  gpio_put(LCD_CS_PIN, 0);
  gpio_put(LCD_DC_PIN, 0);
  spi_write_blocking(spi1, &command, 1);
  gpio_put(LCD_CS_PIN, 1);
}

static void enter_low_power(bool save_game) {
  if (gpio_get(LOW_POWER_BUTTON_PIN)) return;
  delay(25);
  if (gpio_get(LOW_POWER_BUTTON_PIN)) return;

  multicore_reset_core1();
  delay(5);
#if ENABLE_SOUND && defined(CROWPANEL_PWM_AUDIO)
  crowpanel_audio_shutdown();
#endif
#if ENABLE_SDCARD
  if (save_game) write_cart_ram_file(&gb);
#endif

  gpio_put(STATUS_LED_POWER_PIN, 0);
  gpio_put(STATUS_LED_LOW_POWER_PIN, 0);
  chriscade_brightness_shutdown();
  gpio_set_function(SPEAKER_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(SPEAKER_PIN, GPIO_OUT);
  gpio_put(SPEAKER_PIN, 0);

  gpio_init(CP_TOUCH_CS_PIN); gpio_set_dir(CP_TOUCH_CS_PIN, GPIO_OUT); gpio_put(CP_TOUCH_CS_PIN, 1);
  gpio_init(CP_SD_CS_PIN); gpio_set_dir(CP_SD_CS_PIN, GPIO_OUT); gpio_put(CP_SD_CS_PIN, 1);
  gpio_init(LCD_CS_PIN); gpio_set_dir(LCD_CS_PIN, GPIO_OUT); gpio_put(LCD_CS_PIN, 1);
  gpio_init(LCD_DC_PIN); gpio_set_dir(LCD_DC_PIN, GPIO_OUT); gpio_put(LCD_DC_PIN, 0);
  gpio_init(LCD_RESET_PIN); gpio_set_dir(LCD_RESET_PIN, GPIO_OUT); gpio_put(LCD_RESET_PIN, 1);

  spi_init(spi1, 20000000);
  spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);
  low_power_lcd_command(0x28);
  low_power_lcd_command(0x10);
  delay(120);
  spi_deinit(spi1);

  gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SIO);
  gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SIO);
  gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SIO);
  gpio_set_dir(LCD_SCK_PIN, GPIO_OUT); gpio_set_dir(LCD_MOSI_PIN, GPIO_OUT); gpio_set_dir(LCD_MISO_PIN, GPIO_IN);
  gpio_put(LCD_SCK_PIN, 0); gpio_put(LCD_MOSI_PIN, 0);

  uint32_t keep_configured_mask =
      (1u << LOW_POWER_BUTTON_PIN) | (1u << STATUS_LED_POWER_PIN) |
      (1u << STATUS_LED_LOW_POWER_PIN) | (1u << LCD_BL_PIN) |
      (1u << LCD_CS_PIN) | (1u << LCD_DC_PIN) | (1u << LCD_RESET_PIN) |
      (1u << CP_TOUCH_CS_PIN) | (1u << CP_SD_CS_PIN);
  low_power_set_pins_low_leakage(keep_configured_mask);

  // The initial press is held until this point. Wait for release, then arm a
  // falling-edge wake so the next press—not the prior release—reboots cleanly.
  while (!gpio_get(LOW_POWER_BUTTON_PIN)) tight_loop_contents();
  gpio_acknowledge_irq(LOW_POWER_BUTTON_PIN, GPIO_IRQ_EDGE_FALL);
  low_power_dormant_until_gpio_falling(LOW_POWER_BUTTON_PIN);
  watchdog_reboot(0, 0, 1);
  while (true) tight_loop_contents();
}

void chriscade_power_poll(bool save_game) {
  chriscade_battery_update(false);
  if (!gpio_get(LOW_POWER_BUTTON_PIN)) enter_low_power(save_game);
}
