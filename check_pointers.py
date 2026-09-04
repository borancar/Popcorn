#!/usr/bin/env python3
"""Check how the port handles the game's own 16-bit pointers.

The program keeps pointers in its data - the entity chain's links, a cell
address parked in a slot, an animation cursor, a frame list - and they are
16-bit offsets into one of four segments, not C pointers.  The port has one
way in and one way out:

    global_off(p)      a C pointer -> the offset the game would have stored
    global_ptr(off)    that offset -> the pointer it means
    global_w(off)      the 16-bit value **at** an offset, which is how one of
                    the game's pointers is read out of its data

with entity_ptr, ball_ptr, hit_ptr and animations_ptr as the typed forms.  The
rules this checks are what keeps those honest:

  1. A field the game treats as a pointer is named `*_ptr`.
  2. Where a value stored into one came from.  **Informational**: the rule
     that only `global_off` makes a pointer is too narrow to be an error.  A
     pointer can be built out of the game's own data with no C pointer in
     sight - `animated_ptr[p] = group_ptr + (piece << 5)` is a frame inside a
     group, genuinely an offset into the animations block, and no `_off`
     could apply to it.  So this reports where the value came from and leaves
     the judgement.
  3. Reading one must go through a `_ptr` accessor.  A bare `g_image[x]` on
     a stored pointer is the same mistake with the arithmetic inlined.
  4. Casting one **to an integer** is a smell.  A pointer of the game's is
     sixteen bits wide by definition, so `(uint16_t)x_ptr` either says nothing
     or is covering for something else being the wrong width - a local
     declared uint32_t, most often.  Declare the local uint16_t and the cast
     goes.  Casting what global_ptr *returns* to a typed C pointer is a different
     thing and is fine.
  5. The only pointer constant is `END_PTR`.  An address written into a
     call - `global_ptr(0x6b9c)`, or a #define standing for one - is a place in
     the image that has not been given a field yet, and naming it is the fix.
  6. A `_fn` field holds a **routine's** address - something the game calls
     rather than reads - and only an `_FN` constant or another `_fn` is one.
     Constants are fine here, unlike for data pointers: a routine's address is
     fixed in the image and there is nothing to give it a field.  Casting one
     is the same smell as casting a pointer.
  7. What an accessor is called with must be **simple** - a name, a field, a
     subscript.  `global_w(table + index * 2)` is doing pointer arithmetic
     without a pointer type: `table` is an array of somethings, and the C
     that means it is `global_w(table[index])`.

  8. A **32-bit type has to earn it**.  An 8086 does 32-bit arithmetic in
     DX:AX and nowhere else, so a `uint32_t` or an `int32_t` in a transcribed
     routine is a claim: that the value really is wider than the register the
     original keeps it in.  Usually it is not, and the wide version *works* -
     offsets stay under 64K and positions under 256, so the truncation never
     happens and no test notices.  The two that are genuine are named in
     WIDE_OK below; everything else is either a C loop counter, a flag, or a
     register that should be as wide as the register.

  9. A mask is either the machine or the type, and only one of those is
     worth writing.  `& 0xff` and `& 0xffff` model a byte or a word register
     wrapping, which is real - the original adds 15 to a coordinate in AL and
     lets it carry away.  But once the value lives in a uint8_t or a uint16_t,
     the type does that, and the mask is the same truncation said twice.  Said
     twice is one of them being wrong later, so this reports every one and
     leaves the judgement: a mask on an **assignment** into a variable of that
     width is redundant; a mask on a **call argument** whose parameter is
     wider, or inside a **comparison**, is the wrap and stays.

 10. A cast to the type the target already has is the same mistake as a
     redundant mask, and usually left over from when one of them was wider.
     `b->anchor_x = (uint8_t)(b->x - 1)` where both are uint8_t says nothing
     the assignment does not: C truncates on the store.  This looks the width
     up rather than trusting the name, so a cast that really is narrowing -
     into a field one size down - stays unreported.

And one thing it reports without complaining about: `global_ptr(global_w(x))`, a
pointer **to** a pointer.  The game keeps tables of frame addresses, so a
cursor into one needs both steps and two indirections are right there - the
only place they are.  Worth seeing because getting it wrong is silent: one
step too few draws a frame's pixels as if they were an address, one too many
reads the address as if it were pixels.

Rule 4 is the one that finds real work.  Rules 2 and 3 are mostly quiet until
the fields are renamed, so the report ends with the candidates: the fields
whose values already flow into an accessor and which therefore want the
suffix.  That list is the rename, written down.

Stores are sorted rather than lumped, because a pointer does not only come
from global_off:

    made      global_off of a C pointer - the port deciding an address
    copied    global_w, another pointer field, an entry of a table of them:
              the game's own data handing one over
    advanced  the same field plus or minus something - a cursor stepping,
              and the arithmetic is on an offset, which is what makes it
              16-bit wrap-around and not C pointer arithmetic
    neither   no source this can recognise.  Worth a look, not a verdict:
              the value may be a perfectly good offset the checker has no way
              to see the provenance of

    uv run check_pointers.py                 # game.c and game.h
    uv run check_pointers.py --strict         # exit 1 if anything is flagged
    uv run check_pointers.py FILE...
"""
import argparse
import re
import os
import sys

