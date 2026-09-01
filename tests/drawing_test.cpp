#include "../src/drawing_picture.h"
#include "../src/game_picture.h"
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
extern "C" void* memcpy(void* to, const void* from, size_t size) {
  auto dst = (uint8_t*)to; auto src = (const uint8_t*)from;
  while (size--) *dst++ = *src++;
  return to;
}
extern "C" void* memset(void* to, int value, size_t size) {
  auto dst = (uint8_t*)to; while (size--) *dst++ = value; return to;
}
static uint8_t guarded[DrawingCanvas::BYTES + 16];
static uint8_t visible[DrawingCanvas::WIDTH * DrawingCanvas::HEIGHT];
static uint16_t row[DrawingCanvas::WIDTH];
static uint8_t game_frame[160 * 144];
static uint16_t game_colors[64];
static uint16_t expected_game[320 * 240];
static const uint16_t palette[16] = {0x0884,0x269C,0xF1B5,0x46EF,0xFDA4,0xF7BD,0,0,0,0,0,0,0,0,0,0xEF7C};
extern "C" { uint8_t saved_picture[28 + 320 * 240 * 2]; }
struct FakeFS {
  bool destination, temporary, fail_rename, fail_remove;
  unsigned removes, renames;
  bool exists(const char* name) { return name[0] == 'T' ? temporary : destination; }
  bool rename(const char*, const char*) {
    ++renames;
    if (fail_rename || destination || !temporary) return false;
    destination = true; temporary = false; return true;
  }
  bool remove(const char* name) {
    ++removes;
    if (name[0] != 'T' || fail_remove) return false;
    temporary = false; return true;
  }
};
struct FakeFile {
  FakeFS* fs;
  unsigned size, writes, syncs, closes, opens;
  int fail_write;
  bool fail_open, fail_sync, fail_close, fail_read, corrupt_header;
  bool open(const char*, int flags) {
    ++opens;
    if (fail_open) return false;
    if (flags == 1) { if (fs->temporary) return false; fs->temporary = true; size = 0; }
    return fs->temporary;
  }
  size_t write(const void* bytes, size_t count) {
    const unsigned index = writes++;
    if ((int)index == fail_write) return count ? count - 1 : 0;
    if (size + count > sizeof(saved_picture)) return 0;
    memcpy(saved_picture + size, bytes, count); size += count; return count;
  }
  bool sync() { ++syncs; return !fail_sync; }
  bool close() { ++closes; return !fail_close; }
  size_t read(void* bytes, size_t count) {
    if (fail_read) return 0;
    memcpy(bytes, saved_picture, count);
    if (corrupt_header) ((uint8_t*)bytes)[0] ^= 1;
    return count;
  }
  unsigned fileSize() const { return size; }
};

