#!/usr/bin/env python3
"""Write the game's data down as C: generate `reconstruct/src/data.c`.

The port used to read the player's own POPCORN.EXE and unpack it, because the
data - the levels, the sprites, the fonts, the scripts - was in the file and
nowhere else. `game.h` has since said what almost all of it *is*, field by
field, and once a byte has a name and a type there is no reason for it to
arrive as an offset into a blob. This turns the image into one initializer for
`image_t`, and the port ships its own data.

Two things it does that a hexdump would not:

  * **The shape comes from the header, not from here.** The layout is read out
    of the DWARF a real compile of `game.h` produces, so every field, array
    dimension, union arm and anonymous member is the one the C actually has.
    A field added or a padding size changed moves this file's output with it;
    nothing here has to be told.

  * **The game's own pointers are written as what they point at.** A `*_ptr`
    holding 0x6d9f comes out as `GOFF(backdrop)`, and the free list's links
    as `GOFF(entities[1])`, `GOFF(entities[2])` and so on. In the file those
    numbers were true because the linker made them so; here they are true
    because they are computed from the field, which is the same fact with the
    arithmetic left in.

The proof is byte-for-byte and is not taken on trust: `popcorn-dev
--dump-builtin` writes the compiled-in image out, and `validate.py` diffs it
against the one recovered from POPCORN.EXE.

    uv run gen_data.py                  # -> reconstruct/src/data.c
    uv run gen_data.py --check          # regenerate and diff, changing nothing

One file serves both of game.h's layouts. The shape is read from the
matching one, MATCH_MEMORY_LAYOUT; the padding - PAD(n), a `_pad_<line>`
field only that layout has - is zero, so nothing here ever writes it, and
padding found holding anything is refused, since data.c cannot name a field
whose name is a line number. The pointers are offsetof expressions and follow
whichever layout compiles them, which is why a pointer into padding is
refused too: the unmatched build would have no field to compute it from.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "reconstruct", "src")
HEADER = os.path.join(SRC, "game.h")
OUT = os.path.join(SRC, "data.c")

# The four overlays, as (member of image_t, macro that names an offset in it).
SEGMENTS = [
    ("seg_global", "global_t", "GOFF"),
    ("seg_assets", "assets_t", "AOFF"),
    ("seg_animations", "animations_t", "NOFF"),
    ("seg_runtime", "runtime_t", "ROFF"),
]

def is_text(body):
    """Is this run of bytes prose rather than pixels?

    Plain ASCII throughout is text and always was. Anything short of that has
    to earn it: four fifths printable, the rest only a control code a string
    uses or a high byte - the game's strings are CP850, so an accented letter
    is 0x82 - **and a space somewhere in it**.

    That last clause is the one with teeth. A row of a sprite can be printable
    by accident: `panel`'s fifth row is 0xac then twenty-seven 0x44, which is
    27/28 printable with one high byte, and came out as `'\254', 'D', 'D', …`
    until this asked for a space as well. Prose has spaces; pixels do not.
    """
    # Six, not four: a sprite row is four bytes for a 16-pixel cell and five
    # for a 20-pixel one, and a row of 0x44 is four printable 'D's. The
    # shortest strings in the image are six - "LACRAL" and "000000".
    if len(body) < 6:
        return False
    if all(32 <= c < 127 for c in body):
        return True
    ascii_n = sum(1 for c in body if 32 <= c < 127)
    other = [c for c in body if not (32 <= c < 127)]
    return ascii_n * 5 >= len(body) * 4 and 32 in body and \
        all(c in (9, 10, 13) or c >= 0x80 for c in other)


ESCAPES = {9: "\\t", 10: "\\n", 13: "\\r", 34: '\\"', 92: "\\\\"}


def escape(c):
    """One byte inside a string literal. Octal, not hex: `\\202` is exactly
    three digits, so a digit after it is a character rather than more escape."""
    if c in ESCAPES:
        return ESCAPES[c]
    return chr(c) if 32 <= c < 127 else f"\\{c:03o}"


def char_lit(c):
    """The same byte as a character constant."""
    if c == 39:
        return "'\\''"
    if c == 92:
        return "'\\\\'"
    if c in ESCAPES and c != 34:
        return f"'{ESCAPES[c]}'"
    return f"'{chr(c)}'" if 32 <= c < 127 else f"'\\{c:03o}'"


# CLAUDE.md's rule, applied to the data: a number that is a position, a count
# or a period is a quantity and reads as one, while an address or a bit
# pattern stays hex. The names are the game's own, so this is a list rather
# than a guess about what a value means.
#
# `rows` is decimal only below ten, where the two bases agree anyway - the
# field is a row count in every entry the image holds, and the rule keeps a
# larger value looking like the mask it would have to be.
DECIMAL = {"x", "y", "dx", "dy", "timer", "period", "rate", "bricks"}
DECIMAL_SMALL = {"rows"}

# And the same rule where the field name alone cannot carry it. `at` is a
# quantity in eog_groups - a byte column on one video row, which is all seven
# groups differ by - and a full video-memory offset in paddle_rows, interlace
# bit and all. Indices are normalised out, so one entry covers a whole table.
DECIMAL_PATHS = {".seg_global.eog_groups[].at"}


# A slot whose whole job is to stop a walk. It is not a pointer and is not
# named `_ptr` for that reason, but the value in it is END_PTR and printing
# 0xffff there says less than the name the header already gives it.
TERMINATOR = {"ends"}

BYTES_PER_LINE = 16
WORDS_PER_LINE = 8

# A byte array whose rows mean something, laid out so they show. The type
# cannot say this - `cells[168]` is one dimension and the playfield is two -
# so the shape the header's comment describes is written here instead, keyed
# by field name and length. Fifty levels come out as fifty pictures.
ROW_WIDTH = {("cells", 168): 12}


# --------------------------------------------------------------- the shape --
#
# Read out of DWARF rather than parsed out of the header. The compiler has
# already resolved every anonymous member, every union arm and every array
# dimension; re-deriving that from the text would be a second implementation of
# C's layout rules, and the two would disagree eventually.

class Type:
    """One DWARF type, flattened to what the emitter needs."""

    def __init__(self, kind, size, **kw):
        self.kind = kind                # base | array | struct | union
        self.size = size
        self.name = kw.get("name")
        self.encoding = kw.get("encoding")
        self.elem = kw.get("elem")      # array: element Type
        self.count = kw.get("count")    # array: elements
        self.members = kw.get("members", [])   # struct/union: (name, off, Type)


def load_types(header=None):
    """Compile game.h and read image_t's layout back out of the DWARF."""
    from elftools.elf.elffile import ELFFile

    inc = os.path.dirname(header) if header else SRC
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "probe.c")
        o = os.path.join(d, "probe.o")
        open(c, "w").write('#include "game.h"\nimage_t probe;\n'
                           "uint8_t *g_image;\n")
        subprocess.run(["gcc", "-std=c99", "-g3", "-DMATCH_MEMORY_LAYOUT",
                        "-c", "-I", inc, "-I", SRC,
                        "-o", o, c], check=True)
        with open(o, "rb") as f:
            elf = ELFFile(f)
            dw = elf.get_dwarf_info()
            dies = {}
            roots = []
            for cu in dw.iter_CUs():
                for die in cu.iter_DIEs():
                    dies[die.offset] = die
                    roots.append(die)
            return _build(dies, roots)