from tree_sitter import Language, Parser
import tree_sitter_c

C = Language(tree_sitter_c.language())

# Resolve one of the game's stored offsets into something addressable.
PTR_FUNCS = {"global_ptr", "entity_ptr", "ball_ptr", "hit_ptr",
             "animations_ptr", "assets_ptr", "runtime_ptr"}
# Read or write a 16-bit value at an offset - which is how the game's own
# pointers are loaded and stored.
WORD_FUNCS = {"global_w", "animations_w", "global_setw", "runtime_w",
              "global_table_w"}
# The other direction.
OFF_FUNCS = {"global_off", "assets_off", "runtime_off", "animations_off"}

ACCESSORS = PTR_FUNCS | WORD_FUNCS

# Which segment an accessor resolves against.  A stored offset means nothing
# without one: the same 16-bit word is a different byte in each segment, and
# the four bases are tens of kilobytes apart.
#
# Note what this is **not**.  Where a field lives and what its value points
# into are independent, and the program relies on that: global.anim_ptr is a
# member of global_t whose value is an offset into the animations block, so
# `animations_w(global.anim_ptr)` is right, not a mix-up.  A field can also
# retarget - the same word holding an offset into one segment at one moment
# and another at another - so "this field always means segment X" is not a
# fact this can assume.  It reports what it saw and leaves the judgement.
SEGMENT_OF = {
    "global_ptr": "global", "global_w": "global", "global_setw": "global",
    "global_table_w": "global",
    "entity_ptr": "global", "ball_ptr": "global", "hit_ptr": "global",
    "animations_ptr": "animations", "animations_w": "animations",
    "assets_ptr": "assets",
    "runtime_ptr": "runtime", "runtime_w": "runtime",
    "vram_setw": "vram",
}

# The 32-bit values this program genuinely has, and why.  Everything else
# that is uint32_t or int32_t is either C scaffolding - a loop counter, a
# flag, an index into a buffer the port owns - or a register written wider
# than it is.
WIDE_OK = {
    "ticks":  "the BIOS counter at 0040:006C is a dword",
    "io_ticks": "likewise",
    "first":  "particle_height's imul, whose product is DX:AX before idiv",
    "prod":   "likewise",
}

# The argument that is an offset.  global_setw(off, value) writes the second.
OFFSET_ARG = {f: 0 for f in ACCESSORS}


def text(node, src):
    return src[node.start_byte:node.end_byte].decode("utf8", "replace")


def walk(node):
    yield node
    for child in node.children:
        yield from walk(child)


def is_simple(node, src):
    """A name, a field of one, or a subscript of one - and nothing computed.

    This is the shape an offset should arrive in.  `table[i]` is fine because
    the subscript says what `table` is; `table + i * 2` is the same address
    with the type thrown away.
    """
    k = node.type
    if k in ("identifier", "field_expression", "subscript_expression",
             "number_literal", "char_literal"):
        return True
    if k in ("parenthesized_expression", "cast_expression"):
        for c in node.named_children:
            if c.type not in ("type_descriptor", "primitive_type",
                              "type_identifier", "sized_type_specifier"):
                return is_simple(c, src)
        return True
    if k == "call_expression":              # global_ptr(global_w(x)) is two steps,
        return True                         # each of which is checked itself
    return False


def branches(node):
    """The values an expression can take.

    A ternary is two of them, and a rule about what may be stored holds only
    if it holds for **both** arms - `is_two ? ENTITY_CAPSULE_FN :
    ENTITY_POPUP_FN` stores a routine's address either way.  Everything else
    is one value, so this is the identity for it.

    It is also what an accessor's offset argument has to be checked through.
    `global_ptr(is_two ? 0x4e13 : 0x5863)` is the two calls
    `is_two ? global_ptr(0x4e13) : global_ptr(0x5863)` written short, and the
    interesting thing about it is not that the argument is compound - it is
    that each arm is an address nobody has named.  Reporting the ternary as
    one compound offset hides two pointer constants behind it.  Nested arms
    recurse, so a chain of ternaries is as many values as it has ends.
    """
    if node.type == "conditional_expression":
        arms = [node.child_by_field_name(f)
                for f in ("consequence", "alternative")]
        out = []
        for a in arms:
            if a is not None:
                out.extend(branches(a))     # a ternary inside a ternary
        if out:
            return out
    return [node]


def root_name(node, src):
    """The field or variable an expression is ultimately reading."""
    while True:
        k = node.type
        if k == "field_expression":
            f = node.child_by_field_name("field")
            return text(f, src) if f else None
        if k == "subscript_expression":
            node = node.child_by_field_name("argument")
            continue
        if k in ("parenthesized_expression", "cast_expression"):
            kids = [c for c in node.named_children
                    if c.type not in ("type_descriptor", "primitive_type",
                                      "type_identifier",
                                      "sized_type_specifier")]
            if not kids:
                return None
            node = kids[0]
            continue
        if k == "identifier":
            return text(node, src)
        return None


