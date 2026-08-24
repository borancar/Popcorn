# What Popcorn is

Facts about the program: its addresses, its data formats, its structures. Read
from the disassembly and checked against a running emulator, so anything here
can be argued back to a byte in `popcorn.exe`.

[CLAUDE.md](../CLAUDE.md) is the other half - how to work on the port, and the
traps that change what you do. This is what the program is; that is what to do
about it.

| | |
| --- | --- |
| [memory-map.md](memory-map.md) | the load image, every data and code address identified so far, the INT 09h handler, and how the EXEPACK recovery works |
| [level-format.md](level-format.md) | the 176-byte level record, the `.PPC` file, the cell values, the animated bricks, and why level 10 stops a bot |
| [entities.md](entities.md) | the entity chain, the ball structure, the capsules, the parachute, and a slip in the original's brick collision |
| [video-and-sound.md](video-and-sound.md) | CGA mode 05h and its palette, the 8x12 font, the PC speaker |
| [utilities.md](utilities.md) | POPSPEED, and the cheat typed at the menu |
| [the-game.md](the-game.md) | what Popcorn is, its menu keys, and what kind of program the binary turned out to be |
| [original-readme.md](original-readme.md) | `popcorn.doc`, the authors' own readme, decoded from CP850 to UTF-8 |

Addresses are **image offsets** unless written `seg:off`, and `DS = 0` for the
whole program - so a data reference `[0x2d4f]` is image offset `0x2d4f`, and a
code address `1ac2:03e3` is image offset `0x1b003`.