def _attr(die, name):
    a = die.attributes.get(name)
    return a.value if a is not None else None


def _build(dies, roots):
    """image_t as a Type tree. DIE offsets are the graph's identity."""
    cache = {}

    def conv(off):
        if off in cache:
            return cache[off]
        die = dies[off]
        tag = die.tag
        # typedef, const, volatile: pass straight through to what they wrap.
        if tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                   "DW_TAG_volatile_type"):
            t = conv(_attr(die, "DW_AT_type"))
            cache[off] = t
            return t
        if tag == "DW_TAG_base_type":
            t = Type("base", _attr(die, "DW_AT_byte_size"),
                     name=_attr(die, "DW_AT_name").decode(),
                     encoding=_attr(die, "DW_AT_encoding"))
        elif tag == "DW_TAG_array_type":
            elem = conv(_attr(die, "DW_AT_type"))
            # One subrange per dimension, outermost first. Build the nest from
            # the inside out so a [7][1092] is an array of arrays here too.
            dims = []
            for sub in die.iter_children():
                if sub.tag != "DW_TAG_subrange_type":
                    continue
                n = _attr(sub, "DW_AT_count")
                if n is None:
                    ub = _attr(sub, "DW_AT_upper_bound")
                    n = 0 if ub is None else ub + 1
                dims.append(n)
            t = elem
            for n in reversed(dims):
                t = Type("array", t.size * n, elem=t, count=n)
        elif tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            kind = "struct" if tag == "DW_TAG_structure_type" else "union"
            t = Type(kind, _attr(die, "DW_AT_byte_size"),
                     name=(_attr(die, "DW_AT_name") or b"").decode() or None,
                     members=[])
            cache[off] = t                      # before recursing, for cycles
            for m in die.iter_children():
                if m.tag != "DW_TAG_member":
                    continue
                mn = _attr(m, "DW_AT_name")
                t.members.append((mn.decode() if mn else None,
                                  _attr(m, "DW_AT_data_member_location") or 0,
                                  conv(_attr(m, "DW_AT_type"))))
        else:
            raise SystemExit(f"gen_data: unhandled DWARF tag {tag}")
        cache[off] = t
        return t

    for die in roots:
        if die.tag == "DW_TAG_typedef" and _attr(die, "DW_AT_name") == b"image_t":
            return conv(die.offset)
    raise SystemExit("gen_data: image_t is not in the DWARF")


