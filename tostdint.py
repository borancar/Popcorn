"""Rewrite the C port's integer types to stdint, in code only.

Comments and string literals are left exactly as they are: "the compare is
unsigned" is prose, not a declaration, and a blind textual replace turns it
into nonsense. So this walks the file tracking whether it is inside a block
comment, a line comment, a string or a character constant, and only rewrites
the runs of code in between.

Widths are unchanged on this ABI, so the object code is the same and the
byte-for-byte results still hold.
"""
import re, sys

SUBS = [
    (r"\bunsigned\s+char\b",  "uint8_t"),
    (r"\bunsigned\s+short\b", "uint16_t"),
    (r"\bunsigned\s+long\b",  "uint64_t"),
    (r"\bunsigned\s+int\b",   "uint32_t"),
    (r"\bsigned\s+char\b",    "int8_t"),
    (r"\bunsigned\b",         "uint32_t"),
    (r"\bshort\b",            "int16_t"),
    (r"\blong\b",             "int64_t"),
    (r"\bint\b",              "int32_t"),
]

def split_code(src):
    """Yield (is_code, text) runs."""
    i, n, out, start = 0, len(src), [], 0
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            out.append((True, src[start:i]))
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append((False, src[i:j])); i = start = j
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            out.append((True, src[start:i]))
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append((False, src[i:j])); i = start = j
        elif c in '"\'':
            out.append((True, src[start:i]))
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append((False, src[i:j])); i = start = j
        else:
            i += 1
    out.append((True, src[start:]))
    return out

def convert(src):
    parts = []
    for is_code, text in split_code(src):
        if is_code:
            for pat, rep in SUBS:
                text = re.sub(pat, rep, text)
        parts.append(text)
    return "".join(parts)

for path in sys.argv[1:]:
    src = open(path).read()
    out = convert(src)
    if "#include <stdint.h>" not in out:
        m = re.search(r"#include <[^>]+>\n", out)
        if m:
            out = out[:m.start()] + "#include <stdint.h>\n" + out[m.start():]
    open(path, "w").write(out)
    print(f"  {path}: {len(re.findall(r'\\b(uint|int)[0-9]+_t\\b', out))} stdint types")
