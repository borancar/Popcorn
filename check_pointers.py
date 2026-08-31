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
  5. The only pointer constant is `SENTINEL_PTR`.  An address written into a
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
WORD_FUNCS = {"global_w", "animations_w", "global_setw", "runtime_w"}
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
    "entity_ptr": "global", "ball_ptr": "global", "hit_ptr": "global",
    "animations_ptr": "animations", "animations_w": "animations",
    "assets_ptr": "assets",
    "runtime_ptr": "runtime", "runtime_w": "runtime",
    "vram_setw": "vram",
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
    """
    if node.type == "conditional_expression":
        arms = [node.child_by_field_name(f)
                for f in ("consequence", "alternative")]
        arms = [a for a in arms if a is not None]
        if arms:
            return arms
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
          segments, all_fields):
    src = open(path, "rb").read()
    tree = Parser(C).parse(src)
    rel = os.path.relpath(path)

    def add(rule, node, msg):
        findings.append((rule, rel, node.start_point[0] + 1, msg))

    for n in walk(tree.root_node):
        if n.type == "call_expression":
            fn = n.child_by_field_name("function")
            args = n.child_by_field_name("arguments")
            if fn is None or args is None or fn.type != "identifier":
                continue
            name = text(fn, src)
            real = [a for a in args.named_children if a.type != "comment"]
            if name in ACCESSORS and real:
                arg = real[OFFSET_ARG[name]]
                if not is_simple(arg, src):
                    add("compound-offset", arg,
                        "%s(%s) - the offset is computed in the call; give "
                        "the thing it indexes a type and subscript it"
                        % (name, text(arg, src)))
                if is_constant(arg, src):
                    add("pointer-constant", arg,
                        "%s(%s) - the only pointer constant this program has "
                        "is SENTINEL_PTR. An address written into a call is a "
                        "field of the struct that has not been named yet"
                        % (name, text(arg, src)))

                inner = inner_word_call(arg, src)
                if name in PTR_FUNCS and inner is not None:
                    add("pointer-to-pointer", n,
                        "%s - a pointer **to** a pointer: %s is where the "
                        "game keeps one, the word there is another, and this "
                        "resolves that. Two of its indirections, not one"
                        % (text(n, src), inner))

                who = root_name(arg, src)

                # Which segment this stored offset was resolved against.
                # Keyed by a qualified name: the leaf alone collides, and a
                # collision reads as a field meaning two segments at once.
                seg = SEGMENT_OF.get(name)
                qual = qualified_name(arg, src)
                if qual and seg and not qual.startswith(("global_ptr()",
                                                         "animations_ptr()",
                                                         "runtime_ptr()",
                                                         "assets_ptr()",
                                                         "global_w()",
                                                         "animations_w()",
                                                         "runtime_w()",
                                                         "global_setw()")):
                    segments.setdefault(qual, {}).setdefault(seg, set()).add(
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

            field = root_name(lhs, src)
            if field in known_fn_fields:
                names = [root_name(a, src) for a in branches(rhs)]
                ok = all(r is not None and (r.endswith("_FN")
                                            or r.endswith("_fn"))
                         for r in names)
                if not ok:
                    add("routine-from-elsewhere", n,
                        "%s = %s - a `_fn` holds a routine's address, and the "
                        "only things that are one are an _FN constant or "
                        "another _fn" % (text(lhs, src), text(rhs, src)))

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
                            "%s = %s - `%s` holds one of the game's pointers, "
                            "because %s does and this is where it comes from. "
                            "It wants the suffix too"
                            % (text(lhs, src), text(rhs, src), rname, field))
                        candidates.setdefault(rname, set()).add(
                            "%s:%d into %s"
                            % (rel, n.start_point[0] + 1, field))

                how = ("advanced" if compound
                       else classify_store(rhs, field, known_ptr_fields, src))
                if how == "made":
                    pass                # global_off: a C pointer written down
                elif how == "copied":
                    add("pointer-from-data", n,
                        "%s = %s - a pointer taken from the game's own data "
                        "rather than made with global_off. Legitimate, and worth "
                        "seeing: it is where one of its pointers comes from"
                        % (text(lhs, src), text(rhs, src)))
                elif how == "terminator":
                    pass                # 0xffff ends a chain. Zero cannot:
                                        # zero is a real offset here
                elif how == "advanced":
                    add("pointer-advanced", n,
                        "%s = %s - a cursor stepped. The arithmetic is on an "
                        "offset, which is what makes it 16-bit wrap-around "
                        "rather than C pointer arithmetic"
                        % (text(lhs, src), text(rhs, src)))
                else:
                    add("store-without-global_off", n,
                        "%s = %s - a plain number put where one of the game's "
                        "pointers goes" % (text(lhs, src), text(rhs, src)))
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
            if val is not None and not to_pointer:
                who = next((text(k, src) for k in walk(val)
                            if k.type in ("identifier", "field_identifier")
                            and text(k, src).endswith("_ptr")), None)
                if who:
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

    SENTINEL_PTR is the exception and the only one: it is what ends a chain
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
        return name != "SENTINEL_PTR" and name.isupper() and "_" in name + "_"
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
    if lit == "SENTINEL_PTR" or (node.type == "number_literal"
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
    for f in a.files:
        check(f, known, known_fn, findings, candidates, segments, fields)

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

    print("\n%d flagged, %d fields named _ptr, %d named _fn"
          % (len(findings), len(known), len(known_fn)))
    if a.strict and findings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
