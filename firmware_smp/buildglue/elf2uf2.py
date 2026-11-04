#!/usr/bin/env python3
"""elf2uf2.py ELF UF2 - convert an RP2040 ELF image to UF2 (flash-loaded segments only).

Same output as the kernel's own tools/elf2uf2 (build_make/tools/elf2uf2/main.cpp) for these
images: every PT_LOAD segment with file data, at its physical (load) address, split into 256-byte
payloads, RP2040 family id. build_smp.sh uses this instead of that host .exe because Windows Smart
App Control intermittently refuses to run a freshly built, unsigned binary, which failed builds
that had otherwise linked fine.
"""
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
PAYLOAD = 256
FLASH_START, FLASH_END = 0x10000000, 0x11000000


def load_segments(elf):
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        sys.exit("error: not a 32-bit little-endian ELF")
    e_phoff, = struct.unpack_from("<I", elf, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 42)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", elf, e_phoff + i * e_phentsize)
        if p_type == 1 and p_filesz > 0:  # PT_LOAD with data
            if not FLASH_START <= p_paddr < FLASH_END:
                sys.exit(f"error: loadable segment at 0x{p_paddr:08x} is not in flash")
            segs.append((p_paddr, elf[p_offset:p_offset + p_filesz]))
    return segs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        elf = f.read()
    pages = {}
    for addr, data in load_segments(elf):
        for off in range(len(data)):
            a = addr + off
            page = pages.setdefault(a & ~(PAYLOAD - 1), bytearray(PAYLOAD))
            page[a & (PAYLOAD - 1)] = data[off]
    out = bytearray()
    addrs = sorted(pages)
    for n, a in enumerate(addrs):
        out += struct.pack("<IIIIIIII", UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_FLAG_FAMILY_ID_PRESENT,
                           a, PAYLOAD, n, len(addrs), RP2040_FAMILY_ID)
        out += pages[a] + bytes(476 - PAYLOAD)
        out += struct.pack("<I", UF2_MAGIC_END)
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print(f"{sys.argv[2]}: {len(addrs)} blocks, {len(addrs) * PAYLOAD} bytes of flash")


if __name__ == "__main__":
    main()
