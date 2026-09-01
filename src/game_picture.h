#pragma once
#include <stdint.h>

// Read only while emulation is paused and the LCD owner has yielded SPI.
// Reconstruct the exact nearest-neighbour scaling used by lcd_core.cpp.
struct GamePicture {
  const uint8_t* pixels;
  const uint16_t* colors;
  bool cgb;
  unsigned scaling; // 0=native, 1=stretch, 2=keep aspect

  static unsigned scaled_source(unsigned position) {
    static const uint8_t source[10] = {0,0,1,1,2,3,3,4,5,5};
    return (position / 10u) * 6u + source[position % 10u];
  }
  void row(unsigned y, uint16_t* output) const {
    const unsigned width = scaling == 0 ? 160 : scaling == 1 ? 320 : 267;
    const unsigned left = (320 - width) / 2;
    const unsigned top = scaling == 0 ? 48 : 0;
    for (unsigned x = 0; x < 320; ++x) output[x] = 0;
    if (y < top || y >= (scaling == 0 ? 192u : 240u)) return;
    const unsigned sy = scaling == 0 ? y - top : scaled_source(y);
    for (unsigned x = 0; x < width; ++x) {
      const unsigned sx = scaling == 0 ? x : scaling == 1 ? x / 2 : scaled_source(x);
      const uint8_t index = pixels[sy * 160 + sx];
      const unsigned palette_index = cgb ? index & 63u : ((index & 0x30u) >> 2) + (index & 3u);
      output[left + x] = (!cgb && palette_index >= 12) ? 0 : colors[palette_index];
    }
  }
};

// Small deterministic two-click camera shutter. Generated into the existing
// gameplay stereo buffer; it needs no sample asset or extra PCM allocation.
inline int16_t screenshot_shutter_sample(unsigned index, uint32_t& noise) {
  unsigned offset;
  if (index < 300) offset = index;
  else if (index >= 650 && index < 1100) offset = index - 650;
  else return 0;
  noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
  const unsigned duration = index < 300 ? 300 : 450;
  const unsigned attack = offset < 16 ? offset : 16;
  const int amplitude = (int)((duration - offset) * attack * 20 / duration);
  return (int16_t)(((int)(noise & 255u) - 128) * amplitude / 4);
}
