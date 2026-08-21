#!/usr/bin/env python3
"""
Recover POPCORN.EXE to a plain, unpacked MZ executable.

POPCORN.EXE is Microsoft EXEPACK-compressed: 103,848 bytes on disk that expand
to a 133,808-byte image.  The MZ header carries **zero** relocations and an
entry point 16 bytes from the end of the file, which is the give-away - both
belong to the unpacker stub, not to the game.

Rather than reimplement EXEPACK's RLE format, this loads the file the way DOS
would into a Unicorn x86-16 CPU and lets the stub decompress *itself*, stopping
at the handoff to the original entry point.  Whatever is in memory then is the
original image, by construction.  The one thing the stub does that we must undo
is applying relocations for the segment it happened to be loaded at: so the
unpack is run twice, at two different load segments, and words that differ by
exactly the segment delta are the relocation sites.  That reading is then
checked against the stub's own relocation table, which we parse independently.

Usage:
    python unpack_popcorn.py                       # -> popcorn.unpacked.exe
    python unpack_popcorn.py -o /tmp/p.exe --verbose
"""
import argparse
import os
import struct
import sys

from unicorn import *
from unicorn.x86_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.abspath(os.environ.get(
    "POPCORN_GAME_DIR", os.path.join(HERE, "popcorn")))
DEFAULT_EXE = os.path.join(GAME_DIR, "popcorn.exe")

MEM_SIZE = 0x200000
EXEPACK_SIG = 0x4252            # 'RB'


class MZ:
    """The bits of an MZ header this tool cares about."""

    def __init__(self, data):
        self.data = data
        (self.cblp, self.cp, self.crlc, self.cparhdr, self.minalloc,
         self.maxalloc, self.ss, self.sp, self.csum, self.ip, self.cs,
         self.lfarlc, self.ovno) = struct.unpack_from("<13H", data, 2)
        self.hdr = self.cparhdr * 16
        size = (self.cp - 1) * 512 + self.cblp if self.cblp else self.cp * 512
        self.image = data[self.hdr:size]


class ExepackHeader:
    """The 16- or 18-byte block sitting immediately before the stub code.

    Two variants exist.  The longer one inserts `skip_len` before the 'RB'
    signature; the shorter one has no `skip_len` and is what POPCORN uses.
    Which one it is can be read off unambiguously: the signature word is at a
    fixed distance from the *end* of the header, so try both and see which
    lands on 'RB'.
    """

    def __init__(self, image, cs, ip):
        base = cs * 16
        for size in (18, 16):
            if struct.unpack_from("<H", image, base + size - 2)[0] == EXEPACK_SIG:
                break
        else:
            raise SystemExit("not an EXEPACK file: no 'RB' signature at CS:0")
        self.size = size
        f = struct.unpack_from("<8H", image, base)
        (self.real_ip, self.real_cs, self.mem_start, self.exepack_size,
         self.real_sp, self.real_ss, self.dest_len) = f[:7]
        self.skip_len = f[7] if size == 18 else 1
        # The stub begins where the header ends and IP points into it.
        if ip != size:
            print(f"note: entry IP {ip:#06x} is not the header size {size:#06x}",
                  file=sys.stderr)

    def __str__(self):
        return (f"real_cs:ip={self.real_cs:04x}:{self.real_ip:04x}  "
                f"real_ss:sp={self.real_ss:04x}:{self.real_sp:04x}  "
                f"dest_len={self.dest_len:#06x} para "
                f"({self.dest_len * 16} bytes)  "
                f"exepack_size={self.exepack_size:#06x}")


