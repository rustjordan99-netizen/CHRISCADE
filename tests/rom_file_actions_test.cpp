#include "../src/rom_file_actions.h"
#include "../src/rom_entry_recovery.h"

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
using namespace RomFileActions;

extern "C" size_t strlen(const char* p) { size_t n = 0; while (p[n]) ++n; return n; }
extern "C" int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (unsigned char)*a - (unsigned char)*b;
}
extern "C" int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* x = static_cast<const unsigned char*>(a);
  const unsigned char* y = static_cast<const unsigned char*>(b);
  while (n--) { if (*x != *y) return *x - *y; ++x; ++y; }
  return 0;
}
extern "C" char* strchr(const char* p, int value) {
  do { if (*p == value) return const_cast<char*>(p); } while (*p++);
  return nullptr;
}
extern "C" char* strrchr(const char* p, int value) {
  const char* last = nullptr;
  do { if (*p == value) last = p; } while (*p++);
  return const_cast<char*>(last);
}
extern "C" void* memcpy(void* to, const void* from, size_t n) {
  char* dst = static_cast<char*>(to);
  const char* src = static_cast<const char*>(from);
  while (n--) *dst++ = *src++;
  return to;
}
extern "C" void* memset(void* to, int value, size_t n) {
  unsigned char* p = static_cast<unsigned char*>(to);
  while (n--) *p++ = value;
  return to;
}

struct Entry { char name[256]; bool present, directory, read_only; unsigned data; };
static Entry entries[5];
static unsigned mutations, opens;
static bool fail_root, fail_mutation, fail_write;
struct FakeFile {
  bool root = false;
  int index = -1;
  bool open(const char* name, int) { ++opens; root = !fail_root && !strcmp(name, "/"); return root; }
  bool open(FakeFile* parent, const char* name, int flags) {
    ++opens;
    if (!parent->root || (flags == 2 && fail_write)) return false;
    for (int i = 0; i < 5; ++i) {
      if (entries[i].present && same_name(entries[i].name, name)) {
        index = i;
        return true;
      }
    }
    return false;
  }
  bool isFile() const { return index >= 0 && !entries[index].directory; }
  bool isReadOnly() const { return index >= 0 && entries[index].read_only; }
  int attrib() const { return index >= 0 ? (entries[index].read_only ? 1 : 0) : -1; }
  bool attrib(uint8_t bits) {
    if (index < 0 || fail_mutation) return false;
    entries[index].read_only = (bits & 1) != 0;
    return true;
  }
  void close() { root = false; index = -1; }
  bool remove() {
    if (fail_mutation || index < 0) return false;
    entries[index].present = false;
    ++mutations;
    return true;
  }
  bool rename(FakeFile* parent, const char* name) {
    if (fail_mutation || !parent->root || index < 0) return false;
    for (const auto& entry : entries)
      if (entry.present && same_name(entry.name, name)) return false;
    memcpy(entries[index].name, name, strlen(name) + 1);
    ++mutations;
    return true;
  }
};

static void reset_files() {
  memset(entries, 0, sizeof(entries));
  const char* names[] = {"Pokemon Crystal.gbz", "Mario.GBC", "POKEMON CRYSTAL", "SETTINGS.BIN", "folder.gb"};
  for (unsigned i = 0; i < 5; ++i) {
    memcpy(entries[i].name, names[i], strlen(names[i]) + 1);
    entries[i].present = true;
    entries[i].data = 100 + i;
  }
  entries[4].directory = true;
  mutations = opens = 0;
  fail_root = fail_mutation = fail_write = false;
}
static Result action(Action kind, const char* name, const char* target = nullptr) {
  return apply<FakeFile>(kind, name, target, 0, 2);
}

