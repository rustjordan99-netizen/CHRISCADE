/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <TFT_eSPI.h>
#include <pico/platform.h>

#include "common.h"
#include "chriscade_settings.h"
#include "input.h"
#include "game_picture.h"

TFT_eSPI tft = TFT_eSPI();
#if ENABLE_LCD_FRAMEBUFFER
#if ENABLE_FRAMEBUFFER_FLIP_X_Y
#define FRAMEBUFFER_WIDTH DISPLAY_HEIGHT
#define FRAMEBUFFER_HEIGHT DISPLAY_WIDTH
#else
#define FRAMEBUFFER_WIDTH DISPLAY_WIDTH
#define FRAMEBUFFER_HEIGHT DISPLAY_HEIGHT
#endif
#define FRAMEBUFFER_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)
#if ENABLE_LCD_DMA && ENABLE_DOUBLE_BUFFERING
#define BUFFER_COUNT 2
#else // !ENABLE_LCD_DMA
#define BUFFER_COUNT 1 // no use in double buffering without DMA
#endif
static uint16_t framebuffers[BUFFER_COUNT][FRAMEBUFFER_PIXELS];
static int8_t activeFramebufferId = 0;
#else // !ENABLE_LCD_FRAMEBUFFER
// Note DMA mode does not have a measurable effect without a framebuffer
// Put the DMA source in scratch X (SRAM4) instead of striped main SRAM. Core 0
// can then execute the emulator from main SRAM while display DMA reads this
// independent bank, avoiding the movement-only bus contention seen when every
// scaled scanline changes. The lower 2 KiB is free; core 1's stack occupies
// the upper 2 KiB, and this 640-byte buffer stays safely below it.
static uint16_t __scratch_x("lcd_dma") linebuffer[DISPLAY_WIDTH];
#endif

static uint8_t scaledLineOffsetTable[LCD_HEIGHT]; // scaled to 240 lines
static uint8_t scaledColSourceTable[DISPLAY_WIDTH];
static uint16_t scaledColCount = 0;

/* Two source-sized buffers decouple the 60 Hz emulator from the slower scaled
 * SPI display. Core 0 always writes one stable frame while core 1 displays the
 * other, preventing the alternating-line tearing seen without a framebuffer. */
static uint8_t gbFramebuffers[2][LCD_WIDTH * LCD_HEIGHT];
static uint8_t producerFramebuffer = 0;
static int lcd_frame_busy = 0;
static int lcd_frame_pending = 0;
static uint8_t pendingFramebuffer = 0;
static uint32_t lcd_line_hashes[LCD_HEIGHT];
static bool lcd_line_hashes_valid = false;
static uint32_t lcd_frame_palette_hash = 0;
static uint32_t screenshot_state; // 0=idle, 1=requested, 2=SPI yielded
static uint8_t displayedFramebuffer;
static bool displayedFrameValid;

bool lcd_begin_screenshot(GamePicture& picture, uint16_t*& row) {
#if !ENABLE_LCD_FRAMEBUFFER
  __atomic_store_n(&screenshot_state, 1u, __ATOMIC_RELEASE);
  union core_cmd cmd = {};
  cmd.cmd = CORE_CMD_SCREENSHOT_PAUSE;
  if (!multicore_fifo_push_timeout_us(cmd.full, 250000)) {
    __atomic_store_n(&screenshot_state, 0u, __ATOMIC_RELEASE);
    return false;
  }
  const uint64_t deadline = time_us_64() + 250000;
  while (__atomic_load_n(&screenshot_state, __ATOMIC_ACQUIRE) != 2u) {
    if (time_us_64() >= deadline) {
      __atomic_store_n(&screenshot_state, 0u, __ATOMIC_RELEASE);
      return false;
    }
    tight_loop_contents();
  }
  if (!displayedFrameValid) { lcd_end_screenshot(); return false; }
  picture = {gbFramebuffers[displayedFramebuffer],
      gb.cgb.cgbMode ? gb.cgb.fixPalette : &palette[0][0],
      gb.cgb.cgbMode != 0, (unsigned)scalingMode};
  // DMA is drained and core 1 is parked, so its scratch scanline is available.
  row = linebuffer;
  return true;
#else
  (void)picture; (void)row;
  return false;
#endif
}

void lcd_end_screenshot() {
  __atomic_store_n(&screenshot_state, 0u, __ATOMIC_RELEASE);
}

bool lcd_frame_available() {
  return !__atomic_load_n(&lcd_frame_busy, __ATOMIC_SEQ_CST);
}