def enclosing_fn(node, src):
    """The function a node sits in, so two locals called `si` stay apart."""
    n = node.parent
    while n is not None:
        if n.type == "function_definition":
            d = n.child_by_field_name("declarator")
            while d is not None and d.type != "identifier":
                d = d.child_by_field_name("declarator")
            return text(d, src) if d is not None else None
        n = n.parent
    return None


def qualified_name(node, src):
    """A key that separates same-named things.

    root_name gives the leaf, and leaves collide: there are three different
    `script` fields, and `si` is a local in dozens of routines.  For a field
    this keeps the object it was read from, with subscripts flattened so
    `hits[i].cell` and `hits[j].cell` are one thing.  Locals are qualified by
    the function they live in.
    """
    while node.type in ("parenthesized_expression", "cast_expression"):
        kids = [c for c in node.named_children
                if c.type not in ("type_descriptor", "primitive_type",
                                  "type_identifier", "sized_type_specifier")]
        if not kids:
            return None
        node = kids[0]
    if node.type == "subscript_expression":
        arg = node.child_by_field_name("argument")
        return qualified_name(arg, src) if arg is not None else None
    if node.type == "field_expression":
        return re.sub(r"\[[^\]]*\]", "[]", text(node, src))
    if node.type == "identifier":
        fn = enclosing_fn(node, src)
        return "%s() %s" % (fn, text(node, src)) if fn else text(node, src)
    return None


def field_widths(paths):
    """Every field's width, so a mask can be judged against what it lands in."""
    out = {}
    for path in paths:
        text_ = open(path, "rb").read().decode("utf8", "replace")
        for m in re.finditer(r"\b(u?int(?:8|16|32)_t)\s+(\w+)\s*(?:\[[^;]*\])?;",
                             text_):
            out.setdefault(m.group(2), m.group(1))
    return out


def struct_fields(paths):
    """Every struct field name, so a candidate can be told from a local."""
    names = set()
    for path in paths:
        src = open(path, "rb").read()
        tree = Parser(C).parse(src)
        for n in walk(tree.root_node):
            if n.type != "field_declaration":
                continue
            for sub in walk(n):
                if sub.type == "field_identifier":
                    names.add(text(sub, src))
    return names


def fn_fields(paths):
    """Every struct field named `*_fn` - a routine the game calls through."""
    names = set()
    for path in paths:
        src = open(path, "rb").read()
        tree = Parser(C).parse(src)
        for n in walk(tree.root_node):
            if n.type != "field_declaration":
                continue
            for sub in walk(n):
                if sub.type == "field_identifier" \
                        and text(sub, src).endswith("_fn"):
                    names.add(text(sub, src))
    return names


def ptr_fields(paths):
    """Every struct field already named `*_ptr`."""
    names = set()
    for path in paths:
        src = open(path, "rb").read()
        tree = Parser(C).parse(src)
        for n in walk(tree.root_node):
            if n.type != "field_declaration":
                continue
            for d in n.named_children:
                for sub in walk(d):
                    if sub.type == "field_identifier":
                        name = text(sub, src)
                        if name.endswith("_ptr"):
                            names.add(name)
    return names


