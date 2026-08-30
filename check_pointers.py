#!/usr/bin/env python3
"""Check how the port handles the game's own 16-bit pointers.

The program keeps pointers in its data - the entity chain's links, a cell
address parked in a slot, an animation cursor, a frame list - and they are
16-bit offsets into one of four segments, not C pointers.  The port has one
way in and one way out:

    img_off(p)      a C pointer -> the offset the game would have stored
    img_ptr(off)    that offset -> the pointer it means
    img_w(off)      the 16-bit value **at** an offset, which is how one of
                    the game's pointers is read out of its data

with entity_ptr, ball_ptr, hit_ptr and s14a1_ptr as the typed forms.  The
rules this checks are what keeps those honest:

  1. A field the game treats as a pointer is named `*_ptr`.
  2. Storing into one must go through `img_off`.  Anything else is a number
     being put where a pointer goes.
  3. Reading one must go through a `_ptr` accessor.  A bare `g_image[x]` on
     a stored pointer is the same mistake with the arithmetic inlined.
  4. What an accessor is called with must be **simple** - a name, a field, a
     subscript.  `img_w(table + index * 2)` is doing pointer arithmetic
     without a pointer type: `table` is an array of somethings, and the C
     that means it is `img_w(table[index])`.

And one thing it reports without complaining about: `img_ptr(img_w(x))`, a
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
from img_off:

    made      img_off of a C pointer - the port deciding an address
    copied    img_w, another pointer field, an entry of a table of them:
              the game's own data handing one over
    advanced  the same field plus or minus something - a cursor stepping,
              and the arithmetic is on an offset, which is what makes it
              16-bit wrap-around and not C pointer arithmetic
    neither   a bare number put where a pointer goes, which is the catch

    uv run check_pointers.py                 # game.c and game.h
    uv run check_pointers.py --strict         # exit 1 if anything is flagged
    uv run check_pointers.py FILE...
"""
import argparse
import os
import sys

from tree_sitter import Language, Parser
import tree_sitter_c

C = Language(tree_sitter_c.language())

# Resolve one of the game's stored offsets into something addressable.
PTR_FUNCS = {"img_ptr", "entity_ptr", "ball_ptr", "hit_ptr", "s14a1_ptr"}
# Read or write a 16-bit value at an offset - which is how the game's own
# pointers are loaded and stored.
WORD_FUNCS = {"img_w", "s14a1_w", "img_setw"}
# The other direction.
OFF_FUNCS = {"img_off"}

ACCESSORS = PTR_FUNCS | WORD_FUNCS

# The argument that is an offset.  img_setw(off, value) writes the second.
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
    if k == "call_expression":              # img_ptr(img_w(x)) is two steps,
        return True                         # each of which is checked itself
    return False


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


def check(path, known_ptr_fields, findings, candidates):
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
                inner = inner_word_call(arg, src)
                if name in PTR_FUNCS and inner is not None:
                    add("pointer-to-pointer", n,
                        "%s - a pointer **to** a pointer: %s is where the "
                        "game keeps one, the word there is another, and this "
                        "resolves that. Two of its indirections, not one"
                        % (text(n, src), inner))

                who = root_name(arg, src)
                if who and not who.endswith("_ptr") and who not in (
                        "g_image", "off", "si", "di", "bx"):
                    candidates.setdefault(who, set()).add(
                        "%s:%d %s" % (rel, arg.start_point[0] + 1, name))

        if n.type == "assignment_expression":
            lhs = n.child_by_field_name("left")
            rhs = n.child_by_field_name("right")
            if lhs is None or rhs is None:
                continue
            field = root_name(lhs, src)
            if field in known_ptr_fields:
                how = classify_store(rhs, field, known_ptr_fields, src)
                if how == "made":
                    pass                # img_off: a C pointer written down
                elif how == "copied":
                    add("pointer-from-data", n,
                        "%s = %s - a pointer taken from the game's own data "
                        "rather than made with img_off. Legitimate, and worth "
                        "seeing: it is where one of its pointers comes from"
                        % (text(lhs, src), text(rhs, src)))
                elif how == "advanced":
                    add("pointer-advanced", n,
                        "%s = %s - a cursor stepped. The arithmetic is on an "
                        "offset, which is what makes it 16-bit wrap-around "
                        "rather than C pointer arithmetic"
                        % (text(lhs, src), text(rhs, src)))
                else:
                    add("store-without-img_off", n,
                        "%s = %s - a plain number put where one of the game's "
                        "pointers goes" % (text(lhs, src), text(rhs, src)))

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


def inner_word_call(node, src):
    """`img_ptr(img_w(x))` - the x, if this is a pointer read through one.

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

    `made`     img_off of a C pointer - the port deciding an address
    `copied`   img_w, or another pointer field, or an entry of a table of
               them: the game's own data handing one over
    `advanced` the same field plus or minus something - a cursor stepping
    otherwise  a bare number, which is the thing worth catching
    """
    node = rhs
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
        for side in (node.child_by_field_name("left"),
                     node.child_by_field_name("right")):
            if side is not None and root_name(side, src) == field:
                return "advanced"
    who = root_name(node, src)
    if who and (who.endswith("_ptr") or who in known):
        return "copied"
    if node.type == "subscript_expression":
        return "copied"                 # a table of the game's pointers
    return None


def is_cast_of_off(node, src):
    """`(uint16_t)img_off(x)` is still an img_off."""
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
    a = ap.parse_args()

    missing = [f for f in a.files if not os.path.exists(f)]
    if missing:
        raise SystemExit("no such file: " + ", ".join(missing))

    known = ptr_fields(a.files)
    fields = struct_fields(a.files)
    findings, candidates = [], {}
    for f in a.files:
        check(f, known, findings, candidates)

    if not a.candidates:
        by_rule = {}
        for rule, path, line, msg in findings:
            by_rule.setdefault(rule, []).append((path, line, msg))
        order = ["store-without-img_off", "compound-offset",
                 "pointer-advanced", "pointer-from-data",
                 "pointer-to-pointer"]
        for rule in sorted(by_rule, key=lambda r: (order.index(r)
                                                   if r in order else 99, r)):
            hits = by_rule[rule]
            print("== %s (%d)" % (rule, len(hits)))
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

    print("\n%d flagged, %d fields already named _ptr"
          % (len(findings), len(known)))
    if a.strict and findings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
