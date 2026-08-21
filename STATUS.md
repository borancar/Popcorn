# Status

Where the port has got to, what is proven, and what is next.
Facts about the program live in [CLAUDE.md](CLAUDE.md).

Updated 2026-08-22.

## Done

### The executable is recovered and the recovery is proven

`popcorn.exe` is Microsoft EXEPACK. `unpack_popcorn.py` runs the stub under
Unicorn and takes what it leaves in memory, so the result is the original image
by construction rather than by reimplementing the format.

`validate.py` round-trips it: loading the emitted EXE at a segment and applying
its relocation table reproduces the stub's own output **byte for byte**, 133,296
bytes, with the entry point, stack and 35 relocations all as the stub would have
set them. The relocation count is agreed on by two independent readings — the
stub's own table, and a diff of two unpacks at different load segments.

### The game runs, and the CGA output is right

`emulation.py` reaches the title screen, the animated main menu, the player-name
boxes and the playfield frame, with the colours the game is meant to have.
Keyboard works on both paths: the BIOS INT 16h buffer in menus, and IRQ 1 with
scan codes at port 0x60 while the game's own INT 09h handler is installed.

### The code segment is mapped

`analyze.py` follows control flow from the entry point and the INT 09h handler
and reaches **122 routines, 14,798 of 23,696 bytes (62.4%)**. What it does not
reach is either data between routines or reached through a pointer, and
`--gaps` lists it.

## Open

### Reaching gameplay unattended

`--keys` fires on wall clock, and the emulator's speed varies with what the game
is drawing, so a script tuned on one run can miss on another. Typing a name and
pressing Enter twice reaches the second name box and then returns to the menu
rather than starting the level; the routine at `0x13b8` says an empty name with
at least one player entered should set carry and start the game, so either the
second Enter is not arriving or `0x3f08` is not what it is read to be. This
needs a state-triggered input mechanism rather than a timed one, which is worth
having regardless — snapshots, as in Ducks, are the eventual answer.

### Not yet modelled

- **PC-speaker sound.** Ports 0x42 and 0x61 are understood (`sound_tick` at
  `0x0097`) but nothing generates audio yet.
- **The disk reads at startup.** The program issues INT 13h AX=0002 and INT 25h
  AX=0002 before anything else. Both are refused and the game carries on, so
  they are not load-bearing, but what they are reading is not yet known — the
  first guess is where `POPSPEED.EXE` stores its value.
- **`popspeed.exe` and `popgen.exe`.** Neither is examined. `popspeed` patches
  the `mov cx,N` immediate in the delay at `0x164c`; where it puts N is the
  same question as the disk reads above.
- **The `.PPC` level format.** `poptab.ppc` is 8,630 bytes and is read whole by
  the loader at `0x08c8`. Not decoded yet.
- **The `.HSC` high-score format.** ASCII, 180 bytes, one line per entry.

### Not started

- `native.py` — routines hooked at their entry points and reimplemented in
  Python, each byte-checked against the code it replaces.
- The C reconstruction on SDL3.

## Next

1. State-triggered scripted input, so gameplay is reachable in an unattended
   run and screens can be compared without a person at the keyboard.
2. Decode the `.PPC` level format from the loader at `0x08c8`.
3. Name the rest of the code map: work outwards from the play path at `0x02d4`.
4. Start `native.py` with the drawing primitives, since those are what the C
   port needs to agree with first.

## Deferred

- **A full per-file inventory in `CLAUDE.md`.** It currently documents only the
  tools that exist. Once the game is fully reversed, every file in the
  repository gets an entry saying what it is and why it is there.
