#!/usr/bin/env python3
"""Build a flashable GK mini UF2 from the base firmware, a GameKing BIOS and
a GameKing cartridge ROM.

    python3 gkpatch.py PM2040_GK.uf2 gm218.bin game.bin -o game_pm.uf2

The result is a single UF2 for the PM2040 (hold BOOT, plug in, drop the file
onto the RPI-RP2 drive). Flash layout (see code/main.c):

    0x000000  firmware            0x040000  BIOS (512 KB)
    0x03F000  info block          0x0C0000  cartridge ROM

By default the BIOS auto-sleep (the GameKing blanks the screen after ~2
minutes without input and needs START held for ~2 s to wake) is patched out,
using the same patch sites as the GameKing MiSTer core. --keep-sleep leaves
the BIOS untouched. The patch is only applied when the original bytes match.
"""
import argparse
import struct
import sys

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2040_FAMILY = 0xE48BFF56
XIP_BASE = 0x10000000

INFO_OFFSET = 0x03F000
BIOS_OFFSET = 0x040000
ROM_OFFSET = 0x0C0000
BIOS_SIZE = 0x80000

# (flat BIOS address, original byte, patched byte) - GameKing_MiSTer
# rtl/gameking/bios_patch.sv
SLEEP_PATCH = [
    (0x06C0D, 0xD0, 0xEA),   # wake: BNE over the 2 s START hold -> NOP
    (0x06C0E, 0x48, 0xEA),
    (0x04886, 0xD0, 0x80),   # boot shell idle timer: BNE -> BRA
    (0x042D7, 0xB2, 0x60),   # API $52 (sleep service) -> RTS
]


def read_uf2(data):
    """Return {flash address: 256-byte page} from a UF2 image."""
    pages = {}
    for off in range(0, len(data), 512):
        blk = data[off:off + 512]
        if len(blk) < 512:
            break
        m0, m1, flags, addr, size, _, _, fam = struct.unpack_from('<8I', blk)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1:
            continue
        if struct.unpack_from('<I', blk, 508)[0] != UF2_MAGIC_END:
            continue
        if flags & 1:   # "not main flash"
            continue
        pages[addr] = blk[32:32 + size]
    return pages


def write_uf2(pages):
    # Emit one contiguous image (like GB mini's UF2): fill any gaps with
    # erased (0xFF) pages so no boot ROM quirk about sparse files applies.
    lo, hi = min(pages), max(pages)
    for a in range(lo, hi + 256, 256):
        pages.setdefault(a, b'\xff' * 256)
    addrs = sorted(pages)
    out = bytearray()
    for i, addr in enumerate(addrs):
        payload = bytes(pages[addr]).ljust(256, b'\xff')
        blk = struct.pack('<8I', UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, addr,
                          256, i, len(addrs), RP2040_FAMILY)
        blk += payload + bytes(476 - 256)
        blk += struct.pack('<I', UF2_MAGIC_END)
        out += blk
    return out


def add_region(pages, flash_offset, data):
    for i in range(0, len(data), 256):
        pages[XIP_BASE + flash_offset + i] = data[i:i + 256].ljust(256, b'\xff')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('firmware', help='base firmware (PM2040_GK.uf2 or .bin)')
    ap.add_argument('bios', help='GameKing BIOS image (gm218.bin, 512 KB)')
    ap.add_argument('rom', nargs='?', help='GameKing cartridge ROM (.bin)')
    ap.add_argument('-o', '--output', required=True, help='output .uf2')
    ap.add_argument('--keep-sleep', action='store_true',
                    help='do not patch out the BIOS auto-sleep')
    ap.add_argument('--flash-size', type=lambda s: int(s, 0), default=0x200000,
                    help='cart flash size in bytes (default 2 MB)')
    args = ap.parse_args()

    fw = open(args.firmware, 'rb').read()
    if fw[:4] == struct.pack('<I', UF2_MAGIC0):
        pages = read_uf2(fw)
        fw_end = max(pages) + 256 - XIP_BASE if pages else 0
    else:
        pages = {}
        add_region(pages, 0, fw)
        fw_end = len(fw)
    if fw_end > INFO_OFFSET:
        sys.exit(f'firmware is {fw_end:#x} bytes and overlaps the info block')
    # Drop anything the base image may already carry past the firmware.
    pages = {a: p for a, p in pages.items() if a - XIP_BASE < INFO_OFFSET}

    bios = bytearray(open(args.bios, 'rb').read())
    if len(bios) != BIOS_SIZE:
        sys.exit(f'BIOS must be {BIOS_SIZE} bytes (got {len(bios)})')
    flags = 0
    if not args.keep_sleep:
        if all(bios[a] == o for a, o, _ in SLEEP_PATCH):
            for a, _, p in SLEEP_PATCH:
                bios[a] = p
            flags |= 1
            print('BIOS auto-sleep patched out')
        else:
            print('BIOS not recognised, auto-sleep patch skipped')

    rom = open(args.rom, 'rb').read() if args.rom else b''
    max_rom = args.flash_size - ROM_OFFSET
    if len(rom) > max_rom:
        sys.exit(f'ROM is {len(rom)} bytes, at most {max_rom} fit')

    info = b'GKMINI\x00\x01' + struct.pack('<3I', BIOS_SIZE, len(rom), flags)
    add_region(pages, INFO_OFFSET, info)
    add_region(pages, BIOS_OFFSET, bios)
    if rom:
        add_region(pages, ROM_OFFSET, rom)

    out = write_uf2(pages)
    open(args.output, 'wb').write(out)
    print(f'wrote {args.output}: firmware {fw_end} B, ROM {len(rom)} B, '
          f'{len(pages)} blocks')


if __name__ == '__main__':
    main()
