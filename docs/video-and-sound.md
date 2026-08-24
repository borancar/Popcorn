# Video and sound

CGA mode 05h, the font drawn in it, and the PC speaker.

## Video

CGA **mode 05h**, set once with INT 10h AX=0005 and never changed. 320x200,
four colours, two bits per pixel, most significant pair leftmost.

- Memory at `0xb8000`, **interlaced**: even scan lines from offset 0, odd from
  offset `0x2000`, 80 bytes to a row either way. The game's own row-stepping
  idiom is visible all over the code:
  `cmp di,0x2000; jb +; sub di,0x1fb0; jmp ++; +: add di,0x2000`.
- Mode 05h sets the colour-burst-kill bit in the mode-control register, which on
  an RGB monitor selects the third, often-forgotten four-colour palette:
  **background / cyan / red / white**. That is what the game is drawn in.
- The game never writes 0x3d8 itself, so the BIOS defaults for mode 05h are what
  matter. F8 cycles the colour-select register, 0x3d9.
- It waits on port 0x3da bit 3 (vertical retrace) around its blits.

## The font

`draw_char` at `0x0c64` maps a character to a glyph index — `' '` to 0, `'-'`
to `0x0b`, `':'` to `0x26`, `0xff` to `0x27`, `'0'`-`'9'` to `al - 0x2f`,
`'A'`-`'Z'` to `al - 0x35` — multiplies by 24 and indexes a table at `0x9020`.
Each glyph is 12 rows of one word: **8x12 at two bits per pixel**. The
destination steps the CGA interlace and backs `DI` up two bytes each row to
stay in one column.

Glyph 0, what a space maps to, is **not blank**: it is a solid block of colour
2, which is how the game paints the red bars its headings sit on.
`dump_data.py font` renders the sheet, which is the check — a wrong stride is
obvious at a glance and invisible in a hex dump. This is the score-panel font;
the menu uses a second, larger one that has not been located yet.

## Sound

PC speaker: PIT channel 2 (port 0x42) with the gate at port 0x61 bits 0-1.
Both the port and `emulation.py` play it. The divisor arrives as **two** writes
to 0x42, low byte then high, and the low byte is always 1 - `sound_tick` does
`out 0x42,1` then `out 0x42,note`, so the divisor is `(note << 8) | 1` and the
note byte alone picks the pitch.
`sound_tick` at `0x0097` walks a table of (divisor, duration) word pairs; the
tune pointers are at `cs:0xf8` and the enable flag at `cs:0x84`. Not yet
modelled — the emulator is silent.
