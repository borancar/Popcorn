# Popcorn — the game's own code, reconstructed

C reconstructed from the disassembly of **Popcorn** (Christophe Lacaze /
Frédérick Raynal, LACRAL software, 1988), a routine at a time, each carrying
the address in the original it was read from. It builds and plays:

```sh
make && ./popcorn
```

**You need your own copy of the game.** Nothing of Popcorn is distributed
here — no executable, no level sets, no artwork. Every sprite, font, level
table and string the game has lives in the first 0x1ac20 bytes of
`POPCORN.EXE`, and the port unpacks that file at startup and reads its data
from there. Put `popcorn.exe` beside the binary, along with any `.ppc` level
sets you want to play:

```sh
cp /wherever/popcorn.exe /wherever/*.ppc .
make && ./popcorn          # the fifty levels built into the executable
./popcorn POPTAB           # POPTAB.PPC instead, as POPGEN wrote it
```

`POPCORN_EXE` points at a copy kept somewhere else. Everything the game reads
and writes — the executable, the `.ppc` sets, and `popcorn.hsc` — is relative
to the current directory, as it was under DOS.

Needs SDL3 and a C99 compiler. `make` prints what to install if it cannot find
SDL3.

## What this is

The original is **hand-written 16-bit x86 assembly** — not compiled. There are
zero `push bp; mov bp,sp` sequences in its 23,696 bytes of code, no C runtime,
one code segment and one flat data area, with arguments passed in registers and
threaded across calls. So there are no compiler idioms to reverse and every
instruction is a decision someone made, which is why this is written as
structured C that reads as a game rather than as transliterated
register-shuffling. Where a routine cannot honestly be written that way, it is
written literally and says why.

Every integer is a `stdint` one. `int16_t` says "this truncation is the
`imul`'s"; `short` only says "some signed type".

## How it is known to be right

It is not asserted, it is measured. The port runs beside an emulator running
the original, in lockstep, and the two are compared **byte for byte** — the
whole 133,296-byte image and the 16K of video memory, every frame. Separately,
each routine is captured at its entry, the original is run to its return, the C
is run on the same capture, and the results are diffed.

That machinery lives in the repository this was split from, along with the
disassembly notes: <https://github.com/borancar/Popcorn/tree/develop>.

A second binary, `popcorn-dev`, carries the flags that exist to *check* the
port — the lockstep protocol, single-routine verification, screenshots, a bot
that plays the game by itself. `popcorn` takes what the original takes and
nothing else, because that command line is part of what the port is.

## Files

| | |
| --- | --- |
| `main.c` | `popcorn` — the game, and its one optional argument |
| `devmain.c` | `popcorn-dev` — the same game with the harness flags |
| `game.c` | the transcription: every routine, each with its `1ac2:xxxx` |
| `game.h` | types, the named image offsets, the backend interface |
| `exepack.c` | the EXEPACK decoder that recovers the data at startup |
| `sdl_io.c` | window, presentation, keyboard, mouse, retrace, delay |
| `autoplay.c` | the bot, for `popcorn-dev --autoplay` |
| `lockstep.c`, `verify.c` | the two halves of the checking, on this side |
| `stubs.c` | what is not transcribed. One safety net is left in it |

## Licence

The program is stated in its own readme to be public domain — *"Ce programme
fait parti du domaine public"* — but that is the authors' word about their own
distribution, not a grant anybody else can re-publish under, so nothing of the
game is redistributed here. This reconstruction is a separate work; ask before
assuming a licence for it.