def check(path, known_ptr_fields, known_fn_fields, findings, candidates,
          segments, all_fields, wide, wide_loops, masks, casts, all_widths):
    src = open(path, "rb").read()
    tree = Parser(C).parse(src)
    rel = os.path.relpath(path)

    def add(rule, node, msg):
        findings.append((rule, rel, node.start_point[0] + 1, msg))

    # Rule 8: every 32-bit type, so each one is a decision rather than a
    # default.  A `for` counter is C scaffolding and says nothing about the
    # machine, so those are counted and not listed - what is left is the
    # worklist.
    for n in walk(tree.root_node):
        if n.type not in ("declaration", "parameter_declaration",
                          "function_definition"):
            continue
        t = n.child_by_field_name("type")
        if t is None or text(t, src) not in ("uint32_t", "int32_t"):
            continue
        if n.type == "declaration" and n.parent is not None \
                and n.parent.type == "for_statement":
            wide_loops[0] += 1
            continue
        who = None
        for sub in walk(n):
            if sub.type == "identifier":
                who = text(sub, src)
                break
        if who is None or who in WIDE_OK:
            continue
        kind = {"parameter_declaration": "parameter",
                "function_definition": "return",
                "declaration": "local"}[n.type]
        wide.setdefault(who, set()).add(
            "%s:%d %s %s" % (rel, n.start_point[0] + 1, text(t, src), kind))

    # Rule 9: every 0xff and 0xffff mask, split by whether the type it lands
    # in already does the truncation. The widths come from the declarations
    # this file and the header make, leaf name only - a collision costs an
    # entry in the wrong half of the list, never a missed one.
    deref_width = {}
    for raw in (open("reconstruct/src/game.h", "rb").read()
                .decode("utf8", "replace").split("\n")):
        d = re.match(r"\s*(?:static\s+inline\s+)?(u?int(?:8|16|32)_t)\s*\*"
                     r"\s*(\w+)\s*\(", raw)
        if d:
            deref_width[d.group(2)] = d.group(1)

    # A pointer local carries the width of what a store through it lands in:
    # `uint8_t *name` makes `*name = ...` a byte store. The declaration regex
    # below wants `type name` and cannot see the `*`, which is why the first
    # widening of rule 10 still missed `*name++ = (uint8_t)c`.
    for raw in src.decode("utf8", "replace").split("\n"):
        for d in re.finditer(r"\b(?:const\s+)?(u?int(?:8|16|32)_t)\s*\*"
                             r"\s*(\w+)", raw):
            seen = deref_width.get(d.group(2), d.group(1))
            deref_width[d.group(2)] = (d.group(1) if seen == d.group(1)
                                       else "?")

    local_width = {}
    for raw in src.decode("utf8", "replace").split("\n"):
        # Parameters count as declarations: `ending_walk(..., uint16_t dx)`
        # is where dx's width is written down, and matching only the start of
        # the line found the return type instead and left dx to collide with
        # a uint8_t of the same name elsewhere.
        for d in re.finditer(r"\b(?:const\s+)?(u?int(?:8|16|32)_t)\s+(\w+)",
                             raw):
            seen = local_width.get(d.group(2), d.group(1))
            # A leaf name declared at two widths is not something this can
            # resolve without scoping, and guessing is worse than declining:
            # `rows` is a uint8_t in one routine and a uint16_t in another,
            # and taking the first turned a real mask into a false positive.
            local_width[d.group(2)] = (d.group(1) if seen == d.group(1)
                                       else "?")
    for f, t in all_widths.items():
        local_width.setdefault(f, t)
    for i, raw in enumerate(src.decode("utf8", "replace").split("\n"), 1):
        line = raw.split("/*")[0]
        for m in re.finditer(r"&\s*(0xff|0xffff)\b", line):
            bits = m.group(1)
            before = line[:m.start()]
            lhs = re.match(r"\s*(?:const\s+)?(?:(u?int(?:8|16|32)_t)\s+)?"
                           r"([\w.\[\]>-]+)\s*(?:=|[-+|&^]=)", before)
            kind = "in a comparison or an argument - the wrap, and it stays"
            # Only the *outermost* operation is the type's job. A mask on an
            # inner term is splitting a word - `(ticks & 0xff) + (ticks >> 8)`
            # takes the low half before adding the high one - and no width on
            # the left does that.
            tail = line[m.end():].strip()
            outermost = tail in (";", ")", "");
            # A mask need not be the outermost operation to be the store's
            # job twice - it only has to be that nothing between it and the
            # store can bring the discarded bits back. `+ - * <<` and the
            # bitwise operators are all congruence-preserving mod 2^n, so
            # `target = 80 - ((bl << 3) & 0xff)` truncates the same without
            # it. `>> / %` and a comparison are not, which is why the plain
            # outermost test is kept as the other way in.
            if lhs and not outermost and tail.rstrip(") ;") == "":
                rhs = line[lhs.end():]
                if not re.search(r">>|/|%|[<>=!]=|\?|,|\w\s*\(", rhs):
                    outermost = True
            if lhs and outermost:
                want = "uint8_t" if bits == "0xff" else "uint16_t"
                declared = lhs.group(1) or local_width.get(lhs.group(2))
                if declared == want:
                    kind = ("assigned into a %s, which does this already"
                            % want)
            # And the case the two halves above both miss, because it is
            # about neither of them: the *operand* is already that narrow, so
            # the mask does nothing wherever it sits. An array subscript is
            # how these turned up - `hole_picture[row & 0xff]` with row a
            # uint8_t - and a subscript is not an assignment, a comparison or
            # an argument. A right shift only ever loses bits, so `(x >> 2)`
            # is as narrow as x.
            op = re.search(r"(?:\(\s*(\w+)\s*>>\s*(\d+)\s*\)|(\w+))\s*$",
                           before.rstrip())
            if op:
                w = local_width.get(op.group(1) or op.group(3))
                have = {"uint8_t": 8, "int8_t": 8, "uint16_t": 16,
                        "int16_t": 16, "uint32_t": 32, "int32_t": 32}.get(w)
                if have is not None:
                    # A right shift only loses bits, so the operand is that
                    # much narrower than its type.
                    have -= int(op.group(2) or 0)
                    if have <= (8 if bits == "0xff" else 16):
                        kind = ("on %d bits of a %s, which cannot reach it"
                                % (max(have, 0), w))
            masks.setdefault("%s %s" % (bits, kind), set()).add(
                "%s:%d %s" % (rel, i, line.strip()[:56]))

    # Rule 10: a cast to the width the target already has.
    for i, raw in enumerate(src.decode("utf8", "replace").split("\n"), 1):
        line = raw.split("/*")[0]
        # A store **through a pointer** is a target too, and the first version
        # of this could not see one: its left-hand side allowed a name, a
        # field and a subscript but neither a `*` nor a call, so
        # `*global_ptr(cell_ptr) = (uint8_t)to` went unreported. The pointer's
        # own declaration carries the width.
        m = re.search(r"^\s*\*\s*(?:\+\+)?(\w+)\s*(?:\+\+|--)?\s*="
                      r"\s*\((u?int(?:8|16|32)_t)\)", line)
        if m and deref_width.get(m.group(1)) == m.group(2):
            casts.setdefault(m.group(2), set()).add(
                "%s:%d %s" % (rel, i, line.strip()[:58]))
            continue
        m = re.search(r"^\s*\*\s*(\w+)\s*\([^()]*\)\s*="
                      r"\s*\((u?int(?:8|16|32)_t)\)", line)
        if m and deref_width.get(m.group(1)) == m.group(2):
            casts.setdefault(m.group(2), set()).add(
                "%s:%d %s" % (rel, i, line.strip()[:58]))
            continue
        m = re.search(r"^\s*(?:(u?int(?:8|16|32)_t)\s+)?"
                      r"([\w.\[\]>-]+)\s*=\s*\((u?int(?:8|16|32)_t)\)", line)
        if not m:
            continue
        target = m.group(1) or local_width.get(
            m.group(2).split(".")[-1].split(">")[-1].split("[")[0].lstrip("-"))
        if target and target == m.group(3):
            casts.setdefault(m.group(3), set()).add(
                "%s:%d %s" % (rel, i, line.strip()[:58]))

    for n in walk(tree.root_node):
        if n.type == "call_expression":
            fn = n.child_by_field_name("function")
            args = n.child_by_field_name("arguments")
            if fn is None or args is None or fn.type != "identifier":
                continue
            name = text(fn, src)
            real = [a for a in args.named_children if a.type != "comment"]
            # An accessor's own body is where the arithmetic is *defined* -
            # global_table_w exists precisely so that `base + i * 2` is
            # written once. Complaining there would be asking it to call
            # something that does not exist.
            if enclosing_fn(n, src) in ACCESSORS:
                continue
            if name in ACCESSORS and real:
                # One accessor call, but the offset it is handed may be a
                # ternary - which is two calls written short. Check each value
                # it can take, so `global_ptr(a ? X : Y)` reports what
                # `a ? global_ptr(X) : global_ptr(Y)` would.
                for arg in branches(real[OFFSET_ARG[name]]):
                    if not is_simple(arg, src):
                        add("compound-offset", arg,
                            "%s(%s) - the offset is computed in the call; "
                            "give the thing it indexes a type and subscript it"
                            % (name, text(arg, src)))
                    if is_constant(arg, src):
                        add("pointer-constant", arg,
                            "%s(%s) - the only pointer constant this program "
                            "has is END_PTR. An address written into a "
                            "call is a field of the struct that has not been "
                            "named yet" % (name, text(arg, src)))

                    inner = inner_word_call(arg, src)
                    if name in PTR_FUNCS and inner is not None:
                        add("pointer-to-pointer", n,
                            "%s - a pointer **to** a pointer: %s is where the "
                            "game keeps one, the word there is another, and "
                            "this resolves that. Two of its indirections, not "
                            "one" % (text(n, src), inner))

                    who = root_name(arg, src)

                    # Which segment this stored offset was resolved against.
                    # Keyed by a qualified name: the leaf alone collides, and
                    # a collision reads as a field meaning two segments at
                    # once.
                    seg = SEGMENT_OF.get(name)
                    qual = qualified_name(arg, src)
                    if qual and seg and not qual.startswith((
                            "global_ptr()", "animations_ptr()",
                            "runtime_ptr()", "assets_ptr()", "global_w()",
                            "animations_w()", "runtime_w()",
                            "global_setw()")):
                        segments.setdefault(qual, {}).setdefault(
                            seg, set()).add(
                            "%s:%d %s" % (rel, arg.start_point[0] + 1, name))

                    if who and not who.endswith("_ptr") and who not in (
                            "g_image", "off", "si", "di", "bx"):
                        candidates.setdefault(who, set()).add(
                            "%s:%d %s" % (rel, arg.start_point[0] + 1, name))

        if n.type == "assignment_expression":
            lhs = n.child_by_field_name("left")
            rhs = n.child_by_field_name("right")
            if lhs is None or rhs is None:
                continue
            op = n.child_by_field_name("operator")
            compound = op is not None and text(op, src) != "="
            stmt = " ".join(text(n, src).split())

            field = root_name(lhs, src)
            if field in known_fn_fields:
                names = [root_name(a, src) for a in branches(rhs)]
                ok = all(r is not None and (r.endswith("_FN")
                                            or r.endswith("_fn"))
                         for r in names)
                if not ok:
                    add("routine-from-elsewhere", n,
                        "%s - a `_fn` holds a routine's address, and the "
                        "only things that are one are an _FN constant or "
                        "another _fn" % stmt)

            if field in known_ptr_fields:
                # Pointer-ness travels. If one of the game's pointers is
                # copied out of another field, that field holds one too, and
                # the name has to say so - `sprite.frame_ptr = kind->frame`
                # is the shape, and it is how a whole chain stays unnamed
                # because only its last link was looked at.
                for arm in branches(rhs):
                    rname = root_name(arm, src)
                    if (rname and rname in all_fields
                            and not rname.endswith("_ptr")
                            and not rname.endswith("_fn")):
                        add("pointer-from-unnamed", n,
                            "%s - `%s` holds one of the game's pointers, "
                            "because %s does and this is where it comes from. "
                            "It wants the suffix too"
                            % (stmt, rname, field))
                        candidates.setdefault(rname, set()).add(
                            "%s:%d into %s"
                            % (rel, n.start_point[0] + 1, field))

                how = ("advanced" if compound
                       else classify_store(rhs, field, known_ptr_fields, src))
                if how == "made":
                    pass                # global_off: a C pointer written down
                elif how == "copied":
                    add("pointer-from-data", n,
                        "%s - a pointer taken from the game's own data "
                        "rather than made with global_off. Legitimate, and worth "
                        "seeing: it is where one of its pointers comes from"
                        % stmt)
                elif how == "terminator":
                    pass                # 0xffff ends a chain. Zero cannot:
                                        # zero is a real offset here
                elif how == "advanced":
                    add("pointer-advanced", n,
                        "%s - a cursor stepped. The arithmetic is on an "
                        "offset, which is what makes it 16-bit wrap-around "
                        "rather than C pointer arithmetic"
                        % stmt)
                else:
                    add("store-without-global_off", n,
                        "%s - a plain number put where one of the game's "
                        "pointers goes" % stmt)
            elif field and not field.endswith("_ptr"):
                # A field **assigned from** an `_off` accessor is one of the
                # game's pointers as surely as one read through a `_ptr` one -
                # global_off produces an offset and nothing else.  Reading was
                # the only evidence collected, so a field only ever written
                # here and read somewhere this cannot see stayed invisible.
                how = (None if compound
                       else classify_store(rhs, field, known_ptr_fields, src))
                if how == "made":
                    candidates.setdefault(field, set()).add(
                        "%s:%d store" % (rel, n.start_point[0] + 1))

        if n.type == "declaration":
            # A local declared wider than the thing it is given.  One of the
            # game's pointers is sixteen bits; widening it here is what makes
            # the cast on the way back into a `_ptr` field look necessary, so
            # the two findings are one defect seen from both ends.
            ty = n.child_by_field_name("type")
            tyname = text(ty, src) if ty is not None else ""
            if tyname in ("uint32_t", "uint64_t", "size_t"):
                for d in n.named_children:
                    if d.type != "init_declarator":
                        continue
                    val = d.child_by_field_name("value")
                    who = d.child_by_field_name("declarator")
                    if val is None or who is None:
                        continue
                    src_name = root_name(val, src)
                    from_ptr = (src_name is not None
                                and (src_name.endswith("_ptr")
                                     or src_name in known_ptr_fields))
                    # `_off` returns an offset and nothing else.  A `_w`
                    # read is deliberately **not** here: it returns a word,
                    # which is a pointer only sometimes - `rows` and `word`
                    # come out of one and are neither.
                    fn = (val.child_by_field_name("function")
                          if val.type == "call_expression" else None)
                    from_off = fn is not None and text(fn, src) in OFF_FUNCS
                    if from_ptr or from_off:
                        add("pointer-widened", d,
                            "%s %s = %s - one of the game's pointers is "
                            "sixteen bits, and a wider local is what makes "
                            "the cast putting it back look necessary"
                            % (tyname, text(who, src), text(val, src)))

        if n.type == "cast_expression":
            val0 = n.child_by_field_name("value")
            if val0 is not None:
                fnname = next((text(k, src) for k in walk(val0)
                               if k.type in ("identifier", "field_identifier")
                               and (text(k, src).endswith("_fn")
                                    or text(k, src).endswith("_FN"))), None)
                if fnname:
                    add("cast-of-a-routine", n,
                        "%s - a routine's address has the width the game gave "
                        "it; casting it says nothing" % text(n, src))

            typ = n.child_by_field_name("type")
            to_pointer = typ is not None and any(
                k.type in ("pointer_declarator", "abstract_pointer_declarator")
                for k in walk(typ))
            val = n.child_by_field_name("value")
            # A cast of what a **call returned** is not a cast of a pointer,
            # even when a pointer went into the call: `(int16_t)global_w(p)`
            # reinterprets the word that came back, which is how a signed
            # velocity is read, and `(uint8_t)global_w(p)` takes its low byte.
            # Walking the whole operand for a `_ptr` name found the argument
            # and called both of those a fault.
            # A `_ptr` that appears only as an **argument** does not make
            # this a cast of a pointer: the cast applies to what came back.
            # `(int16_t)global_w(p)` reads a signed word and
            # `(uint8_t)(global_w(p) >> 8)` its high byte, and both were being
            # reported because the operand was walked for the name without
            # asking where in it the name sat.
            called = []
            if val is not None:
                called = [(k.start_byte, k.end_byte)
                          for k in walk(val) if k.type == "call_expression"]

            def only_an_argument(k):
                return any(a <= k.start_byte and k.end_byte <= b
                           for a, b in called)

            if val is not None and not to_pointer:
                who = next((text(k, src) for k in walk(val)
                            if k.type in ("identifier", "field_identifier")
                            and text(k, src).endswith("_ptr")
                            and not only_an_argument(k)), None)
                if who and enclosing_fn(n, src) not in ACCESSORS:
                    add("cast-of-a-pointer", n,
                        "%s - one of the game's pointers is 16 bits wide by "
                        "definition; a cast either says nothing or is hiding "
                        "that something else is the wrong width"
                        % text(n, src))

        if n.type == "subscript_expression":
            base = n.child_by_field_name("argument")
            idx = n.child_by_field_name("index")
            if base is None or idx is None:
                continue
            if text(base, src) != "g_image":
                continue
            who = root_name(idx, src)
            if who in known_ptr_fields:
                add("read-without-ptr", n,
                    "g_image[%s] - reading through one of the game's "
                    "pointers without a _ptr accessor" % text(idx, src))