# ------------------------------------------------------- naming an address --

def find_path(typ, base, target):
    """The C member designator for image offset `target` inside `typ`.

    Descends through structs, unions and arrays until it reaches a scalar, so
    a pointer into the middle of a table comes out as the element it names.
    Anonymous members are transparent, which is what the C sees too. Returns
    (path, leftover) or None if the target is outside the type.
    """
    if target < base or target >= base + typ.size:
        return None
    # Each step records what it added and where that subobject began, so the
    # tail can be trimmed afterwards: `entities[1].handler_fn` and
    # `entities[1]` are the same address, and the shorter one is the one that
    # says what the pointer means.
    steps = []
    while True:
        if typ.kind in ("struct", "union"):
            hit = None
            for name, off, mt in typ.members:
                if base + off <= target < base + off + mt.size:
                    # A union's arms overlap: take the first that fits, which
                    # is the arm the emitter writes.
                    hit = (name, off, mt)
                    break
            if hit is None:
                break
            name, off, mt = hit
            base += off
            typ = mt
            if name:
                steps.append((("." if steps else "") + name, base))
        elif typ.kind == "array":
            i = (target - base) // typ.elem.size
            base += i * typ.elem.size
            typ = typ.elem
            steps.append((f"[{i}]", base))
        else:
            break
    # `entities`, `entities[0]` and `entities[0].handler_fn` all begin at the
    # target. Stop at the index where there is one, so a table's entries read
    # alike - `entities[0]` beside `entities[1]`, not `entities` beside it -
    # and otherwise at the first name that starts there.
    at_target = [i for i, (_, at) in enumerate(steps) if at == target]
    if at_target:
        keep = next((i for i in at_target if steps[i][0].startswith("[")),
                    at_target[0])
        del steps[keep + 1:]
    if steps and steps[0][0].startswith("."):
        steps[0] = (steps[0][0][1:], steps[0][1])
    return ("".join(s for s, _ in steps), target - steps[-1][1] if steps
            else target - base)


def defines(pattern):
    """The `#define NAME 0x…` constants in game.h whose name matches."""
    out = {}
    for name, val in re.findall(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+)",
                                open(HEADER).read()):
        if re.search(pattern, name):
            out.setdefault(int(val, 16), name)
    return out


FN_NAMES = None                     # value -> ENTITY_…_FN, filled in main

# The one 16-bit pointer the naming convention cannot reach. `_ptr` marks the
# game's own pointers wherever they are held, and this word is held under
# another name: 0x3138 is the head node's `handler`, and it is the head of the
# **free list** rather than a routine's address. game.h says so at length -
# the union there is what says it - but the field it is written through is
# entity_t's, so it comes out as `handler_fn` and would be a bare number.
POINTER_PATHS = {".seg_global.entity_head.handler_fn"}


# ------------------------------------------------------------ the emitter --