extern "C" int run_drawing_tests() {
  memset(guarded, 0xA5, sizeof(guarded));
  DrawingCanvas canvas(guarded + 8);
  canvas.clear();
  memset(visible, DrawingCanvas::PAPER, sizeof(visible));
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 320; ++x) CHECK(canvas.at(x, y) == DrawingCanvas::PAPER);
  CHECK(canvas.at(-1, 30) == DrawingCanvas::PAPER);
  CHECK(canvas.at(320, 240) == DrawingCanvas::PAPER);
  uint8_t color = 0;
  auto span = [&](int x, int y, int width) {
    for (int i = 0; i < width; ++i) visible[y * 320 + x + i] = color;
  };
  // Widely-spaced stylus samples must produce solid thick strokes.
  for (int radius = 3; radius <= 9; radius += 3) {
    color = radius / 3 - 1;
    const int y = 65 + radius * 8;
    canvas.segment(30, y, 280, y, radius, color, span);
    for (int x = 30; x <= 280; ++x)
      for (int dy = -radius + 1; dy < radius; ++dy) CHECK(canvas.at(x, y + dy) == color);
  }
  color = 3;
  canvas.segment(20, 45, 300, 198, 6, color, span);
  color = 4;
  canvas.segment(300, 45, 20, 198, 9, color, span);
  color = 5;
  canvas.stamp(160, 120, 9, color, span);
  for (int x = 0; x < 320; x += 17) {
    canvas.stamp(x, 42, 9, color, span);
    canvas.stamp(x, 200, 9, color, span);
  }
  for (int y = 0; y < 240; y += 17) {
    canvas.stamp(8, y, 9, color, span);
    canvas.stamp(311, y, 9, color, span);
  }
  for (int y = 0; y < 240; ++y) {
    canvas.row(y, palette, row);
    for (int x = 0; x < 320; ++x) {
      CHECK(canvas.at(x, y) == visible[y * 320 + x]);
      CHECK(row[x] == palette[visible[y * 320 + x]]);
      if (!DrawingCanvas::inside(x, y)) CHECK(canvas.at(x, y) == DrawingCanvas::PAPER);
    }
  }
  for (int i = 0; i < 8; ++i) {
    CHECK(guarded[i] == 0xA5 && guarded[DrawingCanvas::BYTES + 8 + i] == 0xA5);
  }

  FakeFS fs = {};
  FakeFile file = {}; file.fs = &fs; file.fail_write = -1;
  CHECK(DrawingPicture::publish(fs, file, "TEMP", "PICTURE", canvas, palette, row, 1, 0));
  CHECK(fs.destination && !fs.temporary && fs.renames == 1 && !fs.removes);
  CHECK(file.writes == 241 && file.syncs == 1 && file.closes == 2 && file.opens == 2);
  const auto& header = *(const DrawingPicture::Header*)saved_picture;
  CHECK(DrawingPicture::valid(header, sizeof(saved_picture)));
  CHECK(header.source[0] == 'D' && header.source[3] == 'W' && header.source[4] == 0);
  CHECK(!DrawingPicture::valid(header, sizeof(saved_picture) - 1));
  for (unsigned i = 0; i < 320 * 240; ++i) {
    const unsigned offset = 28 + i * 2;
    CHECK((saved_picture[offset] | (saved_picture[offset + 1] << 8)) == palette[visible[i]]);
  }
  // Every write stage, sync, close, verification and publish error must fail.
  for (int failure = 0; failure < 10; ++failure) {
    fs = {}; file = {}; file.fs = &fs; file.fail_write = -1;
    if (failure < 4) file.fail_write = failure == 0 ? 0 : failure == 1 ? 1 : failure == 2 ? 80 : 240;
    if (failure == 4) file.fail_sync = true;
    if (failure == 5) file.fail_close = true;
    if (failure == 6) file.fail_read = true;
    if (failure == 7) file.corrupt_header = true;
    if (failure == 8) fs.fail_rename = true;
    if (failure == 9) file.fail_open = true;
    CHECK(!DrawingPicture::publish(fs, file, "TEMP", "PICTURE", canvas, palette, row, 1, 0));
    CHECK(!fs.destination && !fs.temporary);
    CHECK(fs.removes == (failure == 9 ? 0u : 1u));
    CHECK(canvas.at(160, 120) == 5); // Failed save cannot erase the drawing.
  }
  for (int collision = 0; collision < 2; ++collision) {
    fs = {}; file = {}; file.fs = &fs; file.fail_write = -1;
    fs.destination = collision == 0; fs.temporary = collision == 1;
    CHECK(!DrawingPicture::publish(fs, file, "TEMP", "PICTURE", canvas, palette, row, 1, 0));
    CHECK(!fs.removes && !fs.renames && !file.opens);
    CHECK(fs.destination == (collision == 0) && fs.temporary == (collision == 1));
  }
  // Restore a successful artifact for pixel-level inspection by the runner.
  // Game capture uses real palette indices, not TFT readback. Compare every
  // exported pixel against an independent forward-repeat renderer in all modes.
  for (unsigned color = 0; color < 64; ++color) game_colors[color] = (uint16_t)(0x1100 + color * 37);
  for (unsigned cgb = 0; cgb < 2; ++cgb) {
    for (unsigned i = 0; i < sizeof(game_frame); ++i)
      game_frame[i] = cgb ? (i * 7 + i / 160) % 64 : ((i % 3) << 4) | (i % 4);
    for (unsigned mode = 0; mode < 3; ++mode) {
      memset(expected_game, 0, sizeof(expected_game));
      unsigned dy = mode == 0 ? 48 : 0;
      for (unsigned sy = 0; sy < 144; ++sy) {
        const unsigned repeats_y = mode == 0 ? 1 : 1 + ((sy % 2) || (sy % 6 == 0));
        for (unsigned ry = 0; ry < repeats_y; ++ry, ++dy) {
          unsigned dx = mode == 0 ? 80 : mode == 1 ? 0 : 26;
          for (unsigned sx = 0; sx < 160; ++sx) {
            const uint8_t index = game_frame[sy * 160 + sx];
            const uint16_t value = game_colors[cgb ? index : (index >> 4) * 4 + (index & 3)];
            const unsigned repeats_x = mode == 0 ? 1 : mode == 1 ? 2 : 1 + ((sx % 2) || (sx % 6 == 0));
            for (unsigned rx = 0; rx < repeats_x; ++rx) expected_game[dy * 320 + dx++] = value;
          }
        }
      }
      const GamePicture picture = {game_frame, game_colors, cgb != 0, mode};
      const DrawingPicture::Header game_header = {DrawingPicture::MAGIC, 320, 240, 153600, "POKEMON CRYSTAL"};
      fs = {}; file = {}; file.fs = &fs; file.fail_write = -1;
      CHECK(DrawingPicture::publish_rows(fs, file, "TEMP", "PICTURE", game_header,
          [&](int y, uint16_t* pixels) { picture.row(y, pixels); }, row, 1, 0));
      for (unsigned i = 0; i < 320 * 240; ++i) {
        const unsigned offset = 28 + i * 2;
        CHECK((saved_picture[offset] | (saved_picture[offset + 1] << 8)) == expected_game[i]);
      }
      CHECK(((DrawingPicture::Header*)saved_picture)->source[0] == 'P');
    }
  }
  uint32_t noise = 0x43485249u;
  unsigned clicks = 0;
  for (unsigned i = 0; i < 1600; ++i) {
    const int sample = screenshot_shutter_sample(i, noise);
    CHECK(sample >= -10240 && sample <= 10240);
    if ((i >= 300 && i < 650) || i >= 1100) CHECK(sample == 0);
    if (sample) ++clicks;
  }
  CHECK(clicks > 650);
  // Leave the original drawing preview for regression verification.
  fs = {}; file = {}; file.fs = &fs; file.fail_write = -1;
  CHECK(DrawingPicture::publish(fs, file, "TEMP", "PICTURE", canvas, palette, row, 1, 0));
  canvas.clear();
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 320; ++x) CHECK(canvas.at(x, y) == DrawingCanvas::PAPER);
  return 0;
}