// Model directory sectors beside unrelated FAT/save/recovery-ROM bytes.
// Recovery must only change the first byte of each verified directory entry.
static uint8_t sectors[4][512], original[4][512];
static RomEntryRecovery::Workspace recovery;
struct FakeDevice {
  unsigned reads = 0, writes = 0;
  unsigned fail_write_at = 0, change_at = 0;
  bool read_error = false, sync_error = false, bad_readback = false;
  bool readSector(uint32_t sector, uint8_t* data) {
    ++reads;
    if (read_error || sector >= 4) return false;
    if (change_at == reads) sectors[sector][32 + 20] ^= 1;
    memcpy(data, sectors[sector], 512);
    if (bad_readback && writes) data[400] ^= 1;
    return true;
  }
  bool writeSector(uint32_t sector, const uint8_t* data) {
    if (++writes == fail_write_at || sector >= 4) return false;
    memcpy(sectors[sector], data, 512);
    return true;
  }
  bool syncDevice() { return !sync_error; }
};
static void prepare_recovery() {
  memset(&recovery, 0, sizeof(recovery));
  memset(sectors, 0x5a, sizeof(sectors));
  auto& target = recovery.entries[0];
  target.sector = 2; target.offset = 32;
  memcpy(target.bytes, "POKEMO~1GBZ", 11);
  target.bytes[11] = 0x21; // archive + read-only: still a regular ROM
  target.bytes[26] = 0xff; // broken chain is intentionally never traversed
  const uint8_t sum = RomEntryRecovery::checksum(target.bytes);
  for (unsigned i = 1; i <= 2; ++i) {
    auto& entry = recovery.entries[i];
    entry.sector = i == 1 ? 2 : 1; entry.offset = i == 1 ? 0 : 480;
    entry.bytes[0] = i | (i == 2 ? 0x40 : 0);
    entry.bytes[11] = 0x0f; entry.bytes[13] = sum;
  }
  for (unsigned i = 0; i < 3; ++i) {
    const auto& e = recovery.entries[i];
    memcpy(sectors[e.sector] + e.offset, e.bytes, 32);
  }
  memcpy(original, sectors, sizeof(sectors));
}
static int recovery_tests() {
  prepare_recovery(); FakeDevice device;
  CHECK(RomEntryRecovery::erase(device, recovery, 3, 1, 4));
  CHECK(device.writes == 2);
  for (unsigned s = 0; s < 4; ++s) for (unsigned b = 0; b < 512; ++b) {
    const bool marker = (s == 2 && (b == 32 || b == 0)) || (s == 1 && b == 480);
    CHECK(sectors[s][b] == (marker ? 0xe5 : original[s][b]));
  }
  // An already-deleted, different, or invalid record cannot be erased again.
  const unsigned old_writes = device.writes;
  CHECK(!RomEntryRecovery::erase(device, recovery, 3, 1, 4));
  CHECK(device.writes == old_writes);
  for (unsigned fault = 0; fault < 12; ++fault) {
    prepare_recovery(); FakeDevice bad;
    unsigned count = 3;
    if (fault == 0) recovery.entries[0].offset = 33;
    if (fault == 1) recovery.entries[0].offset = 512;
    if (fault == 2) recovery.entries[0].sector = 0; // FAT sector forbidden
    if (fault == 3) recovery.entries[0].sector = 4;
    if (fault == 4) recovery.entries[0].bytes[11] = 0x10; // directory
    if (fault == 5) recovery.entries[0].bytes[8] = 'S'; // non-ROM/save
    if (fault == 6) recovery.entries[2].bytes[13] ^= 1; // unrelated LFN
    if (fault == 7) recovery.entries[2].bytes[0] = 2; // unterminated LFN
    if (fault == 8) sectors[2][33] ^= 1; // changed identity
    if (fault == 9) bad.read_error = true;
    if (fault == 10) count = 0;
    if (fault == 11) count = 22;
    CHECK(!RomEntryRecovery::erase(bad, recovery, count, 1, 4));
    CHECK(bad.writes == 0);
  }
  prepare_recovery(); FakeDevice changed; changed.change_at = 4;
  CHECK(!RomEntryRecovery::erase(changed, recovery, 3, 1, 4));
  CHECK(changed.writes == 0);
  prepare_recovery(); FakeDevice failed; failed.fail_write_at = 1;
  CHECK(!RomEntryRecovery::erase(failed, recovery, 3, 1, 4));
  CHECK(!memcmp(sectors, original, sizeof(sectors)));
  prepare_recovery(); FakeDevice partial; partial.fail_write_at = 2;
  CHECK(!RomEntryRecovery::erase(partial, recovery, 3, 1, 4));
  CHECK(sectors[2][32] == 0xe5 && !memcmp(sectors[0], original[0], 512));
  CHECK(!memcmp(sectors[1], original[1], 512) && !memcmp(sectors[3], original[3], 512));
  prepare_recovery(); FakeDevice sync; sync.sync_error = true;
  CHECK(!RomEntryRecovery::erase(sync, recovery, 3, 1, 4));
  prepare_recovery(); FakeDevice readback; readback.bad_readback = true;
  CHECK(!RomEntryRecovery::erase(readback, recovery, 3, 1, 4));
  prepare_recovery(); FakeDevice short_name;
  CHECK(RomEntryRecovery::erase(short_name, recovery, 1, 1, 4));
  CHECK(short_name.writes == 1 && sectors[2][32] == 0xe5 && sectors[2][0] == 1);
  return 0;
}

