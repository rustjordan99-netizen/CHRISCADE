#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace RomFileActions {
static constexpr size_t name_capacity = 256;
enum class Action { RENAME, REMOVE, FORCE_REMOVE };
enum class Result { OK, INVALID_NAME, NOT_FOUND, READ_ONLY, EXISTS, SD_ERROR,
    UNCHANGED, DELETE_FAILED, REMOVED_DAMAGED };

inline char fold(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
inline bool same_name(const char* a, const char* b) {
  while (*a && *b && fold(*a) == fold(*b)) { ++a; ++b; }
  return fold(*a) == fold(*b);
}
inline size_t bounded_length(const char* name) {
  size_t n = 0;
  if (name) while (n < name_capacity && name[n]) ++n;
  return n;
}
inline const char* extension(const char* name) {
  const size_t n = bounded_length(name);
  if (!name || !n || n == name_capacity) return nullptr;
  const char* ext = strrchr(name, '.');
  if (!ext || ext == name) return nullptr;
  return same_name(ext, ".gb") || same_name(ext, ".gbc") || same_name(ext, ".gbz")
      ? ext : nullptr;
}
inline bool valid_leaf(const char* name) {
  const size_t n = bounded_length(name);
  if (!name || !n || n == name_capacity || name[0] == '.' ||
      name[n - 1] == ' ' || name[n - 1] == '.') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = name[i];
    if (c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return false;
  }
  // Keep newly created names usable when the SD card goes back into Windows.
  char device[5] = {};
  size_t first = 0;
  while (name[first] && name[first] != '.') ++first;
  if (first <= 4) {
    for (size_t i = 0; i < first; ++i) device[i] = fold(name[i]);
    if (!strcmp(device, "con") || !strcmp(device, "prn") ||
        !strcmp(device, "aux") || !strcmp(device, "nul") ||
        (first == 4 && (!memcmp(device, "com", 3) || !memcmp(device, "lpt", 3)) &&
         device[3] >= '1' && device[3] <= '9')) return false;
  }
  return true;
}
inline bool valid_rom(const char* name) { return valid_leaf(name) && extension(name); }

// Input is a title without its extension. The original extension is immutable.
inline bool renamed_leaf(const char* original, const char* stem, char* out) {
  if (!valid_rom(original) || !stem || !out) return false;
  size_t n = bounded_length(stem);
  if (n == name_capacity) return false;
  while (n && *stem == ' ') { ++stem; --n; }
  while (n && stem[n - 1] == ' ') --n;
  const char* ext = extension(original);
  const size_t ext_len = strlen(ext);
  if (!n || stem[n - 1] == '.' || n + ext_len >= name_capacity) return false;
  memcpy(out, stem, n);
  memcpy(out + n, ext, ext_len + 1);
  return valid_rom(out);
}

// File is FsFile on the device and a fake filesystem in regression tests.
// Open the exact root-directory entry, never a path or recursively a folder.
template<class File>
Result apply(Action action, const char* source, const char* destination,
    int read_only_flags, int read_write_flags) {
  if (!valid_rom(source)) return Result::INVALID_NAME;
  if (action == Action::RENAME) {
    if (!valid_rom(destination) ||
        !same_name(extension(source), extension(destination))) return Result::INVALID_NAME;
    if (same_name(source, destination)) return Result::UNCHANGED;
  }
  File root, file;
  if (!root.open("/", read_only_flags)) return Result::SD_ERROR;
  if (!file.open(&root, source, read_only_flags)) return Result::NOT_FOUND;
  if (!file.isFile()) return Result::INVALID_NAME;
  const bool read_only = file.isReadOnly();
  if (read_only && action != Action::FORCE_REMOVE) return Result::READ_ONLY;
  // Force removal is intentionally narrow: clear only the FAT read-only bit
  // on this already-open exact ROM, then continue through normal removal.
  if (action == Action::FORCE_REMOVE) {
    if (!read_only) return Result::UNCHANGED;
    const int attributes = file.attrib();
    if (attributes < 0 || !file.attrib((uint8_t)(attributes & ~0x01u)))
      return Result::SD_ERROR;
  }
  file.close();
  if (!file.open(&root, source, read_write_flags)) return Result::SD_ERROR;
  // Recheck the opened object before any mutation.
  if (!file.isFile() || file.isReadOnly()) return Result::READ_ONLY;
  if (action != Action::RENAME) return file.remove() ? Result::OK : Result::DELETE_FAILED;
  File existing;
  if (existing.open(&root, destination, read_only_flags)) return Result::EXISTS;
  // SdFat also refuses an existing target in rename(), including directories.
  return file.rename(&root, destination) ? Result::OK : Result::SD_ERROR;
}
} // namespace RomFileActions
