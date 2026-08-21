#!/usr/bin/env python3
"""
Extract the game's data structures from the unpacked image.

Each one is here because a routine was read and said where it is and what
shape it has; the routine is named in the comment. Rendering it back out is
the check - a font that decodes to legible letters is a font, and a wrong
stride is obvious at a glance in a way it never is in a hex dump.

Usage:
    python dump_data.py font --out debug/font.png
    python dump_data.py font --ascii
"""
import argparse
import os

from tools_dis import load_image, UNPACKED

# ---------------------------------------------------------------- the font
#
# From draw_char at 1ac2:0c64. It maps a character to a glyph index:
#
#     ':'      -> 0x26        '-'      -> 0x0b
#     0xff     -> 0x27        ' '      -> 0
#     '0'-'9'  -> al - 0x2f   'A'-'Z'  -> al - 0x35
#
# then `shl ax,3` and `bx = ax*2 + ax`, i.e. index * 24, into a table at
# 0x9020. It copies 12 rows of one word each, stepping the destination by the
# CGA interlace and backing DI up two bytes each time to stay in one column.
# So a glyph is 12 rows of 2 bytes, and two bytes at two bits per pixel is
# eight pixels: an 8x12 cell. Note glyph 0 - what a space maps to - is not
# blank but a solid block of colour 2, which is how the game paints the red
# bars its headings sit on.
FONT = 0x9020
FONT_ROWS = 12
FONT_BYTES = 2
FONT_GLYPH = FONT_ROWS * FONT_BYTES
FONT_COUNT = 0x28
FONT_W = FONT_BYTES * 4          # two bits per pixel

CHARSET = {0x00: " ", 0x0b: "-", 0x26: ":", 0x27: "\x7f"}
for i, c in enumerate("0123456789"):
    CHARSET[i + 1] = c
for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
    CHARSET[i + 12] = c


def glyph_pixels(img, index):
    """One glyph as 12 rows of 8 two-bit pixels."""
    base = FONT + index * FONT_GLYPH
    rows = []
    for r in range(FONT_ROWS):
        row = []
        for b in range(FONT_BYTES):
            byte = img[base + r * FONT_BYTES + b]
            for k in range(4):
                row.append((byte >> (6 - 2 * k)) & 3)
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["font"])
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--out", default=None, help="write a PNG here")
    ap.add_argument("--ascii", action="store_true")
    a = ap.parse_args()

    img = load_image(a.exe)
    glyphs = [glyph_pixels(img, i) for i in range(FONT_COUNT)]

    if a.ascii:
        shades = " .:#"
        for i, g in enumerate(glyphs):
            name = CHARSET.get(i, f"#{i}")
            print(f"--- glyph {i:#04x} {name!r} ---")
            for row in g:
                print("  " + "".join(shades[p] for p in row))
        return

    out = a.out or os.path.join("debug", "font.png")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    import pygame
    pygame.init()
    cols = 8
    rows = (FONT_COUNT + cols - 1) // cols
    cell_w, cell_h = FONT_W + 2, FONT_ROWS + 2
    surf = pygame.Surface((cols * cell_w, rows * cell_h))
    pal = [(0, 0, 0), (85, 255, 255), (255, 85, 85), (255, 255, 255)]
    surf.fill((32, 32, 32))
    for i, g in enumerate(glyphs):
        ox, oy = (i % cols) * cell_w + 1, (i // cols) * cell_h + 1
        for y, row in enumerate(g):
            for x, p in enumerate(row):
                surf.set_at((ox + x, oy + y), pal[p])
    surf = pygame.transform.scale_by(surf, 4)
    pygame.image.save(surf, out)
    print(f"wrote {out}: {FONT_COUNT} glyphs of "
          f"{FONT_W}x{FONT_ROWS} from {FONT:#07x}")


if __name__ == "__main__":
    main()
