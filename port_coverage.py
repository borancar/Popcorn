#!/usr/bin/env python3
"""
How much of the code segment the C port actually covers.

Measured by the image offset each routine carries, not by name - names are what
makes this kind of count wrong, and a routine can be named in a comment without
being written. A routine counts as transcribed when a `1ac2:xxxx` marker for it
appears in one of the port's source files and it is *not* in stubs.c.

Usage:
    python port_coverage.py                # the summary and what is left
    python port_coverage.py --by-size      # biggest first, to pick the next
    python port_coverage.py --verified     # cross-check against verify.c
"""
import argparse
import os
import re
import struct

from analyze import CodeMap, CODE_BASE, EXTRA_ENTRIES
from tools_dis import load_image, UNPACKED

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "reconstruct")
# A routine's *header* comment, which is the convention every transcribed
# routine in game.c carries:  ` * 1ac2:27d7  ball_step`. Matching bare
# `1ac2:xxxx` anywhere instead counts every offset mentioned in prose, which
# inflated this by thirty routines the first time it was run.
# Matches both forms a header takes: `/* 1ac2:27d7  ball_step` opening a
# comment, and ` * 1ac2:27d7  ball_step` continuing one. A header line may name
# more than one offset (`1ac2:0085 / 1ac2:0090`), so every offset on a matching
# line counts.
MARK = re.compile(r"^\s*(?:/\*|\*)\s*1ac2:[0-9a-f]{4}.*$", re.M)
STUB_MARK = re.compile(r"1ac2:([0-9a-f]{4})")


def port_markers():
    """Offsets claimed by the port, and separately those only stubbed."""
    done, stubbed = set(), set()
    for name in sorted(os.listdir(SRC)):
        if not name.endswith((".c", ".h")):
            continue
        if name.endswith(".h"):
            continue                    # declarations, not implementations
        text = open(os.path.join(SRC, name)).read()
        if name == "stubs.c":
            stubbed |= {int(m, 16) for m in STUB_MARK.findall(text)}
        else:
            for line in MARK.findall(text):
                done |= {int(m, 16) for m in STUB_MARK.findall(line)}
    # A header lists everything, including stubs. Anything only stubs.c
    # implements is not transcribed, whatever the header says.
    return done - stubbed, stubbed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--by-size", action="store_true")
    ap.add_argument("--verified", action="store_true")
    a = ap.parse_args()

    img = load_image(UNPACKED)
    data = open(UNPACKED, "rb").read()
    entry_ip = struct.unpack_from("<H", data, 0x14)[0]
    cm = CodeMap(img)
    cm.build([entry_ip] + list(EXTRA_ENTRIES))

    sizes = {}
    for e, body in cm.funcs.items():
        sizes[e] = sum(cm.insns[o].size for o in body if o in cm.insns)

    done, stubbed = port_markers()
    done_r = {e for e in sizes if e in done}
    left = {e: n for e, n in sizes.items() if e not in done}

    tb = sum(sizes[e] for e in done_r)
    total = sum(sizes.values())
    skip = noops()
    nr = {e for e in done_r if e in skip}
    nb = sum(sizes[e] for e in nr)
    print(f"{len(done_r) - len(nr)} of {len(sizes) - len(nr)} routines the "
          f"port is meant to have, {tb - nb} of {total - nb} bytes "
          f"({100.0 * (tb - nb) / (total - nb):.1f}%)")
    if nr:
        print(f"\n{len(nr)} deliberately not transcribed, {nb} bytes:")
        for e in sorted(nr):
            print(f"  {e:04x}  {sizes[e]:5d} b  {skip[e]}")

    if a.verified:
        v = open(os.path.join(SRC, "verify.c")).read()
        checked = {int(m, 16) for m in re.findall(r"case 0x([0-9A-Fa-f]{4}):", v)}
        unver = sorted(e for e in done_r - set(skip) if e not in checked)
        print(f"{len((done_r - set(skip)) & checked)} can be byte-checked; "
              f"{len(unver)} are not:")
        for e in unver:
            print(f"  {e:04x}  {sizes[e]:5d} b")
        return

    order = sorted(left.items(), key=lambda kv: -kv[1]) if a.by_size \
        else sorted(left.items())
    print(f"\n{len(order)} routines left, {sum(left.values())} bytes:")
    for e, n in order:
        tag = "stub" if e in stubbed else ""
        callers = len(cm.callers[e])
        print(f"  {e:04x}  {n:5d} b  called by {callers:2d}  {tag}")


    unwired(os.path.dirname(os.path.abspath(__file__)))


def noops():
    """Routines the port has on purpose left empty.

    A no-op still carries its `1ac2:xxxx` header, so the transcription count
    would keep claiming it. That is the wrong claim to make: F10's boss key
    and F5's redefine-keys screen are decisions about what the port is for,
    and counting them either way - as done, or as work remaining - says
    something untrue. They come out of both sides of the figure instead.

    Detected rather than listed, so emptying a routine and forgetting to say
    so here cannot happen.
    """
    import re
    src = open(os.path.join(SRC, "game.c")).read()
    out = {}
    head = re.compile(r"^(?:/\*|\s\*)\s*1ac2:([0-9a-f]{4})\s+(\w+)", re.M)
    for m in head.finditer(src):
        off, name = int(m.group(1), 16), m.group(2)
        d = re.search(r"^\w[^\n]*\b" + name + r"\s*\([^\n]*\n\{\n(.*?)^\}",
                      src[m.end():], re.M | re.S)
        if not d:
            continue
        body = re.sub(r"/\*.*?\*/", "", d.group(1), flags=re.S)
        body = re.sub(r"\(void\)\s*\w+\s*;", "", body)
        if not body.strip():
            out[off] = name
    return out


def unwired(here):
    """Functions the port defines and never calls.

    Transcribing a routine and never wiring it up leaves something that looks
    finished from the notes and has never run. level_load_file sat like that
    for months - the .PPC loader was complete and the port had no way to name
    a file - and employee_enter's message and the sound player's sustain were
    the same shape. A count is cheap; the three that are meant to be here are
    named so the fourth stands out.
    """
    import re
    src = open(os.path.join(here, "reconstruct", "game.c")).read()
    body = src
    for f in ("verify.c", "lockstep.c", "sdl_io.c", "main.c"):
        body += open(os.path.join(here, "reconstruct", f)).read()
    sig = r"^(?:static\s+)?(?:void|int32_t|uint32_t|uint8_t)\s+"
    defs = re.findall(sig + r"(\w+)\s*\(", src, re.M)
    expected = {
        "drive_check": "no disk here; the port opens the file directly",
        "drive_writable": "likewise",
        "plot_pixel": "INT 10h AH=0Ch without bit 7; the game only XORs",
        "read_new_key": "the redefine-keys screen, deliberately not ported",
        "define_keys_prompt": "likewise",
    }
    out = []
    for name in sorted(set(defs)):
        uses = len(re.findall(r"\b" + name + r"\s*\(", body))
        made = len(re.findall(sig + name + r"\s*\(", src, re.M))
        if uses - made <= 0:
            out.append(name)
    print(f"\n{len(set(defs))} functions in the port, "
          f"{len(out)} never called:")
    for n in out:
        why = expected.get(n)
        print(f"  {n:24s} {why or '** not accounted for **'}")
    return [n for n in out if n not in expected]

if __name__ == "__main__":
    main()
