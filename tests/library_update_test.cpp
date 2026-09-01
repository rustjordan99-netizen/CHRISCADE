// Reuse the freestanding libc and existing safe-file-action regression suite.
#include "rom_file_actions_test.cpp"
#include "../src/rom_ui_helpers.h"
#include "../src/rom_upload_protocol.h"

extern "C" { uint8_t mushroom_preview[37 * 37 + 13 * 13]; }

extern "C" void* memmove(void* to, const void* from, size_t n) {
  auto* d = static_cast<unsigned char*>(to);
  const auto* s = static_cast<const unsigned char*>(from);
  if (d > s) { while (n) { --n; d[n] = s[n]; } }
  else { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
  return to;
}
extern "C" int strncmp(const char* a, const char* b, size_t n) {
  while (n--) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; ++a; ++b; }
  return 0;
}

static int ui_tests() {
  unsigned preview_offset = 0;
  const int mushroom_sizes[] = {18, 6};
  for (int size : mushroom_sizes) {
    for (int y = -size; y <= size; ++y) {
      for (int x = -size; x <= size; ++x) {
        const auto pixel = mushroom_pixel(x, y, size);
        CHECK(pixel == mushroom_pixel(-x, y, size));
        mushroom_preview[preview_offset++] = (uint8_t)pixel;
      }
    }
    CHECK(mushroom_pixel(0, -size / 2, size) == MushroomPixel::WHITE);
    CHECK(mushroom_pixel(size * 2 / 3, -size / 6, size) == MushroomPixel::WHITE);
    CHECK(mushroom_pixel(-size * 2 / 3, -size / 6, size) == MushroomPixel::WHITE);
    CHECK(mushroom_pixel(0, 0, size) == (size < 9 ? MushroomPixel::OUTLINE : MushroomPixel::RED));
    CHECK(mushroom_pixel(size, -size, size) == MushroomPixel::CLEAR);
  }
  char text[32] = "Pokemon Yellow";
  RomNameEditor editor(text, sizeof(text) - 1);
  editor.place(8);
  CHECK(editor.insert('X') && !strcmp(text, "Pokemon XYellow"));
  editor.backspace();
  CHECK(!strcmp(text, "Pokemon Yellow") && editor.cursor == 8);
  editor.place(0); editor.backspace(); editor.left();
  CHECK(!strcmp(text, "Pokemon Yellow") && editor.cursor == 0);
  editor.place(999); editor.right();
  CHECK(editor.cursor == editor.length);
  editor.clear();
  CHECK(editor.cursor == 0 && editor.length == 0 && !*text);
  memcpy(text, "Pok\xC3\xA9mon", 9);
  RomNameEditor utf(text, sizeof(text) - 1);
  utf.place(4); CHECK(utf.cursor == 3);
  utf.right(); CHECK(utf.cursor == 5);
  utf.backspace(); CHECK(!strcmp(text, "Pokmon") && utf.cursor == 3);
  char small[5] = "ABCD";
  RomNameEditor full(small, 4);
  full.place(2); CHECK(!full.insert('X') && !strcmp(small, "ABCD"));
  full.backspace(); CHECK(full.insert('X') && !strcmp(small, "AXCD"));
  const int radii[] = {4, 13};
  for (int radius : radii) {
    for (int y = -radius - 2; y <= radius + 2; ++y) {
      for (int x = -radius - 2; x <= radius + 2; ++x) {
        const auto pixel = pokeball_pixel(x, y, radius);
        CHECK(pixel == pokeball_pixel(-x, y, radius));
        CHECK((pixel == PokeballPixel::CLEAR) == (x*x + y*y > radius*radius));
        if (pixel == PokeballPixel::RED) CHECK(y < 0);
        if (x*x + y*y == radius*radius) CHECK(pixel == PokeballPixel::OUTLINE);
      }
    }
    CHECK(pokeball_pixel(radius, -radius, radius) == PokeballPixel::CLEAR);
    CHECK(pokeball_pixel(0, radius, radius) == PokeballPixel::OUTLINE);
    CHECK(pokeball_pixel(0, 0, radius) == PokeballPixel::WHITE);
  }
  return 0;
}

