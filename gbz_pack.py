#!/usr/bin/env python3
"""Pack a legally obtained GB/GBC ROM into CHRISCADE's banked GBZ format."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib


MAGIC = 0x315A4247  # GBZ1
VERSION = 1
HEADER_SIZE = 4096
BANK_SIZE = 16384
MAX_BANKS = 128
FLAG_DEFLATE = 1
FLAG_LZ4 = 2
HEADER_FORMAT = "<IHHIHHIIII"
ENTRY_FORMAT = "<IHHI"


def _lz4_length(output: bytearray, length: int) -> None:
    while length >= 255:
        output.append(255)
        length -= 255
    output.append(length)


def lz4_compress(block: bytes) -> bytes:
    """Create a raw LZ4 block using a small deterministic hash-table encoder."""
    output = bytearray()
    table: list[list[int]] = [[] for _ in range(16384)]
    anchor = 0
    position = 0
    limit = len(block) - 4

    def hash_at(offset: int) -> int:
        sequence = int.from_bytes(block[offset : offset + 4], "little")
        return ((sequence * 2654435761) & 0xFFFFFFFF) >> 18

    def remember(offset: int) -> None:
        bucket = table[hash_at(offset)]
        bucket.append(offset)
        if len(bucket) > 8:
            del bucket[0]

    while position <= limit:
        hash_value = hash_at(position)
        best_reference = -1
        best_match_end = position
        for reference in reversed(table[hash_value]):
            if block[reference : reference + 4] != block[position : position + 4]:
                continue
            candidate_end = position + 4
            reference_end = reference + 4
            while (
                candidate_end < len(block)
                and block[candidate_end] == block[reference_end]
            ):
                candidate_end += 1
                reference_end += 1
            if candidate_end > best_match_end:
                best_reference = reference
                best_match_end = candidate_end

        remember(position)
        if best_reference < 0:
            position += 1
            continue

        literal_length = position - anchor
        match_length = best_match_end - position - 4
        output.append((min(literal_length, 15) << 4) | min(match_length, 15))
        if literal_length >= 15:
            _lz4_length(output, literal_length - 15)
        output.extend(block[anchor:position])
        output.extend(struct.pack("<H", position - best_reference))
        if match_length >= 15:
            _lz4_length(output, match_length - 15)

        for update in range(position + 1, best_match_end):
            if update <= limit:
                remember(update)
        position = best_match_end
        anchor = best_match_end

    literal_length = len(block) - anchor
    output.append(min(literal_length, 15) << 4)
    if literal_length >= 15:
        _lz4_length(output, literal_length - 15)
    output.extend(block[anchor:])
    return bytes(output)


def lz4_decompress(block: bytes) -> bytes:
    output = bytearray()
    position = 0
    while position < len(block):
        token = block[position]
        position += 1
        literal_length = token >> 4
        if literal_length == 15:
            while True:
                extension = block[position]
                position += 1
                literal_length += extension
                if extension != 255:
                    break
        output.extend(block[position : position + literal_length])
        position += literal_length
        if position == len(block):
            break
        offset = struct.unpack_from("<H", block, position)[0]
        position += 2
        match_length = (token & 15) + 4
        if (token & 15) == 15:
            while True:
                extension = block[position]
                position += 1
                match_length += extension
                if extension != 255:
                    break
        for _ in range(match_length):
            output.append(output[-offset])
    return bytes(output)


def parse_bank_list(specification: str) -> set[int]:
    banks: set[int] = set()
    if not specification:
        return banks
    for item in specification.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            first_text, last_text = item.split("-", 1)
            first = int(first_text, 0)
            last = int(last_text, 0)
            if last < first:
                raise ValueError(f"invalid descending bank range: {item}")
            banks.update(range(first, last + 1))
        else:
            banks.add(int(item, 0))
    return banks


def pack_rom(
    source: pathlib.Path,
    destination: pathlib.Path,
    raw_banks: set[int],
    codec: str,
) -> None:
    rom = source.read_bytes()
    if not rom or len(rom) % BANK_SIZE:
        raise ValueError("ROM size must be a non-zero multiple of 16 KB")
    bank_count = len(rom) // BANK_SIZE
    if bank_count > MAX_BANKS:
        raise ValueError(f"ROM has {bank_count} banks; GBZ supports at most {MAX_BANKS}")
    invalid_banks = sorted(bank for bank in raw_banks if bank < 0 or bank >= bank_count)
    if invalid_banks:
        raise ValueError(f"raw bank outside ROM: {invalid_banks[0]}")

    payload = bytearray()
    entries: list[tuple[int, int, int, int]] = []
    for bank_number in range(bank_count):
        bank = rom[bank_number * BANK_SIZE : (bank_number + 1) * BANK_SIZE]
        compressor = zlib.compressobj(level=9, wbits=-15)
        deflated = compressor.compress(bank) + compressor.flush()
        lz4 = lz4_compress(bank) if codec in ("lz4", "auto") else b""
        if lz4 and lz4_decompress(lz4) != bank:
            raise ValueError(f"internal LZ4 verification failed for bank {bank_number}")
        if codec == "deflate":
            compressed, compressed_flag = deflated, FLAG_DEFLATE
        elif codec == "lz4":
            compressed, compressed_flag = lz4, FLAG_LZ4
        else:
            compressed, compressed_flag = min(
                ((deflated, FLAG_DEFLATE), (lz4, FLAG_LZ4)), key=lambda item: len(item[0])
            )
        if bank_number not in raw_banks and len(compressed) < len(bank):
            stored = compressed
            flags = compressed_flag
        else:
            stored = bank
            flags = 0
        offset = HEADER_SIZE + len(payload)
        entries.append((offset, len(stored), flags, zlib.crc32(bank) & 0xFFFFFFFF))
        payload.extend(stored)

    total_size = HEADER_SIZE + len(payload)
    if total_size > 1280 * 1024:
        raise ValueError(
            f"Compressed container is {total_size:,} bytes and exceeds the 1.25 MiB slot"
        )

    header = bytearray(HEADER_SIZE)
    struct.pack_into(
        HEADER_FORMAT,
        header,
        0,
        MAGIC,
        VERSION,
        HEADER_SIZE,
        len(rom),
        BANK_SIZE,
        bank_count,
        zlib.crc32(rom) & 0xFFFFFFFF,
        len(payload),
        0,
        0,
    )
    entry_size = struct.calcsize(ENTRY_FORMAT)
    for index, entry in enumerate(entries):
        struct.pack_into(ENTRY_FORMAT, header, 32 + index * entry_size, *entry)
    header_crc = zlib.crc32(header) & 0xFFFFFFFF
    struct.pack_into("<I", header, 24, header_crc)

    destination.write_bytes(header + payload)
    raw_percent = total_size * 100.0 / len(rom)
    print(f"ROM:       {len(rom):,} bytes ({bank_count} banks)")
    print(f"GBZ:       {total_size:,} bytes ({raw_percent:.1f}% of original)")
    print(f"ROM CRC32: {zlib.crc32(rom) & 0xFFFFFFFF:08X}")
    print(f"Raw banks: {len(raw_banks)}")
    print(f"Codec:     {codec}")
    print(f"Saved:     {destination}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=pathlib.Path, help="input .gb or .gbc ROM")
    parser.add_argument("output", nargs="?", type=pathlib.Path, help="output .gbz path")
    parser.add_argument(
        "--raw-banks",
        default="",
        metavar="LIST",
        help="banks to store raw for direct XIP reads, e.g. 0-28,0x3a-0x3d",
    )
    parser.add_argument(
        "--codec",
        choices=("deflate", "lz4", "auto"),
        default="deflate",
        help="compression used for non-raw banks (default: deflate)",
    )
    args = parser.parse_args()
    output = args.output or args.rom.with_suffix(".gbz")
    try:
        pack_rom(args.rom, output, parse_bank_list(args.raw_banks), args.codec)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