void lcd_restart_after_core_reset() {
  // A forced core-1 stop may interrupt an in-flight display command before it
  // clears busy. Drop that stale command before relaunching the LCD owner.
  __atomic_store_n(&lcd_frame_busy, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&lcd_frame_pending, 0, __ATOMIC_SEQ_CST);
  lcd_line_hashes_valid = false;
}

// The boot animation runs before core 1 starts. Reuse the two source-sized
// display buffers as temporary 4-bit boot graphics memory, then return them
// to the emulator unchanged when the ROM list/game starts.
uint8_t* lcd_boot_work_area(uint32_t* size) {
  if (size) *size = sizeof(gbFramebuffers);
  return &gbFramebuffers[0][0];
}

volatile ScalingMode scalingMode = ScalingMode::STRETCH_KEEP_ASPECT;

#define IS_REPEATED(pos) ((pos % 2) || (pos % 6 == 0))

static void calcExtraLineTable() {
  uint8_t offset = 0;
  for (uint8_t line = 0; line < LCD_HEIGHT; ++line) {
    scaledLineOffsetTable[line] = offset;
    offset += 1 + IS_REPEATED(line);
  }

  scaledColCount = 0;
  for (uint16_t col = 0; col < LCD_WIDTH; ++col) {
    scaledColSourceTable[scaledColCount++] = (uint8_t)col;
    if (IS_REPEATED(col)) {
      scaledColSourceTable[scaledColCount++] = (uint8_t)col;
    }
  }
}

void lcd_init(bool isCore1) {
  tft.init();

  if (isCore1) chriscade_brightness_init();

#if ENABLE_LCD_DMA
  // do not enable DMA on core0 as it fails on core1 if it is already enabled
  if (isCore1 && !tft.initDMA(/*ctrl_cs not supported in RP2040 implementation*/)) {
    error("Failed to initialize TFT DMA");
  }
#endif

  bool rotate = true;
#if ENABLE_FRAMEBUFFER_FLIP_X_Y
  // the rotation only flips x and y in GRAM but does not change the LCD screen refresh direction.
  // If the display is a native portrait (and not landscape) lcd, rotating the lcd to landscape results in ugly
  // diagonal update lines. Keeping it in portrait mode and Flipping x and y in the framebuffer results in nicer
  // horizontal update lines. Even better would be VSync or TE line handling (which is not present in most cheap displays)
  rotate = !isCore1; // rotate in start menu but not in-game 
#endif
  tft.setRotation(rotate ? 1 : 0);

  tft.fillScreen(TFT_BLACK);
}

void lcd_draw_line(struct gb_s* gb, const uint8_t pixels[LCD_WIDTH], const uint_fast8_t line) {
  (void)gb;
  memcpy(&gbFramebuffers[producerFramebuffer][line * LCD_WIDTH],
      pixels, LCD_WIDTH);

  if (line == LCD_HEIGHT - 1 &&
      !__atomic_load_n(&lcd_frame_busy, __ATOMIC_SEQ_CST) &&
      !__atomic_load_n(&lcd_frame_pending, __ATOMIC_SEQ_CST)) {
    pendingFramebuffer = producerFramebuffer;
    producerFramebuffer ^= 1;
    __atomic_store_n(&lcd_frame_pending, 1, __ATOMIC_SEQ_CST);
  }
}

void lcd_present_pending_frame() {
  if (!__atomic_load_n(&lcd_frame_pending, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&lcd_frame_busy, __ATOMIC_SEQ_CST)) {
    return;
  }

  union core_cmd cmd = {};
  cmd.cmd = CORE_CMD_LCD_FRAME;
  cmd.data = pendingFramebuffer;
  __atomic_store_n(&lcd_frame_busy, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&lcd_frame_pending, 0, __ATOMIC_SEQ_CST);
  multicore_fifo_push_blocking(cmd.full);
}

