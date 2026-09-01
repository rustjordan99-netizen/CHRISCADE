#pragma once
#include "rom_file_actions.h"

namespace RomUpload {
static constexpr uint32_t max_bytes = 16u * 1024u * 1024u;
static constexpr size_t chunk_bytes = 512;
inline int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = RomFileActions::fold(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
// No paths or non-ROM names; the computer never gets general SD access.
inline bool parse_header(const char* line, uint32_t& size, uint32_t& crc, char* name) {
  if (strncmp(line, "CCROM1 ", 7)) return false;
  const char* p = line + 7;
  size = 0;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') {
    if (size > max_bytes / 10) return false;
    size = size * 10 + (*p++ - '0');
    if (size > max_bytes) return false;
  }
  if (!size || *p++ != ' ') return false;
  crc = 0;
  for (int i = 0; i < 8; ++i) {
    const int digit = hex_digit(*p++);
    if (digit < 0) return false;
    crc = (crc << 4) | digit;
  }
  if (*p++ != ' ') return false;
  size_t n = 0;
  while (*p) {
    if (n >= RomFileActions::name_capacity - 1) return false;
    const int hi = hex_digit(*p++);
    if (hi < 0 || !*p) return false;
    const int lo = hex_digit(*p++);
    if (lo < 0 || !(hi || lo)) return false;
    name[n++] = static_cast<char>((hi << 4) | lo);
  }
  name[n] = '\0';
  return RomFileActions::valid_rom(name);
}
inline uint32_t crc_update(uint32_t crc, const uint8_t* data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// Instantiate with FsFile on device, an in-memory fake in tests. Every write
// goes to a freshly created exclusive temp file; existing files are untouched.
template<class File> struct Receiver {
  File root, file;
  bool created = false;
  uint32_t received = 0, crc = 0xFFFFFFFFu;
  bool begin(const char* destination, const char* temporary, int read_flags, int create_flags) {
    if (created || !RomFileActions::valid_rom(destination) ||
        !RomFileActions::valid_leaf(temporary) ||
        !RomFileActions::same_name(temporary + strlen(temporary) -
            (strlen(temporary) >= 4 ? 4 : 0), ".TMP")) return false;
    if (!root.open("/", read_flags)) return false;
    File existing;
    if (existing.open(&root, destination, read_flags)) { root.close(); return false; }
    if (!file.open(&root, temporary, create_flags)) { root.close(); return false; }
    created = true;
    received = 0;
    crc = 0xFFFFFFFFu;
    return true;
  }
  bool write(const uint8_t* data, size_t length, uint32_t expected_size) {
    if (!created || expected_size > max_bytes || received > expected_size ||
        length > expected_size - received || length > chunk_bytes) return false;
    if (file.write(data, length) != length) return false;
    crc = crc_update(crc, data, length);
    received += length;
    return true;
  }
  bool commit(const char* destination, uint32_t size, uint32_t checksum) {
    if (!created || !RomFileActions::valid_rom(destination) || received != size ||
        (crc ^ 0xFFFFFFFFu) != checksum || !file.sync()) return false;
    // SdFat rename refuses an existing destination (including directories).
    if (!file.rename(&root, destination)) return false;
    created = false;
    file.close();
    root.close();
    return true;
  }
  void cancel() {
    if (created) file.remove(); // Only the exclusive file this receiver created.
    created = false;
    file.close();
    root.close();
  }
};
} // namespace RomUpload
