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

## How fast the play loop runs

The retrace is 60 Hz, but the play loop is **not** paced on it. One iteration
of the loop is one simulation step for everything - the ball, the capsules,
the laser, the entity chain, the keyboard repeat - and the ball's own gate at
`1ac2:1b0d` can at most let it step once per iteration:

```
if (--[0x1485] != 0)  step the balls    else  [0x1485] = [0x1486]
```

so the gate **skips** one iteration in `[0x1486]`. A larger value there means
the ball moves *more* often, and `SPEED_TIMER` raises it every 0x4e20
iterations, which is the level speeding up as it runs. `play_setup` starts it
at 3 - two steps in three - or at 0xfa when POPSPEED has patched the delay out.

The loop paces itself on three busy-waits, at 17 cycles a taken `loop`:

| | |
| --- | --- |
| three passes of 0xb4, one fewer per point of `[0x33d6]` | 9180 cy |
| `FRAME_DELAY` at `0x1487`, 0x1f4 loops | 8500 cy |
| `game_delay` at `1ac2:164c`, POPSPEED's N, default 0x6f-1 = 110 | 1870 cy |

`FRAME_DELAY` is reloaded with 0x1f4 at the top of every frame, and
`draw_paddle_shifted` takes 0x1f3 of it **back** if the paddle actually moved.
The 500 loops *are* the author's estimate of what redrawing the paddle costs,
and the delay is there to make the moved and still branches take the same
time. A port that redraws the paddle for free must not simply keep the delay:
it will only ever run the 1-loop branch and come out about twice too fast.

**Measured, by `cycles.py`:** summing the iAPX 86/88 manual's cycle costs over
every instruction of a frame gives about **24,500 cycles, or 326 Hz** at the
8 MHz the readme names.

| | cycles | Hz |
| --- | --- | --- |
| level 10, the bot moving the paddle | 24541 | 326.0 |
| level 10, paddle still | 24537 | 326.0 |
| level 1 | 24419 | 327.6 |

Three-quarters of that is the `loop $` waits, which are exactly 17 cycles a
turn, so only the remaining quarter carries any modelling - which is why the
three figures agree to within two Hz. The manual's table excludes instruction
fetch, and a real 8086 empties its prefetch queue on every branch, so 326 Hz
is an **upper bound**; `cycles.py --stall` says what a given assumption costs.

At 326 Hz the ball moves 217 pixels a second at the start of a level and 326
at the ceiling, crossing the ~190-pixel field in around three-quarters of a
second.

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