#if ENABLE_LCD_FRAMEBUFFER
void lcd_pushLine(uint16_t screenColOffset, uint16_t screenLineOffset, uint16_t line, const uint16_t* pixels, uint_fast16_t width) {
  uint16_t* framebuffer = framebuffers[activeFramebufferId];
#if ENABLE_FRAMEBUFFER_FLIP_X_Y
  uint_fast16_t pos = (screenColOffset * DISPLAY_HEIGHT) + DISPLAY_HEIGHT - (screenLineOffset + line) - 1;
  for (uint_fast16_t i = 0; i < width; ++i) {
    framebuffer[pos] = pixels[i];
    pos += DISPLAY_HEIGHT;
  }
#else
  uint32_t offset = screenColOffset + (uint32_t)(screenLineOffset + line) * DISPLAY_WIDTH;
  memcpy(&framebuffer[offset], pixels, width * sizeof(uint16_t));
#endif
}
#else // !ENABLE_LCD_FRAMEBUFFER
void lcd_pushLine(uint16_t screenColOffset, uint16_t screenLineOffset, uint16_t line, const uint16_t* pixels, uint_fast16_t width) {
  tft.setAddrWindow(screenColOffset, screenLineOffset + line, width, 1);
#if ENABLE_LCD_DMA
  tft.dmaWait();
  memcpy(linebuffer, pixels, width * sizeof(uint16_t));
  tft.setSwapBytes(true);
  tft.startWrite(); // manual start required as DMA transfer is asynchronous
  tft.pushPixelsDMA((uint16_t*) linebuffer, width);
  //tft.endWrite(); // do not call endWrite(), as it will wait for the DMA transfer to finish, which results in no performance gain
#else
  tft.pushColors((uint16_t*) pixels, width, true);
#endif
}
#endif

void lcd_write_pixels_normal(const uint16_t* pixels, uint8_t line, uint_fast16_t count) {
  const uint16_t colOffset = (DISPLAY_WIDTH - LCD_WIDTH) / 2;
  const uint16_t screenLineOffset = (DISPLAY_HEIGHT - LCD_HEIGHT) / 2;
  lcd_pushLine(colOffset, screenLineOffset, line, pixels, count);
}

void lcd_write_pixels_stretched(const uint16_t* pixels, uint8_t line, uint_fast16_t count) {
  static uint16_t doubledPixels[DISPLAY_WIDTH];
  uint16_t pos = 0;
  for (int col = 0; col < count; ++col) {
    doubledPixels[pos++] = pixels[col];
    doubledPixels[pos++] = pixels[col];
  }
  const uint16_t stretchedWidth = pos;

  const uint8_t lineRepeated = IS_REPEATED(line);
  const uint16_t lineOffset = scaledLineOffsetTable[line];
  lcd_pushLine(0, 0, lineOffset, doubledPixels, stretchedWidth);
  if (lineRepeated) {
    lcd_pushLine(0, 0, lineOffset + 1, doubledPixels, stretchedWidth);
  }
}

void lcd_write_pixels_stretched_keep_aspect(const uint16_t* pixels, uint8_t line, uint_fast16_t count) {
  static uint16_t doubledPixels[DISPLAY_WIDTH];
  (void)count;
  for (uint16_t pos = 0; pos < scaledColCount; ++pos) {
    doubledPixels[pos] = pixels[scaledColSourceTable[pos]];
  }
  const uint16_t stretchedWidth = scaledColCount;

  const uint16_t colOffset = (DISPLAY_WIDTH - stretchedWidth) / 2;
 
  const uint8_t lineRepeated = IS_REPEATED(line);
  const uint16_t lineOffset = scaledLineOffsetTable[line];
  lcd_pushLine(colOffset, 0, lineOffset, doubledPixels, stretchedWidth);
  if (lineRepeated) {
    lcd_pushLine(colOffset, 0, lineOffset + 1, doubledPixels, stretchedWidth);
  }
}

// Writes pixels to screen or framebuffer
void lcd_write_pixels(const uint16_t* pixels, uint8_t line, uint_fast16_t count) {
  switch (scalingMode)
  {
  case ScalingMode::STRETCH:
    lcd_write_pixels_stretched(pixels, line, count);
    break;
  case ScalingMode::STRETCH_KEEP_ASPECT:
    lcd_write_pixels_stretched_keep_aspect(pixels, line, count);
    break;
  case ScalingMode::NORMAL:
  default:
    lcd_write_pixels_normal(pixels, line, count);
    break;
  }
}


#if ENABLE_LCD_FRAMEBUFFER

void lcd_swap_buffers() {
#if BUFFER_COUNT == 2
  activeFramebufferId = (activeFramebufferId == 0) ? 1 : 0;
#endif
}

// Writes framebuffer to screen
void lcd_write_framebuffer_to_screen() {
  uint16_t* framebuffer = framebuffers[activeFramebufferId];
  tft.setSwapBytes(true);
#if ENABLE_LCD_DMA
  tft.startWrite(); // manual start required as DMA transfer is asynchronous
  tft.pushImageDMA(0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, framebuffer);
  //tft.endWrite(); // do not call endWrite(), as it will wait for the DMA transfer to finish, which results in no performance gain
  lcd_swap_buffers();
#else
  tft.pushImage(0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, framebuffer);
#endif
}