static int scroll_tests() {
  // Sorted synthetic library, no SD reads. Test every small library size,
  // including exactly eight entries, multiple windows and a short final one.
  char names[8][16] = {}, next[16];
  for (unsigned total = 0; total <= 30; ++total) {
    RomLibraryWindow window;
    window.count = total < 8 ? total : 8;
    window.add_selected = !total;
    for (unsigned i = 0; i < window.count; ++i) { names[i][0] = 'A' + i; names[i][1] = 0; }
    auto find = [&](const char* anchor, char* out, bool back) {
      int index = anchor[0] - 'A' + (back ? -1 : 1);
      if (index < 0 || index >= (int)total) return false;
      out[0] = 'A' + index; out[1] = 0; return true;
    };
    if (!total) {
      CHECK(window.move(false, names, next, find) == RomScrollChange::NONE);
      CHECK(window.move(true, names, next, find) == RomScrollChange::NONE);
      continue;
    }
    CHECK(window.move(true, names, next, find) == RomScrollChange::NONE);
    for (unsigned rank = 0; rank < total; ++rank) {
      CHECK(!window.add_selected && window.first + window.selected == rank);
      CHECK(names[window.selected][0] == 'A' + rank);
      CHECK(window.move(false, names, next, find) != RomScrollChange::NONE);
    }
    CHECK(window.add_selected);
    CHECK(window.move(false, names, next, find) == RomScrollChange::NONE);
    CHECK(window.move(true, names, next, find) == RomScrollChange::FOOTER);
    for (int rank = total - 1; rank >= 0; --rank) {
      CHECK(!window.add_selected && window.first + window.selected == rank);
      CHECK(names[window.selected][0] == 'A' + rank);
      auto result = window.move(true, names, next, find);
      CHECK(rank ? result != RomScrollChange::NONE : result == RomScrollChange::NONE);
    }
  }
  // Returning after deleting the last file can create a short shifted window.
  RomLibraryWindow short_window;
  short_window.first = 10; short_window.count = 1;
  memcpy(names[0], "K", 2);
  auto previous = [](const char* a, char* out, bool back) {
    if (!back || a[0] <= 'A') return false;
    out[0] = a[0] - 1; out[1] = 0; return true;
  };
  for (int rank = 9; rank >= 0; --rank) {
    CHECK(short_window.move(true, names, next, previous) == RomScrollChange::WINDOW);
    CHECK(short_window.first == rank && names[0][0] == 'A' + rank);
  }
  return 0;
}

struct UploadEntry { char name[32]; bool present; unsigned writes; };
static UploadEntry upload_entries[6];
static bool short_write, sync_error, rename_error;
struct UploadFile {
  bool root = false;
  int index = -1;
  bool open(const char* name, int) { root = !strcmp(name, "/"); return root; }
  bool open(UploadFile* parent, const char* name, int flags) {
    if (!parent->root) return false;
    for (int i = 0; i < 6; ++i) if (upload_entries[i].present && same_name(upload_entries[i].name, name)) {
      if (flags == 14) return false; // O_EXCL
      index = i; return true;
    }
    if (flags != 14) return false;
    for (int i = 0; i < 6; ++i) if (!upload_entries[i].present) {
      memcpy(upload_entries[i].name, name, strlen(name) + 1);
      upload_entries[i].present = true;
      upload_entries[i].writes = 0;
      index = i; return true;
    }
    return false;
  }
  size_t write(const uint8_t*, size_t n) {
    if (index < 0) return 0;
    upload_entries[index].writes += n;
    return short_write && n ? n - 1 : n;
  }
  bool sync() { return !sync_error; }
  bool rename(UploadFile* parent, const char* name) {
    if (rename_error || !parent->root || index < 0) return false;
    for (const auto& e : upload_entries) if (e.present && same_name(e.name, name)) return false;
    memcpy(upload_entries[index].name, name, strlen(name) + 1); return true;
  }
  bool remove() { if (index < 0) return false; upload_entries[index].present = false; index = -1; return true; }
  void close() { root = false; index = -1; }
};