class Emitter:
    def poison(self, typ, off, value):
        """Fill every `_`-named member with `value`, recursively.

        The layout is untouched - this is the padded image with the gaps
        made **visible**. pad_writes.py shows nothing writes them; this is
        the other half, and the harness can run against it unchanged because
        every field is still where the original put it. Anything that reads
        a byte no field covers now draws or plays this instead.
        """
        if typ.kind == "array":
            for i in range(typ.count or 0):
                self.poison(typ.elem, off + i * typ.elem.size, value)
        elif typ.kind in ("struct", "union"):
            for name, moff, mt in typ.members:
                if name and name.startswith("_"):
                    self.img[off + moff:off + moff + mt.size] = \
                        bytes([value]) * mt.size
                else:
                    self.poison(mt, off + moff, value)

    def __init__(self, img, image_t):
        self.img = img
        self.image_t = image_t
        self.poisoned = False
        # Which segment each image offset belongs to, and the macro for it.
        self.segs = []
        for member, tname, macro in SEGMENTS:
            for name, off, mt in image_t.members:
                if name == member:
                    self.segs.append((off, off + mt.size, mt, macro))

    def seg_of(self, off):
        for lo, hi, typ, macro in self.segs:
            if lo <= off < hi:
                return lo, typ, macro
        return None

    def word(self, off):
        return self.img[off] | self.img[off + 1] << 8

    def pointer(self, value, container_off):
        """A stored 16-bit offset, as the field it points at."""
        if value == 0xFFFF:
            return "END_PTR"
        if value == 0:
            return "0"
        seg = self.seg_of(container_off)
        if seg is None:
            return f"{value:#06x}"
        base, typ, macro = seg
        found = find_path(typ, base, base + value)
        if found is None:
            return f"{value:#06x}"
        path, extra = found
        if not path:
            return f"{value:#06x}"
        if any(x.startswith("_") for x in path.replace("[", ".[").split(".")):
            # The unmatched layout has no such field, so the address cannot
            # be computed there. Name the bytes it points at instead.
            raise SystemExit(f"gen_data: {value:#06x} points into padding, "
                             f"{macro}({path}) - name that field in game.h")
        text = f"{macro}({path})"
        return text if extra == 0 else f"{text} + {extra}"

    # -- the recursion -----------------------------------------------------

    def zero(self, off, size):
        return not any(self.img[off:off + size])

    def emit(self, typ, off, ind, field=None, path=""):
        """The initializer for one subobject, as a list of lines."""
        if typ.kind == "base":
            return [self.scalar(typ, off, field, path)]
        if self.zero(off, typ.size):
            return [self.zero_init(typ, off)]
        if typ.kind == "array":
            return self.array(typ, off, ind, field, path)
        return self.struct(typ, off, ind, path)

    def zero_init(self, typ, off):
        """`{0}` for an aggregate, braced as deep as the aggregate goes.

        A single `{0}` does zero the whole object, but it names only the
        innermost first element and GCC says so under -Wmissing-braces. The
        depth is what the brace count has to match.
        """
        depth = 0
        while typ.kind != "base":
            if typ.kind == "array":
                typ = typ.elem
            else:
                arms = [self.arm(typ, off)] if typ.kind == "union" \
                    else typ.members
                if not arms:
                    break
                name, moff, mt = arms[0]
                off, typ = off + moff, mt
            depth += 1
        return "{" * depth + "0" + "}" * depth

    def scalar(self, typ, off, field, path=""):
        n = typ.size
        v = int.from_bytes(self.img[off:off + n], "little")
        if n == 2 and field:
            if field.endswith("_ptr") or path in POINTER_PATHS:
                return self.pointer(v, off)
            if field.endswith("_fn"):
                return FN_NAMES.get(v, f"{v:#06x}")
        if n == 2 and field in TERMINATOR and v == 0xFFFF:
            return "END_PTR"
        if field in DECIMAL or (field in DECIMAL_SMALL and v < 10) or \
                re.sub(r"\[\d+\]", "[]", path) in DECIMAL_PATHS:
            # A signed quantity reads as one. DW_ATE_signed_char is what an
            # int8_t comes back as, and bonus_path's dx runs -38 to +35 - as
            # hex that is 0xda to 0x23, which says nothing about a capsule
            # swaying left of where it started.
            if typ.encoding in (5, 6) and v >= 1 << (8 * n - 1):
                return str(v - (1 << 8 * n))
            return str(v)
        if typ.encoding == 6 and n == 1:            # DW_ATE_signed_char
            return f"{v:#04x}"
        if typ.encoding == 5 and v >= 1 << (8 * n - 1):     # DW_ATE_signed
            return str(v - (1 << 8 * n))
        return f"{v:#0{2 + 2 * n}x}"

    def last_nonzero(self, off, size, stride):
        """How many elements are left once the trailing zero ones go."""
        n = size // stride
        while n and not any(self.img[off + (n - 1) * stride:off + n * stride]):
            n -= 1
        return n

    def array(self, typ, off, ind, field, path=""):
        e = typ.elem
        keep = self.last_nonzero(off, typ.size, e.size)
        dropped = typ.count - keep
        tail = f"  /* + {dropped} zero */" if dropped >= 4 else ""

        if e.kind == "base" and e.size == 1:
            # A char array that is plainly text says so - as a string where
            # there is room for the terminator, and as characters where the
            # text fills the array exactly. That second case is legal C with
            # the NUL dropped and GCC warns about it, which is the whole
            # reason for the two forms.
            raw = bytes(self.img[off:off + typ.size])
            if e.encoding in (6, 8):            # signed / unsigned char
                body = raw.split(b"\0")[0]
                if is_text(body):
                    if len(body) < typ.size and not any(raw[len(body):]):
                        return ['"' + "".join(escape(c) for c in body) + '"']
                    if len(body) == typ.size:
                        return self.wrap([char_lit(c) for c in raw], 10, ind, "")
            per = ROW_WIDTH.get((field, typ.count), BYTES_PER_LINE)
            if per != BYTES_PER_LINE:
                keep, tail = typ.count, ""   # a picture keeps its blank rows
            vals = [f"{b:#04x}" for b in raw[:keep]]
        elif e.kind == "base":
            per = WORDS_PER_LINE
            vals = [self.scalar(e, off + i * e.size, field, f"{path}[{i}]")
                    for i in range(keep)]
            if any(len(v) > 8 for v in vals):
                per = 4
        else:
            # Elements that are themselves structured: one per entry, and the
            # index in a comment so a table can be read against the original.
            lines = ["{"]
            for i in range(keep):
                sub = self.emit(e, off + i * e.size, ind + 1, field,
                                f"{path}[{i}]")
                lines += self.entry(sub, ind + 1, f"[{i}] = ", "," if i < keep - 1 else ",")
            if dropped:
                lines.append("    " * (ind + 1) + f"/* {dropped} more, all zero */")
            lines.append("    " * ind + "}")
            return lines

        return self.wrap(vals, per, ind, tail)

    @staticmethod
    def wrap(vals, per, ind, tail):
        """A brace-list, on one line where it fits and in rows where it does not."""
        if len(vals) <= per and sum(len(v) + 2 for v in vals) < 60:
            return ["{ " + ", ".join(vals) + " }" + tail]
        lines = ["{"]
        for i in range(0, len(vals), per):
            lines.append("    " * (ind + 1) + ", ".join(vals[i:i + per]) + ",")
        if tail:
            lines.append("    " * (ind + 1) + tail.strip())
        lines.append("    " * ind + "}")
        return lines

    @staticmethod
    def entry(sub, ind, prefix, suffix):
        """One `.name = value,` line, or a block when the value is a block."""
        pad = "    " * ind
        if len(sub) == 1:
            return [pad + prefix + sub[0] + suffix]
        # A block already carries its own indentation, closing brace included.
        return [pad + prefix + sub[0]] + sub[1:-1] + [sub[-1] + suffix]

    def arm(self, typ, off):
        """The one arm of a union this file writes.

        A union is one object: initialising a second arm would overwrite the
        first, so exactly one is written and it has to be one that covers the
        whole union - a shorter arm leaves the rest zero-filled, which is only
        right when the rest is zero. Prefer a full-width arm whose name is not
        a placeholder; fall back to the widest there is.
        """
        full = [m for m in typ.members if m[2].size == typ.size]
        for m in full:
            if m[0] is None or not m[0].startswith("_"):
                return m
        if full:
            return full[0]
        pick = max(typ.members, key=lambda m: m[2].size)
        if not self.zero(off + pick[2].size, typ.size - pick[2].size):
            raise SystemExit(
                f"gen_data: no arm of {typ.name or 'a union'} at {off:#x} "
                f"covers its non-zero bytes")
        return pick

    def members(self, typ, off, ind, path):
        """Designators for the members worth writing, in order."""
        out = []
        arms = [self.arm(typ, off)] if typ.kind == "union" else typ.members
        for name, moff, mt in arms:
            if self.zero(off + moff, mt.size):
                continue
            if name is None:                    # anonymous: splice it in
                out += self.members(mt, off + moff, ind, path)
                continue
            if name.startswith("_") and not self.poisoned:
                # Padding is named for the line it is on, which moves with
                # every edit above it - so it has to be zero, and these bytes
                # are something. --poison is the exception: its file is a
                # throwaway for the matching build, made against one header.
                raise SystemExit(
                    f"gen_data: padding {path}.{name} at {off + moff:#x} is not "
                    f"zero - make those bytes a field in game.h")
            sub = self.emit(mt, off + moff, ind, name, f"{path}.{name}")
            out += self.entry(sub, ind, f".{name} = ", ",")
        return out

    def struct(self, typ, off, ind, path):
        body = self.members(typ, off, ind + 1, path)
        # A record of two or three scalars - a point, a frame, a capsule kind -
        # says more on one line than on five. 364 of them is the difference
        # between a path you can read and fourteen hundred lines of braces.
        if body and all(len(ln.strip()) and "{" not in ln for ln in body) \
                and sum(len(ln.strip()) + 1 for ln in body) < 56:
            return ["{ " + " ".join(ln.strip() for ln in body) + " }"]
        return ["{"] + body + ["    " * ind + "}"]