def stub_relocations(image, cs, xh):
    """Parse the relocation table the stub carries, without running it.

    It sits after the stub code, inside the `exepack_size` window, and is
    sixteen runs - one per 64 KB of the unpacked image, segments 0x0000 to
    0xF000 - each a count followed by that many 16-bit offsets.  Returned as
    linear offsets into the unpacked image, which is the form the MZ
    relocation table wants back (as seg:off pairs).
    """
    base = cs * 16
    end = base + xh.exepack_size
    # Find the table by scanning for the only position from which sixteen
    # well-formed runs land exactly on `end`.  The stub's `mov si, imm` tells
    # us where it starts, but reading it out of the code is more fragile than
    # checking the structure closes.
    for start in range(base + xh.size, end):
        p, ok = start, True
        for _ in range(16):
            if p + 2 > end:
                ok = False
                break
            n = struct.unpack_from("<H", image, p)[0]
            p += 2 + n * 2
        if ok and p == end:
            break
    else:
        raise SystemExit("could not locate the stub's relocation table")

    relocs, p = [], start
    for i in range(16):
        n = struct.unpack_from("<H", image, p)[0]
        p += 2
        for j in range(n):
            off = struct.unpack_from("<H", image, p)[0]
            p += 2
            relocs.append(i * 0x10000 + off)
    return sorted(relocs), start - base


def unpack_at(exe, mz, xh, load_seg, verbose=False):
    """Run the stub with the image loaded at `load_seg`; return the result."""
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, MEM_SIZE)
    base = load_seg * 16
    uc.mem_write(base, mz.image)

    # A PSP is not needed - the stub touches none of it - but the memory above
    # the image must exist, and it does: the whole 2 MB is mapped and zeroed.
    uc.reg_write(UC_X86_REG_CS, (load_seg + mz.cs) & 0xFFFF)
    uc.reg_write(UC_X86_REG_IP, mz.ip)
    uc.reg_write(UC_X86_REG_SS, (load_seg + mz.ss) & 0xFFFF)
    uc.reg_write(UC_X86_REG_SP, mz.sp)
    uc.reg_write(UC_X86_REG_DS, load_seg - 0x10)
    uc.reg_write(UC_X86_REG_ES, load_seg - 0x10)

    stop = ((load_seg + xh.real_cs) & 0xFFFF) * 16 + xh.real_ip
    trap = []

    def on_intr(uc_, intno, user):
        # The stub's only interrupt is the "Packed file is corrupt" bail-out.
        trap.append(("int", intno, uc_.reg_read(UC_X86_REG_AX)))
        uc_.emu_stop()

    uc.hook_add(UC_HOOK_INTR, on_intr)
    start = ((load_seg + mz.cs) & 0xFFFF) * 16 + mz.ip
    uc.emu_start(start, stop, count=20_000_000)

    ip = uc.reg_read(UC_X86_REG_IP)
    cs = uc.reg_read(UC_X86_REG_CS)
    if trap:
        raise SystemExit(f"stub bailed out: {trap[0]}")
    if cs * 16 + ip != stop:
        raise SystemExit(f"stub stopped at {cs:04x}:{ip:04x}, "
                         f"expected {(load_seg + xh.real_cs) & 0xFFFF:04x}:"
                         f"{xh.real_ip:04x}")
    if verbose:
        ss, sp = uc.reg_read(UC_X86_REG_SS), uc.reg_read(UC_X86_REG_SP)
        print(f"  load_seg={load_seg:#06x}  handoff at {cs:04x}:{ip:04x}  "
              f"ss:sp={ss:04x}:{sp:04x}")
    return bytes(uc.mem_read(base, xh.dest_len * 16))


def diff_relocations(a, b, delta):
    """Words differing by exactly `delta` between two loads are relocations."""
    out = []
    for i in range(0, min(len(a), len(b)) - 1):
        if a[i] == b[i] and a[i + 1] == b[i + 1]:
            continue
        wa = a[i] | (a[i + 1] << 8)
        wb = b[i] | (b[i + 1] << 8)
        if (wb - wa) & 0xFFFF == delta & 0xFFFF:
            out.append(i)
    # A relocated word makes both its bytes differ, so consecutive hits at
    # i and i+1 cannot both be sites; keep the first of any adjacent pair.
    pruned, prev = [], -2
    for i in out:
        if i == prev + 1:
            continue
        pruned.append(i)
        prev = i
    return pruned


