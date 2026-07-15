#!/usr/bin/env python3
"""Builds a minimal, fully original homebrew .nds ROM for testing melonDS's
remote-server patch (host/melonds-patches/0001-remote-server-integration.patch)
against a real, direct-booting, JIT-executing ROM -- see
tests/homebrew-test-rom/README.md for how and why.

Field offsets below were confirmed against melonDS's own compiled
NDSHeader layout using a small offsetof() dumper compiled against
melonDS's own src/NDS_Header.h (not guessed or assumed from a naive
struct-packing reading, since the real struct has no #pragma pack and
could in principle have compiler-inserted padding -- it turned out not
to for this compiler/platform, but that was verified, not assumed). See
NDS_Header.h and NDSCart.cpp's ValidateROM()/ParseROM() in the melonDS
source for the validation rules this must satisfy:
  - ARM9ROMOffset/ARM7ROMOffset must each be >= 0x200
  - ARM9ROMOffset+ARM9Size and ARM7ROMOffset+ARM7Size must each be <= romlen
  - romlen must be >= 0x1000 and <= 512MiB
  - ARM9ROMOffset < 0x4000 makes NDSHeader::IsHomebrew() true, which skips
    the commercial-cart "secure area" decryption step entirely
No CRC/logo/checksum fields are validated by melonDS at load time, so
this script leaves those as zero.
"""
import struct

ARM9_RAM_ADDR = 0x02004000
ARM7_RAM_ADDR = 0x02380000
ARM9_ROM_OFFSET = 0x1000  # 4096, right after the header; < 0x4000 => IsHomebrew()

with open("arm9.bin", "rb") as f:
    arm9 = f.read()
with open("arm7.bin", "rb") as f:
    arm7 = f.read()

# Align ARM7 offset to 4 bytes past ARM9 (not required by melonDS, just tidy).
arm7_offset = ARM9_ROM_OFFSET + len(arm9)
arm7_offset = (arm7_offset + 3) & ~3

header = bytearray(4096)


def put(offset, fmt, *values):
    struct.pack_into(fmt, header, offset, *values)


put(0, "12s", b"MELONDSTEST\x00")  # GameTitle[12]
put(12, "4s", b"####")  # GameCode[4] -- conventional homebrew marker
put(16, "2s", b"\x00\x00")  # MakerCode[2]
put(18, "B", 0x00)  # UnitCode: 0 = NDS (not DSi)
put(31, "B", 0x00)  # Autostart

put(32, "<I", ARM9_ROM_OFFSET)
put(36, "<I", ARM9_RAM_ADDR)  # ARM9EntryAddress
put(40, "<I", ARM9_RAM_ADDR)  # ARM9RAMAddress
put(44, "<I", len(arm9))      # ARM9Size

put(48, "<I", arm7_offset)
put(52, "<I", ARM7_RAM_ADDR)  # ARM7EntryAddress
put(56, "<I", ARM7_RAM_ADDR)  # ARM7RAMAddress
put(60, "<I", len(arm7))      # ARM7Size

rom_size = arm7_offset + len(arm7)
rom_size_padded = 1
while rom_size_padded < rom_size:
    rom_size_padded *= 2

put(128, "<I", rom_size_padded)  # ROMSize
put(132, "<I", 0x4000)            # HeaderSize (conventional value; not validated)

rom = bytearray(rom_size_padded)
rom[0:4096] = header
rom[ARM9_ROM_OFFSET:ARM9_ROM_OFFSET + len(arm9)] = arm9
rom[arm7_offset:arm7_offset + len(arm7)] = arm7

with open("test.nds", "wb") as f:
    f.write(rom)

print(f"wrote test.nds: {len(rom)} bytes "
      f"(arm9 @ {ARM9_ROM_OFFSET:#x}+{len(arm9)}, arm7 @ {arm7_offset:#x}+{len(arm7)})")