#endif


void lcd_clear() {
  lcd_line_hashes_valid = false;
#if ENABLE_LCD_FRAMEBUFFER
  memset(framebuffers[0], 0, FRAMEBUFFER_PIXELS * sizeof(uint16_t));
#if BUFFER_COUNT == 2
  memset(framebuffers[1], 0, FRAMEBUFFER_PIXELS * sizeof(uint16_t));
#endif
#else
  tft.fillScreen(TFT_BLACK);
#endif
}

void core1_lcd_draw_line(const uint8_t* source, const uint_fast8_t line) {
  static uint16_t fb[LCD_WIDTH];
  uint32_t hash = 2166136261u ^ lcd_frame_palette_hash;

  // Hash four palette indices at a time and postpone RGB565 conversion until
  // the row is known to have changed. During scrolling this cuts the hash loop
  // to one quarter; in menus it also avoids all 160 palette lookups.
  for (unsigned int x = 0; x < LCD_WIDTH; x += sizeof(uint32_t)) {
    uint32_t packed;
    memcpy(&packed, &source[x], sizeof(packed));
    hash = (hash ^ packed) * 16777619u;
  }

  if (lcd_line_hashes_valid && lcd_line_hashes[line] == hash) return;
  lcd_line_hashes[line] = hash;

  if (gb.cgb.cgbMode) {
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb[x] = gb.cgb.fixPalette[source[x]];
    }
  } else {
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb[x] = palette[(source[x] & LCD_PALETTE_ALL) >> 4]
                    [source[x] & 3];
    }
  }

  lcd_write_pixels(fb, line, LCD_WIDTH);

#if ENABLE_LCD_FRAMEBUFFER
  if (line == LCD_HEIGHT - 1) {
    lcd_write_framebuffer_to_screen();
  }
#endif
}

void core1_lcd_draw_frame(const uint8_t framebufferId) {
  const uint8_t* source = gbFramebuffers[framebufferId & 1u];
  lcd_frame_palette_hash = 2166136261u;
  if (gb.cgb.cgbMode) {
    for (uint8_t i = 0; i < 0x40; ++i) {
      lcd_frame_palette_hash =
          (lcd_frame_palette_hash ^ gb.cgb.fixPalette[i]) * 16777619u;
    }
  }
  for (uint_fast16_t line = 0; line < LCD_HEIGHT; ++line) {
    core1_lcd_draw_line(&source[line * LCD_WIDTH], line);
  }
  // The renderer deliberately keeps its DMA transaction open between lines.
  // Close it before selecting the touch controller, then restore the LCD
  // transaction afterward. Without this handoff, a touch read raises LCD CS
  // and subsequent frames remain black while emulation keeps running.
  if (chriscade_gameplay_touch_enabled()) {
    tft.endWrite();
    pollGameplayTouch();
    tft.startWrite();
  }
  lcd_line_hashes_valid = true;
  displayedFramebuffer = framebufferId & 1u;
  displayedFrameValid = true;
  __atomic_store_n(&lcd_frame_busy, 0, __ATOMIC_SEQ_CST);
}

void core1DispatchLoop() {
  union core_cmd cmd;

  // Handle commands coming from core0
  cmd.full = multicore_fifo_pop_blocking();
  switch (cmd.cmd) {
  case CORE_CMD_LCD_LINE:
    break;

  case CORE_CMD_LCD_FRAME:
    core1_lcd_draw_frame(cmd.data);
    break;

  case CORE_CMD_SCREENSHOT_PAUSE: {
    // Only the display owner may finish its open transaction. In particular,
    // do not call lcd_init/initDMA or multicore_reset_core1 on this path.
    tft.dmaWait();
    tft.endWrite();
    uint32_t expected = 1;
    if (__atomic_compare_exchange_n(&screenshot_state, &expected, 2u, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&screenshot_state, __ATOMIC_ACQUIRE) == 2u)
        tight_loop_contents();
    }
    tft.startWrite();
    break;
  }

  case CORE_CMD_IDLE_SET:
    lcd_clear();
    break;

  case CORE_CMD_NOP:
  default:
    break;
  }
}

void core1_init() {
  // Initialise and control LCD on core 1
  lcd_init(true);

  lcd_clear();

  calcExtraLineTable();

  while (true) {
    core1DispatchLoop();
  }
}