def write_exe(path, image, relocs, xh, load_seg_ref):
    """Emit a plain MZ: the image, a relocation table, the original entry."""
    n = len(relocs)
    hdr_len = 0x1C + n * 4
    hdr_len = (hdr_len + 15) & ~15
    if hdr_len < 0x20:
        hdr_len = 0x20
    total = hdr_len + len(image)
    cp = (total + 511) // 512
    cblp = total % 512

    h = bytearray(hdr_len)
    struct.pack_into("<14H", h, 0,
                     0x5A4D, cblp, cp, n, hdr_len // 16,
                     0x0770,               # minalloc: what the packed file asked
                     0xFFFF,               # maxalloc
                     xh.real_ss, xh.real_sp,
                     0,                    # checksum, unused
                     xh.real_ip, xh.real_cs,
                     0x001C, 0)
    for i, off in enumerate(relocs):
        struct.pack_into("<HH", h, 0x1C + i * 4, off & 0xF, off >> 4)
    with open(path, "wb") as f:
        f.write(bytes(h) + image)
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", nargs="?", default=DEFAULT_EXE)
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "popcorn.unpacked.exe"))
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    data = open(a.exe, "rb").read()
    mz = MZ(data)
    if mz.crlc:
        print(f"note: {mz.crlc} relocations in the packed header", file=sys.stderr)
    xh = ExepackHeader(mz.image, mz.cs, mz.ip)
    print(f"EXEPACK: {xh}")

    tbl, tbl_off = stub_relocations(mz.image, mz.cs, xh)
    print(f"stub relocation table at CS:{tbl_off:#06x}: {len(tbl)} entries")

    # Not the segment DOS would pick.  The stub walks its pointers downwards
    # and keeps them normalised with `or si,0xfff0`, which drives the segment
    # register *below* zero and relies on the 8086 wrapping the resulting
    # address at 1 MB.  Unicorn's memory is flat and does not wrap, so at a
    # realistic load segment the source pointer escapes to 0x10eea1 and the
    # stub bails out with "Packed file is corrupt".  Loading high enough that
    # the segment never goes negative sidesteps it without patching anything;
    # the image is normalised back to segment zero at the end regardless.
    seg_a, seg_b = 0x2000, 0x3000
    img_a = unpack_at(a.exe, mz, xh, seg_a, a.verbose)
    img_b = unpack_at(a.exe, mz, xh, seg_b, a.verbose)
    found = diff_relocations(img_a, img_b, seg_b - seg_a)
    print(f"differential relocations: {len(found)} sites")

    missing = sorted(set(tbl) - set(found))
    extra = sorted(set(found) - set(tbl))
    if missing:
        # A relocated word whose stored value is zero-ish can coincide between
        # the two loads only if the delta is zero, so this should be empty.
        print(f"  in stub table but not seen: {len(missing)} "
              f"(first: {[hex(x) for x in missing[:8]]})")
    if extra:
        print(f"  seen but not in stub table: {len(extra)} "
              f"(first: {[hex(x) for x in extra[:8]]})")
    if not missing and not extra:
        print("  the two readings agree exactly")

    relocs = sorted(set(tbl) | set(found))
    # Normalise the image back to a load segment of zero.
    img = bytearray(img_a)
    for off in relocs:
        v = struct.unpack_from("<H", img, off)[0]
        struct.pack_into("<H", img, off, (v - seg_a) & 0xFFFF)

    total = write_exe(a.out, bytes(img), relocs, xh, seg_a)
    print(f"wrote {a.out}: {total} bytes "
          f"({len(img)} image + {len(relocs)} relocations), "
          f"entry {xh.real_cs:04x}:{xh.real_ip:04x}")


if __name__ == "__main__":
    main()
