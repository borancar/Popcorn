#!/usr/bin/env python3
"""
Check popcorn.unpacked.exe against the packed original.

The decisive test is a round trip: load the emitted EXE the way DOS would at
some segment, apply its relocation table, and require the result to be
byte-identical to what EXEPACK's own stub leaves in memory at that segment.
If that holds, the unpacked file is the original image by construction, and
its relocation table is the one the stub would have applied.

If reconstruct/popcorn-dev has been built, its own unpacker is checked too. That
one is a hundred lines of C that decode EXEPACK's RLE directly rather than
running the stub, and the port depends on it to read the player's copy of the
game at startup - so "it agrees with the emulator" is the property that
matters, and it is cheap to assert.

Usage:
    python validate.py
    python validate.py --seg 0x1234
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

from unpack_popcorn import MZ, ExepackHeader, DEFAULT_EXE, unpack_at

HERE = os.path.dirname(os.path.abspath(__file__))


def load_flat(path, seg):
    """Load an MZ at `seg` and apply its relocations, as DOS would."""
    data = open(path, "rb").read()
    mz = MZ(data)
    img = bytearray(mz.image)
    for i in range(mz.crlc):
        o, s = struct.unpack_from("<HH", data, mz.lfarlc + i * 4)
        a = s * 16 + o
        v = struct.unpack_from("<H", img, a)[0]
        struct.pack_into("<H", img, a, (v + seg) & 0xFFFF)
    return mz, bytes(img)


def check_c_unpacker(packed, reference, seg):
    """Compare reconstruct/popcorn-dev's unpacker against this one, if it is built.

    The C image comes out with relocations unapplied, which is the same thing
    the round-trip reference holds before `seg` is added - so they are directly
    comparable only at seg 0. Rather than special-case that, the C output is
    compared against the unpacked file's own image, which is what the port
    actually consumes.
    """
    exe = os.path.join(HERE, "reconstruct", "popcorn-dev")
    if not os.path.exists(exe):
        print("reconstruct/popcorn-dev is not built; "
              "skipping the C unpacker check")
        return True
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        out = tmp.name
    try:
        r = subprocess.run([exe, "--dump-image", out],
                           capture_output=True, text=True,
                           env={**os.environ, "POPCORN_EXE": packed})
        if r.returncode != 0:
            print(f"FAIL reconstruct/popcorn-dev exited {r.returncode}: "
                  f"{r.stderr.strip()}")
            return False
        got = open(out, "rb").read()
    finally:
        os.unlink(out)

    _, want = load_flat(os.path.join(HERE, "popcorn.unpacked.exe"), 0)
    if got == want:
        print(f"the C unpacker agrees: {len(got)} bytes identical")
        return True
    if len(got) != len(want):
        print(f"FAIL the C unpacker produced {len(got)} bytes, not {len(want)}")
        return False
    bad = [i for i in range(len(got)) if got[i] != want[i]]
    print(f"FAIL the C unpacker differs in {len(bad)} bytes, "
          f"first at {bad[0]:#08x}")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packed", default=DEFAULT_EXE)
    ap.add_argument("--unpacked", default=os.path.join(HERE, "popcorn.unpacked.exe"))
    ap.add_argument("--seg", type=lambda s: int(s, 0), default=0x2140)
    a = ap.parse_args()

    packed = open(a.packed, "rb").read()
    pmz = MZ(packed)
    xh = ExepackHeader(pmz.image, pmz.cs, pmz.ip)

    umz, ours = load_flat(a.unpacked, a.seg)
    theirs = unpack_at(a.packed, pmz, xh, a.seg)

    ok = True
    if len(ours) != len(theirs):
        print(f"FAIL length {len(ours)} != {len(theirs)}")
        ok = False
    else:
        bad = [i for i in range(len(ours)) if ours[i] != theirs[i]]
        if bad:
            print(f"FAIL {len(bad)} bytes differ at seg {a.seg:#06x}, "
                  f"first at {bad[0]:#08x}")
            ok = False
        else:
            print(f"round trip at seg {a.seg:#06x}: "
                  f"{len(ours)} bytes identical")

    ok = check_c_unpacker(a.packed, ours, a.seg) and ok

    for name, got, want in (("entry cs", umz.cs, xh.real_cs),
                            ("entry ip", umz.ip, xh.real_ip),
                            ("stack ss", umz.ss, xh.real_ss),
                            ("stack sp", umz.sp, xh.real_sp)):
        if got != want:
            print(f"FAIL {name}: {got:#06x} != {want:#06x}")
            ok = False
    if ok:
        print(f"entry {umz.cs:04x}:{umz.ip:04x}, stack {umz.ss:04x}:{umz.sp:04x}, "
              f"{umz.crlc} relocations - all as the stub would have set them")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
