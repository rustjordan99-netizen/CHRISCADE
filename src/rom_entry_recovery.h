#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Last resort for one confirmed FAT32 ROM deletion. Never follows/frees FAT
// chains: a broken or cross-linked chain may describe someone else's data.
// Snapshots come from the exact root entry opened by its full selected name.
namespace RomEntryRecovery {
struct Entry {
  uint32_t sector;
  uint16_t offset;
  uint8_t bytes[32];
};
struct Workspace {
  Entry entries[21]; // short entry plus at most 20 long-name entries
  uint8_t before[512];
  uint8_t after[512];
};
inline uint8_t checksum(const uint8_t* name) {
  uint8_t sum = 0;
  for (unsigned i = 0; i < 11; ++i)
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
  return sum;
}
inline bool rom_entry(const uint8_t* p) {
  if (!p[0] || p[0] == 0xe5 || p[0] == '.' || (p[11] & 0x18)) return false;
  return p[8] == 'G' && p[9] == 'B' &&
      (p[10] == ' ' || p[10] == 'C' || p[10] == 'Z');
}
inline bool long_entry(const uint8_t* p, unsigned order, uint8_t sum) {
  return order >= 1 && order <= 20 && (p[0] == order || p[0] == (order | 0x40)) &&
      p[11] == 0x0f && p[12] == 0 && p[13] == sum && p[26] == 0 && p[27] == 0;
}

// Device exposes only bounded sector I/O, never allocation-table operations.
// Recheck every snapshot before any write and again before its sector write.
// Only directory entry first bytes change, to FAT's deleted marker (0xE5).
template<class Device>
bool erase(Device& device, Workspace& w, unsigned count,
    uint32_t data_start, uint32_t data_end) {
  if (!count || count > 21 || data_start >= data_end ||
      !rom_entry(w.entries[0].bytes)) return false;
  const uint8_t sum = checksum(w.entries[0].bytes);
  for (unsigned i = 0; i < count; ++i) {
    const Entry& e = w.entries[i];
    if (e.sector < data_start || e.sector >= data_end ||
        e.offset > 480 || (e.offset & 31)) return false;
    if (i && (!long_entry(e.bytes, i, sum) ||
        (bool)(e.bytes[0] & 0x40) != (i == count - 1))) return false;
    for (unsigned j = 0; j < i; ++j)
      if (e.sector == w.entries[j].sector && e.offset == w.entries[j].offset) return false;
    if (!device.readSector(e.sector, w.before) ||
        memcmp(w.before + e.offset, e.bytes, 32)) return false;
  }
  // Order begins with the short entry: interruption cannot leave a live file
  // pointing to freed/reallocated data (no data clusters are freed at all).
  for (unsigned i = 0; i < count; ++i) {
    const uint32_t sector = w.entries[i].sector;
    bool done = false;
    for (unsigned j = 0; j < i; ++j) if (w.entries[j].sector == sector) done = true;
    if (done) continue;
    if (!device.readSector(sector, w.before)) return false;
    memcpy(w.after, w.before, 512);
    for (unsigned j = i; j < count; ++j) {
      const Entry& e = w.entries[j];
      if (e.sector != sector) continue;
      if (memcmp(w.before + e.offset, e.bytes, 32)) return false;
      w.after[e.offset] = 0xe5;
    }
    if (!device.writeSector(sector, w.after) || !device.syncDevice() ||
        !device.readSector(sector, w.before) || memcmp(w.before, w.after, 512)) return false;
  }
  return true;
}
} // namespace RomEntryRecovery
