#pragma once

#include <stdint.h>

// CHRISCADE compressed Game Boy ROM container. Each cartridge bank is stored
// independently so the emulator can inflate only the bank currently selected
// by the MBC without ever holding the complete ROM in SRAM.
static constexpr uint32_t GBZ_MAGIC = 0x315A4247u; // "GBZ1"
static constexpr uint16_t GBZ_VERSION = 1;
static constexpr uint16_t GBZ_HEADER_SIZE = 4096;
static constexpr uint16_t GBZ_BANK_SIZE = 16384;
static constexpr uint16_t GBZ_MAX_BANKS = 128;
static constexpr uint16_t GBZ_FLAG_DEFLATE = 1u;
static constexpr uint16_t GBZ_FLAG_LZ4 = 2u;

struct __attribute__((packed)) GbzBankEntry {
  uint32_t offset;
  uint16_t stored_size;
  uint16_t flags;
  uint32_t crc32;
};

struct __attribute__((packed)) GbzHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t rom_size;
  uint16_t bank_size;
  uint16_t bank_count;
  uint32_t rom_crc32;
  uint32_t payload_size;
  uint32_t header_crc32;
  uint32_t reserved;
  GbzBankEntry banks[GBZ_MAX_BANKS];
  uint8_t padding[GBZ_HEADER_SIZE - 32 -
      GBZ_MAX_BANKS * sizeof(GbzBankEntry)];
};

static_assert(sizeof(GbzBankEntry) == 12, "Unexpected GBZ bank entry size");
static_assert(sizeof(GbzHeader) == GBZ_HEADER_SIZE, "GBZ header must fill one flash sector");