static int upload_tests() {
  using namespace RomUpload;
  uint32_t size, crc;
  char name[256];
  CHECK(parse_header("CCROM1 9 CBF43926 4e65772e6762", size, crc, name));
  CHECK(size == 9 && crc == 0xCBF43926 && !strcmp(name, "New.gb"));
  const char* invalid[] = {"", "CCROM1", "CCROM1 9", "CCROM1 0 00000000 412e6762",
    "CCROM1 16777217 00000000 412e6762", "CCROM1 999999999999999999 00000000 412e6762",
    "CCROM1 9 Z0000000 412e6762", "CCROM1 9 00000000 4", "CCROM1 9 00000000 00",
    "CCROM1 9 00000000 2e2e2f412e6762", "CCROM1 9 00000000 412e736176",
    "CCROM1 9 00000000 434f4e2e6762", "CCROM1 9 00000000 412f422e6762"};
  for (const char* line : invalid) CHECK(!parse_header(line, size, crc, name));
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>("123456789");
  CHECK((crc_update(crc_update(0xFFFFFFFFu, bytes, 4), bytes + 4, 5) ^ 0xFFFFFFFFu) == 0xCBF43926);
  const char* originals[] = {"Old.gb", "Old.sav", "settings.cfg", "CCEXIST.TMP", "Folder.gbc"};
  for (int i = 0; i < 5; ++i) {
    memcpy(upload_entries[i].name, originals[i], strlen(originals[i]) + 1);
    upload_entries[i].present = true; upload_entries[i].writes = 1234;
  }
  for (int scenario = 0; scenario < 8; ++scenario) {
    short_write = sync_error = rename_error = false;
    upload_entries[5].present = false;
    Receiver<UploadFile> receiver;
    CHECK(!receiver.begin("Old.gb", "CCNEW.TMP", 1, 14));
    CHECK(!receiver.begin("New.gb", "CCEXIST.TMP", 1, 14));
    CHECK(!receiver.begin("Old.sav", "CCNEW.TMP", 1, 14));
    CHECK(!receiver.begin("../New.gb", "CCNEW.TMP", 1, 14));
    CHECK(!receiver.begin("New.gb", "Old.gb", 1, 14));
    CHECK(receiver.begin("New.gb", "CCNEW.TMP", 1, 14));
    short_write = scenario == 1;
    const bool wrote = receiver.write(bytes, 9, 9);
    CHECK(wrote == !short_write);
    CHECK(!receiver.write(bytes, 10, 9));
    CHECK(!receiver.write(bytes, 513, 1024));
    sync_error = scenario == 2;
    rename_error = scenario == 3;
    const bool committed = receiver.commit(scenario == 6 ? "Old.gb" : "New.gb",
        scenario == 4 ? 10 : 9, scenario == 5 ? 0 : 0xCBF43926);
    CHECK(committed == (scenario == 0 || scenario == 7));
    receiver.cancel();
    CHECK(upload_entries[5].present == committed);
    if (committed) CHECK(!strcmp(upload_entries[5].name, "New.gb"));
    for (int i = 0; i < 5; ++i) {
      CHECK(upload_entries[i].present && upload_entries[i].writes == 1234);
      CHECK(!strcmp(upload_entries[i].name, originals[i]));
    }
  }
  return 0;
}

extern "C" int run_library_update_tests() {
  int result = run_rom_file_actions_tests();
  if (result) return result;
  if ((result = ui_tests())) return result;
  if ((result = scroll_tests())) return result;
  return upload_tests();
}
