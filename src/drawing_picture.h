#pragma once
#include "drawing_canvas.h"

namespace DrawingPicture {
constexpr uint32_t MAGIC = 0x53484343u;
struct Header {
  uint32_t magic;
  uint16_t width;
  uint16_t height;
  uint32_t bytes;
  char source[16];
};
static_assert(sizeof(Header) == 28, "Keep the existing gallery format");
inline bool valid(const Header& header, uint32_t file_size) {
  return header.magic == MAGIC && header.width == DrawingCanvas::WIDTH &&
      header.height == DrawingCanvas::HEIGHT &&
      header.bytes == DrawingCanvas::WIDTH * DrawingCanvas::HEIGHT * 2u &&
      file_size == sizeof(Header) + header.bytes;
}
// Write to a newly-created temporary file. The caller publishes it by renaming
// only after sync/close, so a failed write never becomes a visible picture.
template<class File, class Row>
bool write_rows(File& file, const Header& header, Row render_row, uint16_t* row) {
  if (file.write(&header, sizeof(header)) != sizeof(header)) return false;
  for (int y = 0; y < DrawingCanvas::HEIGHT; ++y) {
    render_row(y, row);
    if (file.write(row, DrawingCanvas::WIDTH * sizeof(uint16_t)) != DrawingCanvas::WIDTH * sizeof(uint16_t))
      return false;
  }
  return file.sync();
}

template<class FS, class File, class Row>
bool publish_rows(FS& fs, File& file, const char* temporary, const char* name,
    const Header& header, Row render_row, uint16_t* row,
    int create_flags, int read_flags) {
  if (fs.exists(name) || fs.exists(temporary)) return false;
  if (!file.open(temporary, create_flags)) return false;
  bool success = write_rows(file, header, render_row, row);
  const bool closed = file.close();
  success = success && closed;
  if (success) {
    Header header = {};
    success = file.open(temporary, read_flags);
    if (success) {
      success = file.read(&header, sizeof(header)) == sizeof(header) && valid(header, file.fileSize());
      const bool verify_closed = file.close();
      success = success && verify_closed;
    }
  }
  if (success) success = !fs.exists(name) && fs.rename(temporary, name);
  if (!success) fs.remove(temporary); // Only the file this attempt created.
  return success;
}

template<class FS, class File>
bool publish(FS& fs, File& file, const char* temporary, const char* name,
    const DrawingCanvas& canvas, const uint16_t palette[16], uint16_t* row,
    int create_flags, int read_flags) {
  const Header header = {MAGIC, DrawingCanvas::WIDTH, DrawingCanvas::HEIGHT,
      DrawingCanvas::WIDTH * DrawingCanvas::HEIGHT * 2u, "DRAW"};
  return publish_rows(fs, file, temporary, name, header,
      [&](int y, uint16_t* pixels) { canvas.row(y, palette, pixels); },
      row, create_flags, read_flags);
}
} // namespace DrawingPicture