extern "C" int run_rom_file_actions_tests() {
  const int recovery_failure = recovery_tests();
  if (recovery_failure) return recovery_failure;
  char renamed[256];
  CHECK(renamed_leaf("Pokemon Crystal.GBZ", "  My Crystal  ", renamed));
  CHECK(!strcmp(renamed, "My Crystal.GBZ"));
  CHECK(renamed_leaf("CRYSTALR.GBZ", "Pokemon - Crystal Version", renamed));
  CHECK(!strcmp(renamed, "Pokemon - Crystal Version.GBZ"));
  CHECK(renamed_leaf("Yellow.gb", "Pokemon Yellow (USA) - Rev 1", renamed));
  CHECK(!strcmp(renamed, "Pokemon Yellow (USA) - Rev 1.gb"));
  CHECK(!renamed_leaf("Yellow.gb", "", renamed));
  CHECK(!renamed_leaf("Yellow.gb", "  ", renamed));
  CHECK(!renamed_leaf("Yellow.gb", "../other", renamed));
  CHECK(!renamed_leaf("Yellow.gb", "Bad.", renamed));
  CHECK(!renamed_leaf("Yellow.gb", "CON", renamed));
  const char* invalid[] = {nullptr, "", ".", "..", "a.", "a.g", "a.gbcx", "a.sav",
      "../Mario.gb", "/Mario.gb", "folder/Mario.gb", "folder\\Mario.gb", "D:Mario.gb",
      "x\n.gb", "x*.gb", "x?.gb", "x|.gb", "x<.gb", "x>.gb", "x\".gb", ".gb",
      "CON.gb", "PRN.gb", "AUX.gb", "NUL.gb", "COM1.gb", "LPT9.gbc", "Mario.gb "};
  for (const char* name : invalid) {
    reset_files();
    CHECK(!valid_rom(name));
    CHECK(action(Action::REMOVE, name) == Result::INVALID_NAME);
    CHECK(opens == 0 && mutations == 0);
  }
  CHECK(valid_rom("game.Gb"));
  CHECK(valid_rom("game.GbC"));
  CHECK(valid_rom("game.GbZ"));
  CHECK(valid_rom("Pok\xC3\xA9mon.gb"));
  char long_name[256];
  memset(long_name, 'a', 252); long_name[252] = 0;
  CHECK(!renamed_leaf("x.gbz", long_name, renamed));
  long_name[251] = 0;
  CHECK(renamed_leaf("x.gbz", long_name, renamed));
  CHECK(strlen(renamed) == 255);
  memset(long_name, 'x', sizeof(long_name));
  CHECK(!valid_rom(long_name));

  reset_files();
  CHECK(action(Action::REMOVE, "folder.gb") == Result::INVALID_NAME);
  CHECK(action(Action::REMOVE, "missing.gb") == Result::NOT_FOUND);
  CHECK(mutations == 0);
  entries[0].read_only = true;
  CHECK(action(Action::REMOVE, entries[0].name) == Result::READ_ONLY);
  CHECK(action(Action::RENAME, entries[0].name, "New.gbz") == Result::READ_ONLY);
  CHECK(mutations == 0);
  CHECK(action(Action::FORCE_REMOVE, entries[0].name) == Result::OK);
  CHECK(mutations == 1 && !entries[0].present);
  for (unsigned i = 1; i < 5; ++i) CHECK(entries[i].present && entries[i].data == 100 + i);
  reset_files();
  CHECK(action(Action::FORCE_REMOVE, entries[0].name) == Result::UNCHANGED);
  entries[0].read_only = true;
  fail_mutation = true;
  CHECK(action(Action::FORCE_REMOVE, entries[0].name) == Result::SD_ERROR);
  CHECK(entries[0].present && entries[0].read_only && mutations == 0);
  reset_files();
  CHECK(action(Action::RENAME, entries[0].name, "bad.sav") == Result::INVALID_NAME);
  CHECK(action(Action::RENAME, entries[0].name, "bad.gbc") == Result::INVALID_NAME);
  CHECK(action(Action::RENAME, "Mario.GBC", "mario.gbc") == Result::UNCHANGED);
  CHECK(opens == 0 && mutations == 0);
  // Duplicate file/directory names must never be overwritten.
  memcpy(entries[1].name, "Existing.GBZ", 13);
  CHECK(action(Action::RENAME, entries[0].name, "existing.gbz") == Result::EXISTS);
  entries[1].directory = true;
  CHECK(action(Action::RENAME, entries[0].name, "Existing.GBZ") == Result::EXISTS);
  CHECK(mutations == 0);

  reset_files(); fail_root = true;
  CHECK(action(Action::REMOVE, entries[0].name) == Result::SD_ERROR);
  reset_files(); fail_write = true;
  CHECK(action(Action::REMOVE, entries[0].name) == Result::SD_ERROR);
  reset_files(); fail_mutation = true;
  CHECK(action(Action::REMOVE, entries[0].name) == Result::DELETE_FAILED);
  CHECK(action(Action::RENAME, entries[0].name, "New.gbz") == Result::SD_ERROR);
  CHECK(mutations == 0);

  reset_files();
  CHECK(action(Action::RENAME, "Pokemon Crystal.gbz", "A Crystal.gbz") == Result::OK);
  CHECK(mutations == 1 && entries[0].data == 100 && entries[0].present);
  CHECK(!strcmp(entries[0].name, "A Crystal.gbz"));
  CHECK(action(Action::REMOVE, "A Crystal.gbz") == Result::OK);
  CHECK(mutations == 2 && !entries[0].present);
  for (unsigned i = 1; i < 5; ++i) CHECK(entries[i].present && entries[i].data == 100 + i);
  CHECK(!strcmp(entries[2].name, "POKEMON CRYSTAL"));
  CHECK(!strcmp(entries[3].name, "SETTINGS.BIN"));
  return 0;
}