def is_constant(node, src):
    """A literal address, or a #define standing for one.

    END_PTR is the exception and the only one: it is what ends a chain
    and it is not an address.  Everything else written as a constant where a
    pointer goes is a place in the image with no name.
    """
    while node.type in ("cast_expression", "parenthesized_expression"):
        kids = [c for c in node.named_children
                if c.type not in ("type_descriptor", "primitive_type",
                                  "type_identifier", "sized_type_specifier")]
        if not kids:
            return False
        node = kids[0]
    if node.type == "number_literal":
        return True
    if node.type == "identifier":
        name = text(node, src)
        return name != "END_PTR" and name.isupper() and "_" in name + "_"
    return False


def inner_word_call(node, src):
    """`global_ptr(global_w(x))` - the x, if this is a pointer read through one.

    The game keeps tables of pointers, so a cursor into one is a pointer to a
    pointer and needs both steps.  Worth seeing rather than fixing: the shape
    is correct, and it is the only place two indirections are right.
    """
    while node.type in ("cast_expression", "parenthesized_expression"):
        kids = [c for c in node.named_children
                if c.type not in ("type_descriptor", "primitive_type",
                                  "type_identifier", "sized_type_specifier")]
        if not kids:
            return None
        node = kids[0]
    if node.type != "call_expression":
        return None
    fn = node.child_by_field_name("function")
    args = node.child_by_field_name("arguments")
    if fn is None or args is None or text(fn, src) not in WORD_FUNCS:
        return None
    real = [a for a in args.named_children if a.type != "comment"]
    return text(real[0], src) if real else "?"


