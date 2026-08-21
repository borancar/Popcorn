#!/usr/bin/env python3
"""Disassemble a range of the unpacked load image.

Addresses are printed as linear offsets into the image (segment 0 at 0), the
convention every note and every reconstructed function in this repository uses.
`--seg` prints `seg:off` instead, for reading a routine the way its own code
addresses it.

Usage:
    python tools_dis.py 0x1ad33 0x200
    python tools_dis.py 0x1ad33 0x200 --seg 0x1ac2
"""
import argparse
import os
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_16

HERE = os.path.dirname(os.path.abspath(__file__))
UNPACKED = os.path.join(HERE, "popcorn.unpacked.exe")


def load_image(path=UNPACKED):
    """The load image with relocations left at segment 0."""
    data = open(path, "rb").read()
    cparhdr = struct.unpack_from("<H", data, 8)[0]
    cblp, cp = struct.unpack_from("<HH", data, 2)
    hdr = cparhdr * 16
    size = (cp - 1) * 512 + cblp if cblp else cp * 512
    return data[hdr:size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start", type=lambda s: int(s, 0))
    ap.add_argument("count", type=lambda s: int(s, 0), nargs="?", default=0x80)
    ap.add_argument("--seg", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--exe", default=UNPACKED)
    a = ap.parse_args()

    img = load_image(a.exe)
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    seg = a.seg
    off = a.start - (seg * 16 if seg is not None else 0)
    for i in md.disasm(img[a.start:a.start + a.count], off):
        lin = i.address + (seg * 16 if seg is not None else 0)
        label = f"{seg:04x}:{i.address:04x}" if seg is not None else f"{lin:06x}"
        print(f"{label}  {i.bytes.hex():<18s} {i.mnemonic} {i.op_str}")


if __name__ == "__main__":
    main()