PREAMBLE = '''/*
 * The game's data, written down.
 *
 * **Generated by ../../gen_data.py - do not edit.** What it is generated
 * *from* is `game.h`: the layout comes out of the DWARF a real compile of
 * that header produces, so every field, dimension, union arm and anonymous
 * member here is the one the C actually has, and the values come out of the
 * load image POPCORN.EXE unpacks to.
 *
 * This is what the port used to read the player's own POPCORN.EXE for. It
 * still can - `popcorn-dev --dump-image` runs the EXEPACK decoder, and
 * validate.py checks the two agree byte for byte - but nothing has to: the
 * levels, the sprites, the fonts and the scripts are here.
 *
 * The game's own 16-bit pointers are written as the fields they point at,
 * through the GOFF/AOFF/NOFF/ROFF macros in game.h, so the addresses are
 * computed from the layout rather than repeated from the file. The free
 * list's links are the clearest case: the chain the linker built is
 * `GOFF(entities[1])`, `GOFF(entities[2])` and so on, which says what it is
 * in a way 0x3154 does not.
 *
 * A field that is missing from an initializer is zero, which C guarantees and
 * this file leans on heavily - most of the image is zero, and `{0}` is a
 * clearer way of saying so than four thousand commas.
 */
#include "game.h"

const image_t popcorn_image_data = '''


