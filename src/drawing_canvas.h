#pragma once
#include <stdint.h>
#include <stddef.h>

// App-only storage: borrow the idle GB framebuffers before core 1 is launched.
// Seven fixed colors fit in four bits; never read pixels back from the TFT.
class DrawingCanvas {
public:
  static constexpr int WIDTH = 320;
  static constexpr int HEIGHT = 240;
  // A packed nibble supports fifteen ink colors plus one transparent paper
  // value, without increasing the drawing buffer's RAM footprint.
  static constexpr uint8_t PAPER = 15;
  static constexpr size_t BYTES = WIDTH * HEIGHT / 2;
  explicit DrawingCanvas(uint8_t* storage) : pixels(storage) {}
  void clear() {
    for (size_t i = 0; i < BYTES; ++i) pixels[i] = PAPER | (PAPER << 4);
  }
  uint8_t at(int x, int y) const {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return PAPER;
    const unsigned index = y * WIDTH + x;
    return (pixels[index / 2] >> ((index & 1) * 4)) & 15;
  }
  void set(int x, int y, uint8_t color) {
    if (!inside(x, y) || color > PAPER) return;
    const unsigned index = y * WIDTH + x, shift = (index & 1) * 4;
    pixels[index / 2] = (pixels[index / 2] & ~(15u << shift)) | (color << shift);
  }
  void row(int y, const uint16_t palette[16], uint16_t* output) const {
    for (int x = 0; x < WIDTH; ++x) {
      const uint8_t color = at(x, y);
      output[x] = palette[color <= PAPER ? color : PAPER];
    }
  }
  static bool inside(int x, int y) {
    // Inset rounded canvas: brush edges cannot overwrite the frame or toolbar.
    if (x < 8 || x > 311 || y < 42 || y > 200) return false;
    const int dx = x < 18 ? 18 - x : x > 301 ? x - 301 : 0;
    const int dy = y < 52 ? 52 - y : y > 190 ? y - 190 : 0;
    return dx * dx + dy * dy <= 100;
  }
  template<class Span>
  void stamp(int cx, int cy, int radius, uint8_t color, Span span) {
    if (radius < 1 || radius > 9 || color >= PAPER) return;
    for (int y = cy - radius; y <= cy + radius; ++y) {
      int first = -1, last = -1;
      for (int x = cx - radius; x <= cx + radius; ++x) {
        const int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > radius * radius || !inside(x, y)) continue;
        const unsigned index = y * WIDTH + x;
        const unsigned shift = (index & 1) * 4;
        pixels[index / 2] = (pixels[index / 2] & ~(15u << shift)) | (color << shift);
        if (first < 0) first = x;
        last = x;
      }
      if (first >= 0) span(first, y, last - first + 1);
    }
  }
  template<class Before, class Span>
  void segment_tracked(int x0, int y0, int x1, int y1, int radius, uint8_t color,
      Before before, Span span) {
    const int dx = x1 - x0, dy = y1 - y0;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int distance = ax > ay ? ax : ay;
    const int spacing = radius / 2 > 0 ? radius / 2 : 1;
    auto tracked_stamp = [&](int cx, int cy) {
      if (radius < 1 || radius > 9 || color >= PAPER) return;
      for (int y = cy - radius; y <= cy + radius; ++y) {
        int first = -1, last = -1;
        for (int x = cx - radius; x <= cx + radius; ++x) {
          const int dx = x - cx, dy = y - cy;
          if (dx * dx + dy * dy > radius * radius || !inside(x, y)) continue;
          before(x, y, at(x, y)); set(x, y, color);
          if (first < 0) first = x;
          last = x;
        }
        if (first >= 0) span(first, y, last - first + 1);
      }
    };
    if (distance) for (int step = 0; step < distance; step += spacing)
      tracked_stamp(x0 + dx * step / distance, y0 + dy * step / distance);
    tracked_stamp(x1, y1);
  }
  template<class Span>
  void segment(int x0, int y0, int x1, int y1, int radius, uint8_t color, Span span) {
    const int dx = x1 - x0, dy = y1 - y0;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int distance = ax > ay ? ax : ay;
    const int spacing = radius / 2 > 0 ? radius / 2 : 1;
    if (distance) {
      for (int step = 0; step < distance; step += spacing)
        stamp(x0 + dx * step / distance, y0 + dy * step / distance, radius, color, span);
    }
    stamp(x1, y1, radius, color, span);
  }
private:
  uint8_t* pixels;
};
