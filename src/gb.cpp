#include <Arduino.h>
#include <pico/platform.h>

#include "minigb_apu.h"
#include "peanut_gb.h"
#include <uzlib.h>

#include "gbcolors.h"
#include "gb.h"
#include "gbz_format.h"
#include "common.h"

struct gb_s gb;
palette_t palette; // Colour palette

uint8_t ram[RAM_SIZE];

static bool cart_ram_dirty = false;
static uint64_t cart_ram_last_write_us = 0;
static constexpr uint64_t CART_RAM_AUTOSAVE_DELAY_US = 3000000;

// Definition of ROM data
#if !ENABLE_SDCARD
#include "game_bin.h"
const uint8_t *rom = GAME_DATA;
#else
/** Definition of ROM data
 * We're going to erase and reprogram the region defines as "Filesystem" in platformio.ini (see board_build.filesystem_size).
 * This is available from _FS_start (i.e. XIP_BASE + program size) to _FS_end. Note that the last sector is reserved for EEPROM.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
const uint8_t *rom = (const uint8_t *)(&_FS_start);
#endif

// Keep the first four ROM banks in SRAM. This is the proven layout for this
// build: it avoids a visible frame hitch when games rapidly switch banks.
static uint8_t rom_bank0[0x10000];

#if ENABLE_SDCARD
static constexpr uint8_t GBZ_CACHE_SLOTS =
    sizeof(rom_bank0) / GBZ_BANK_SIZE;
static bool gbz_active = false;
static const GbzHeader* gbz_header = nullptr;
static const uint8_t* gbz_bank_ptr[GBZ_MAX_BANKS];
static const uint8_t* gbz_fast_raw_bank_ptr[GBZ_MAX_BANKS];
static int8_t gbz_cached_slot[GBZ_MAX_BANKS];
static uint16_t gbz_slot_bank[GBZ_CACHE_SLOTS];
static uint32_t gbz_slot_age[GBZ_CACHE_SLOTS];
static uint32_t gbz_age = 0;
static uint8_t gbz_cache_first_slot = 1;
static uint16_t gbz_last_compressed_bank = UINT16_MAX;
static TINF_DATA gbz_inflater;

static bool gbz_lz4_decompress(const uint8_t* source, uint16_t source_size,
    uint8_t* destination) {
  const uint8_t* input = source;
  const uint8_t* input_end = source + source_size;
  uint8_t* output = destination;
  uint8_t* output_end = destination + GBZ_BANK_SIZE;

  while (input < input_end) {
    const uint8_t token = *input++;
    uint32_t literal_length = token >> 4;
    if (literal_length == 15) {
      uint8_t extension;
      do {
        if (input >= input_end) return false;
        extension = *input++;
        literal_length += extension;
      } while (extension == 255);
    }
    if (literal_length > (uint32_t)(input_end - input) ||
        literal_length > (uint32_t)(output_end - output)) return false;
    memcpy(output, input, literal_length);
    input += literal_length;
    output += literal_length;

    // A final literal-only sequence ends exactly at the input boundary.
    if (input == input_end) return output == output_end;
    if (input_end - input < 2) return false;
    const uint16_t offset = input[0] | ((uint16_t)input[1] << 8);
    input += 2;
    if (offset == 0 || offset > (uint32_t)(output - destination)) return false;

    uint32_t match_length = (token & 0x0F) + 4u;
    if ((token & 0x0F) == 15) {
      uint8_t extension;
      do {
        if (input >= input_end) return false;
        extension = *input++;
        match_length += extension;
      } while (extension == 255);
    }
    if (match_length > (uint32_t)(output_end - output)) return false;
    uint8_t* match = output - offset;
    while (match_length--) *output++ = *match++;
  }
  return output == output_end;
}

static bool gbz_header_valid(const GbzHeader* header) {
  if (header->magic != GBZ_MAGIC || header->version != GBZ_VERSION ||
      header->header_size != GBZ_HEADER_SIZE ||
      header->bank_size != GBZ_BANK_SIZE || header->bank_count == 0 ||
      header->bank_count > GBZ_MAX_BANKS ||
      header->rom_size != (uint32_t)header->bank_count * GBZ_BANK_SIZE ||
      header->payload_size > (uint32_t)MAX_ROM_SIZE - GBZ_HEADER_SIZE) {
    return false;
  }

  const uint32_t container_end = GBZ_HEADER_SIZE + header->payload_size;
  for (uint16_t bank = 0; bank < header->bank_count; ++bank) {
    const GbzBankEntry& entry = header->banks[bank];
    if (entry.offset < GBZ_HEADER_SIZE || entry.stored_size == 0 ||
        entry.stored_size > GBZ_BANK_SIZE ||
        (entry.flags != 0 && entry.flags != GBZ_FLAG_DEFLATE &&
         entry.flags != GBZ_FLAG_LZ4) ||
        (entry.flags == 0 && entry.stored_size != GBZ_BANK_SIZE) ||
        entry.offset > container_end ||
        entry.stored_size > container_end - entry.offset) {
      return false;
    }
  }
  return true;
}

static bool gbz_load_bank(uint16_t bank, uint8_t& slot_out) {
  if (bank >= gbz_header->bank_count) return false;

  const uint8_t* mapped = gbz_bank_ptr[bank];
  if (mapped) {
    const int8_t mapped_slot = gbz_cached_slot[bank];
    if (mapped_slot < 0) return false;
    slot_out = (uint8_t)mapped_slot;
    if (bank != gbz_last_compressed_bank) {
      gbz_slot_age[slot_out] = ++gbz_age;
      gbz_last_compressed_bank = bank;
    }
    return true;
  }

  uint8_t slot = gbz_cache_first_slot;
  for (uint8_t candidate = gbz_cache_first_slot;
       candidate < GBZ_CACHE_SLOTS; ++candidate) {
    if (gbz_slot_bank[candidate] == UINT16_MAX) {
      slot = candidate;
      break;
    }
    if (gbz_slot_age[candidate] < gbz_slot_age[slot]) slot = candidate;
  }

  if (gbz_slot_bank[slot] != UINT16_MAX) {
    const uint16_t evicted_bank = gbz_slot_bank[slot];
    gbz_cached_slot[evicted_bank] = -1;
    const GbzBankEntry& evicted = gbz_header->banks[evicted_bank];
    // A raw bank can immediately fall back to direct XIP after its optional
    // SRAM acceleration slot is reused. Compressed banks must be paged again.
    gbz_bank_ptr[evicted_bank] =
        evicted.flags == 0 ? rom + evicted.offset : nullptr;
  }

  const GbzBankEntry& entry = gbz_header->banks[bank];
  uint8_t* destination = rom_bank0 + (uint32_t)slot * GBZ_BANK_SIZE;
  const uint8_t* source = rom + entry.offset;
  if (entry.flags == 0) {
    memcpy(destination, source, GBZ_BANK_SIZE);
  } else if (entry.flags == GBZ_FLAG_DEFLATE) {
    memset(&gbz_inflater, 0, sizeof(gbz_inflater));
    gbz_inflater.source = source;
    gbz_inflater.source_limit = source + entry.stored_size;
    gbz_inflater.dest_start = destination;
    gbz_inflater.dest = destination;
    gbz_inflater.dest_limit = destination + GBZ_BANK_SIZE;
    uzlib_uncompress_init(&gbz_inflater, nullptr, 0);
    const int status = uzlib_uncompress(&gbz_inflater);
    if ((status != TINF_OK && status != TINF_DONE) ||
        gbz_inflater.dest != destination + GBZ_BANK_SIZE) {
      Serial.printf("E GBZ bank %u inflate failed (%d, %u bytes)\n",
          bank, status, (unsigned)(gbz_inflater.dest - destination));
      return false;
    }
  } else if (!gbz_lz4_decompress(source, entry.stored_size, destination)) {
    Serial.printf("E GBZ bank %u LZ4 decode failed\n", bank);
    return false;
  }

  gbz_slot_bank[slot] = bank;
  gbz_bank_ptr[bank] = destination;
  gbz_cached_slot[bank] = slot;
  gbz_slot_age[slot] = ++gbz_age;
  gbz_last_compressed_bank = bank;
  slot_out = slot;
  return true;
}
#endif

/**
 * Returns a byte from the ROM file at the given address.
 */