def classify_store(rhs, field, known, src):
    """Where a value stored into one of the game's pointers came from.

    `made`     global_off of a C pointer - the port deciding an address
    `copied`   global_w, or another pointer field, or an entry of a table of
               them: the game's own data handing one over
    `advanced` the same field plus or minus something - a cursor stepping
    otherwise  a bare number, which is the thing worth catching
    """
    node = rhs
    # Either arm of a ternary has to stand on its own, and the store is only
    # as well-founded as the weaker of the two.
    arms = branches(node)
    if len(arms) > 1:
        kinds = [classify_store(a, field, known, src) for a in arms]
        if any(k is None for k in kinds):
            return None
        return kinds[0] if len(set(kinds)) == 1 else "copied"

    while node.type in ("cast_expression", "parenthesized_expression"):
        kids = [c for c in node.named_children
                if c.type not in ("type_descriptor", "primitive_type",
                                  "type_identifier", "sized_type_specifier")]
        if not kids:
            return None
        node = kids[0]

    if node.type == "call_expression":
        fn = node.child_by_field_name("function")
        if fn is not None:
            name = text(fn, src)
            if name in OFF_FUNCS:
                return "made"
            if name in WORD_FUNCS:
                return "copied"
    if node.type == "binary_expression":
        # A pointer with something added is still a pointer.  The obvious case
        # is a cursor stepping itself, but the value can equally have come
        # from another field, or out of another segment with its `_w`:
        # `group_ptr + (piece << 5)` is a frame inside a group, and there is
        # no `_off` that could apply to it because no C pointer was ever
        # involved - the offset came from the game's own data.
        for side in (node.child_by_field_name("left"),
                     node.child_by_field_name("right")):
            if side is None:
                continue
            who = root_name(side, src)
            if who == field or (who and (who.endswith("_ptr") or who in known)):
                return "advanced"
            inner = side
            while inner.type in ("cast_expression", "parenthesized_expression"):
                kids = [c for c in inner.named_children
                        if c.type not in ("type_descriptor", "primitive_type",
                                          "type_identifier",
                                          "sized_type_specifier")]
                if not kids:
                    break
                inner = kids[0]
            if inner.type == "call_expression":
                fn = inner.child_by_field_name("function")
                if fn is not None and text(fn, src) in WORD_FUNCS:
                    return "advanced"
    lit = text(node, src)
    if lit == "END_PTR" or (node.type == "number_literal"
                                 and lit.lower() in ("0xffff", "65535")):
        return "terminator"             # what ends a chain, not a pointer
    who = root_name(node, src)
    if who and (who.endswith("_ptr") or who in known):
        return "copied"
    if node.type == "subscript_expression":
        return "copied"                 # a table of the game's pointers
    return None