def generate(img, poison=None):
    global FN_NAMES
    FN_NAMES = defines(r"_FN$")
    image_t = load_types()
    if image_t.size != len(img):
        raise SystemExit(f"gen_data: image_t is {image_t.size:#x} but the "
                         f"image is {len(img):#x}")
    em = Emitter(img, image_t)
    if poison is not None:
        em.img = bytearray(em.img)
        em.poison(image_t, 0, poison)
        em.poisoned = True
    body = em.emit(image_t, 0, 0)
    text = PREAMBLE + "\n".join(body) + ";\n"
    # The unmatched layout's header declares these and its accessors use
    # them; only here is image_t complete enough to work them out.
    text += ("\n#ifndef MATCH_MEMORY_LAYOUT\n"
             "/* Where each segment starts once the layout is the compiler's. */\n"
             "const int32_t popcorn_seg_assets = "
             "(int32_t)offsetof(image_t, seg_assets);\n"
             "const int32_t popcorn_seg_animations = "
             "(int32_t)offsetof(image_t, seg_animations);\n"
             "const int32_t popcorn_seg_runtime = "
             "(int32_t)offsetof(image_t, seg_runtime);\n"
             "#endif\n")
    return text


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="regenerate and report whether data.c is up to date, "
                         "without writing it")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--poison", nargs="?", const="0xcc", default=None,
                    metavar="BYTE",
                    help="fill every `_`-named field with this byte instead "
                         "of the image's. The layout is unchanged, so the "
                         "whole harness still runs against the result - and "
                         "anything that reads a byte no field covers now "
                         "shows it. pad_writes.py is the other half of this: "
                         "that one proves nothing writes the padding")
    args = ap.parse_args()

    sys.path.insert(0, HERE)
    from tools_dis import load_image
    text = generate(load_image(),
                    None if args.poison is None else int(args.poison, 0))

    if args.check:
        have = open(args.out).read() if os.path.exists(args.out) else None
        if have == text:
            print(f"{os.path.relpath(args.out, HERE)} is up to date "
                  f"({len(text):,} bytes)")
            return 0
        print(f"{os.path.relpath(args.out, HERE)} is stale", file=sys.stderr)
        return 1

    open(args.out, "w").write(text)
    print(f"{os.path.relpath(args.out, HERE)}: {len(text):,} bytes, "
          f"{text.count(chr(10)):,} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