static uint8_t __time_critical_func(gb_rom_read)(struct gb_s* gb,
    const uint_fast32_t addr) {
  (void)gb;
#if ENABLE_SDCARD
  if (gbz_active) {
    const uint16_t bank = addr / GBZ_BANK_SIZE;
    if (bank >= gbz_header->bank_count) return 0xFF;
    const uint16_t bank_offset = addr & (GBZ_BANK_SIZE - 1);

    // One pointer lookup handles both raw XIP banks and decompressed cache
    // banks. This keeps the per-byte callback close to the normal ROM path.
    const uint8_t* bank_data = gbz_bank_ptr[bank];
    if (bank_data) {
      const int8_t cached_slot = gbz_cached_slot[bank];
      if (cached_slot >= 0 && bank != gbz_last_compressed_bank) {
        const uint8_t slot = (uint8_t)cached_slot;
        gbz_slot_age[slot] = ++gbz_age;
        gbz_last_compressed_bank = bank;
      }
      return bank_data[bank_offset];
    }

    uint8_t slot = 0;
    if (!gbz_load_bank(bank, slot)) {
      return 0xFF;
    }
    return gbz_bank_ptr[bank][bank_offset];
  }
#endif
  // The first four banks are hot and live in SRAM. Larger games such as
  // Pokemon switch to later banks, which must be read directly from XIP flash
  // rather than indexing past this 64 KiB buffer.
  return addr < sizeof(rom_bank0) ? rom_bank0[addr] : rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
static uint8_t gb_cart_ram_read(struct gb_s* gb, const uint_fast32_t addr) {
  (void)gb;
  return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
static void gb_cart_ram_write(struct gb_s* gb, const uint_fast32_t addr,
    const uint8_t val) {
  if (ram[addr] != val) {
    ram[addr] = val;
    cart_ram_dirty = true;
    cart_ram_last_write_us = time_us_64();
  }
}

bool cartRamSaveDue() {
  return cart_ram_dirty &&
      (time_us_64() - cart_ram_last_write_us >= CART_RAM_AUTOSAVE_DELAY_US);
}

bool compressedCartridgeActive() {
#if ENABLE_SDCARD
  return gbz_active;
#else
  return false;
#endif
}

// Keep the frame scheduler beside Peanut-GB's hot CPU stepper in SRAM.  A
// flash-resident loop otherwise crosses an XIP-to-RAM veneer once per
// emulated instruction, which is especially costly during CGB overworld play.
void __time_critical_func(runGbFrameFast)() {
  // During scrolling, core 1 can still be transmitting the previous scaled
  // frame when Peanut-GB reaches its next render pass. lcd_draw_line() would
  // reject that completed frame at line 143, so avoid generating all 144
  // lines in the first place. CPU/timer/APU execution remains untouched.
  auto draw_line = gb.display.lcd_draw_line;
  const bool lcd_available = draw_line != nullptr && lcd_frame_available();

  const bool suppress_lcd = draw_line != nullptr && !lcd_available;
  if (suppress_lcd) gb.display.lcd_draw_line = nullptr;

  gb.gb_frame = false;
  while (!gb.gb_frame) {
    __gb_step_cpu(&gb);
  }

  if (suppress_lcd) gb.display.lcd_draw_line = draw_line;
}

void cartRamMarkSaved() {
  cart_ram_dirty = false;
}

static void gb_error(struct gb_s* gb, const enum gb_error_e gb_err, const uint16_t addr) {
  const char* gb_err_str[4] = {
      "UNKNOWN",
      "INVALID OPCODE",
      "INVALID READ",
      "INVALID WRITE"};
  error(String("Error ") + gb_err + " occurred: " + gb_err_str[gb_err] + " at 0x" + String(addr, 16));
}

#ifdef USE_BOOT_ROM
#include "boot_cgb_bin.h"
#include "boot_gb_bin.h"
static uint8_t gb_bootrom_read(struct gb_s* gb, const uint_fast16_t addr) {
	if (gb->cgb.cgbMode) {
    return CGB_BOOT_ROM[addr];
  } else {
    return GB_BOOT_ROM[addr];
  }
}
#endif

void initGbContext() {
  cartRamMarkSaved();
#if ENABLE_SDCARD
  gbz_header = reinterpret_cast<const GbzHeader*>(rom);
  gbz_active = gbz_header_valid(gbz_header);
  if (gbz_active) {
    memset(gbz_bank_ptr, 0, sizeof(gbz_bank_ptr));
    memset(gbz_fast_raw_bank_ptr, 0, sizeof(gbz_fast_raw_bank_ptr));
    memset(gbz_cached_slot, -1, sizeof(gbz_cached_slot));
    for (uint8_t slot = 0; slot < GBZ_CACHE_SLOTS; ++slot) {
      gbz_slot_bank[slot] = UINT16_MAX;
      gbz_slot_age[slot] = 0;
    }
    gbz_age = 0;
    gbz_last_compressed_bank = UINT16_MAX;
    uzlib_init();

    for (uint16_t bank = 0; bank < gbz_header->bank_count; ++bank) {
      const GbzBankEntry& entry = gbz_header->banks[bank];
      if (entry.flags == 0) {
        gbz_bank_ptr[bank] = rom + entry.offset;
        gbz_fast_raw_bank_ptr[bank] = rom + entry.offset;
      }
    }

    // A compressed bank zero never leaves slot zero. Fast-profile containers
    // keep bank zero raw in XIP, making all four SRAM slots available to cold
    // compressed banks while retaining the original 64 KiB footprint.
    const GbzBankEntry& bank0 = gbz_header->banks[0];
    gbz_cache_first_slot = bank0.flags == 0 ? 0 : 1;
    bool bank0_ok = bank0.flags == 0;
    if (bank0.flags == 0) {
      // Served directly from flash by gb_rom_read().
    } else if (bank0.flags == GBZ_FLAG_DEFLATE) {
      memset(&gbz_inflater, 0, sizeof(gbz_inflater));
      gbz_inflater.source = rom + bank0.offset;
      gbz_inflater.source_limit = gbz_inflater.source + bank0.stored_size;
      gbz_inflater.dest_start = rom_bank0;
      gbz_inflater.dest = rom_bank0;
      gbz_inflater.dest_limit = rom_bank0 + GBZ_BANK_SIZE;
      uzlib_uncompress_init(&gbz_inflater, nullptr, 0);
      const int status = uzlib_uncompress(&gbz_inflater);
      bank0_ok = (status == TINF_OK || status == TINF_DONE) &&
          gbz_inflater.dest == rom_bank0 + GBZ_BANK_SIZE;
    } else {
      bank0_ok = gbz_lz4_decompress(rom + bank0.offset,
          bank0.stored_size, rom_bank0);
    }
    if (!bank0_ok) error("Compressed cartridge bank 0 is corrupt");
    if (bank0.flags != 0) {
      gbz_slot_bank[0] = 0;
      gbz_bank_ptr[0] = rom_bank0;
      gbz_cached_slot[0] = 0;
      gbz_slot_age[0] = ++gbz_age;
    }

    Serial.printf("I GBZ cartridge: %lu bytes, %u banks\n",
        (unsigned long)gbz_header->rom_size, gbz_header->bank_count);
  } else {
    memcpy(rom_bank0, rom, sizeof(rom_bank0));
  }
#else
  memcpy(rom_bank0, rom, sizeof(rom_bank0));
#endif

  auto ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, NULL);
  if (ret != GB_INIT_NO_ERROR) {
    error(String("Error initializing emulator: ") + ret);
  }

#if ENABLE_SDCARD
  // Only raw XIP banks bypass the callback. Compressed cache hits continue
  // through gb_rom_read() so its LRU ages remain correct across bank changes.
  if (gbz_active) {
    gb.fast_rom_banks = gbz_fast_raw_bank_ptr;
    __gb_refresh_fast_selected_rom_bank(&gb);
  }
#endif

#if ENABLE_SDCARD
  // Full CGB rendering is the dominant RP2040 cost in active overworld scenes.
  // Peanut-GB's native frame skip omits alternate pixel-render passes while
  // preserving CPU, timer, input, save, and APU execution at the original rate.
  gb.direct.frame_skip = gbz_active;
#endif

#ifdef USE_BOOT_ROM
  // gb_bootrom_read has to be set after gb_init(), as it sets it to NULL
  gb_set_bootrom(&gb, &gb_bootrom_read);
  gb_reset(&gb);
#endif
}