def is_cast_of_off(node, src):
    """`(uint16_t)global_off(x)` is still an global_off."""
    while node.type in ("cast_expression", "parenthesized_expression"):
        kids = [c for c in node.named_children
                if c.type not in ("type_descriptor", "primitive_type",
                                  "type_identifier", "sized_type_specifier")]
        if not kids:
            return False
        node = kids[0]
    fn = (node.child_by_field_name("function")
          if node.type == "call_expression" else None)
    return fn is not None and text(fn, src) in OFF_FUNCS


def main():
    ap = argparse.ArgumentParser(
        description="Check the port's handling of the game's 16-bit pointers.")
    ap.add_argument("files", nargs="*",
                    default=["reconstruct/src/game.c",
                             "reconstruct/src/game.h"])
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when anything is flagged")
    ap.add_argument("--candidates", action="store_true",
                    help="only the fields that want a _ptr suffix")
    ap.add_argument("--segments", action="store_true",
                    help="only the segment each stored offset is read against")
    a = ap.parse_args()

    missing = [f for f in a.files if not os.path.exists(f)]
    if missing:
        raise SystemExit("no such file: " + ", ".join(missing))

    known = ptr_fields(a.files)
    known_fn = fn_fields(a.files)
    fields = struct_fields(a.files)
    findings, candidates, segments = [], {}, {}
    wide, wide_loops, masks, casts = {}, [0], {}, {}
    all_widths = field_widths(a.files)
    for f in a.files:
        check(f, known, known_fn, findings, candidates, segments, fields,
              wide, wide_loops, masks, casts, all_widths)

    # A stored offset read against more than one segment.  Informational: it
    # can be right - a field that legitimately retargets - but at most one
    # reading is right at any one moment, so each of these is a place to look.
    mixed = {k: v for k, v in segments.items() if len(v) > 1}
    if a.segments or (mixed and not a.candidates):
        print("== a stored offset resolved against more than one segment (%d)"
              % len(mixed))
        print("   informational: where a field lives and what its value points")
        print("   into are different things, and a field may retarget - but at")
        print("   most one of these readings is right at any one moment")
        for who in sorted(mixed):
            print("  %s" % who)
            for seg in sorted(mixed[who]):
                where = sorted(mixed[who][seg])
                print("      %-10s %s%s" % (seg, where[0],
                                            " ..." if len(where) > 1 else ""))
        print()
    if a.segments:
        return

    if not a.candidates:
        by_rule = {}
        for rule, path, line, msg in findings:
            by_rule.setdefault(rule, []).append((path, line, msg))
        order = ["routine-from-elsewhere",
                 "cast-of-a-routine", "pointer-constant",
                 "cast-of-a-pointer", "pointer-from-unnamed",
                 "pointer-widened", "compound-offset",
                 "store-without-global_off",
                 "pointer-advanced", "pointer-from-data",
                 "pointer-to-pointer"]
        # Reported, not complained about: each of these has a shape that is
        # right often enough that calling it a fault would train the reader
        # to skip the report.
        info = {"store-without-global_off", "pointer-advanced",
                "pointer-from-data", "pointer-to-pointer"}
        for rule in sorted(by_rule, key=lambda r: (order.index(r)
                                                   if r in order else 99, r)):
            hits = by_rule[rule]
            print("== %s (%d)%s" % (rule, len(hits),
                                    "  [info]" if rule in info else ""))
            for path, line, msg in hits:
                print("  %s:%d  %s" % (path, line, msg))
            print()

    struct_side = {k: v for k, v in candidates.items() if k in fields}
    local_side = {k: v for k, v in candidates.items() if k not in fields}

    def report(title, note, group):
        print("== %s (%d)" % (title, len(group)))
        print("   %s" % note)
        for who in sorted(group):
            where = sorted(group[who])
            print("  %-22s %d use%s   %s%s"
                  % (who, len(where), "" if len(where) == 1 else "s",
                     where[0], " ..." if len(where) > 1 else ""))
        print()

    report("struct fields feeding an accessor",
           "the game stores a pointer here and the name does not say so - "
           "this list is the rename", struct_side)
    report("locals feeding an accessor",
           "not the rename, but each one is a pointer the routine is "
           "holding in an integer", local_side)

    # The rule's reasoning is about transcribed code. The platform layer is
    # the port's own and answers to the host: milliseconds, cycle counts and
    # a palette are as wide as they need to be.
    def is_platform(uses):
        return all("sdl_io.c" in u or " io_" in u or " g_" in u for u in uses)

    transcribed = {k: v for k, v in wide.items()
                   if not (k.startswith(("io_", "g_")) or is_platform(v))}
    platform = {k: v for k, v in wide.items() if k not in transcribed}

    report("32-bit where the machine has 16",
           "an 8086 does 32-bit arithmetic in DX:AX and nowhere else, so "
           "each of these is a claim to check - %d `for` counters are C "
           "scaffolding and not listed" % wide_loops[0], transcribed)
    report("masks",
           "a byte or word register wrapping is real; the same truncation "
           "written where the type already does it is not - the two halves "
           "below say which is which", masks)

    report("casts to the width the target already has",
           "C truncates on the store, so the cast is that said twice - and "
           "the width is looked up, so a cast that really narrows is not "
           "here", casts)

    report("32-bit in the platform layer",
           "the port's own code rather than a transcribed register - "
           "milliseconds, cycles and a palette are as wide as the host "
           "wants them, and this list is here to stay small rather than to "
           "reach zero", platform)

    print("\n%d flagged, %d fields named _ptr, %d named _fn"
          % (len(findings), len(known), len(known_fn)))
    if a.strict and findings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
