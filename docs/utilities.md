# The utilities, and the cheat

The two programs shipped beside the game, and the thing typed at the menu.

## What POPSPEED actually does

It calculates nothing. It parses a decimal number and stores it verbatim as a
loop count, in 620 bytes:

- the PSP command tail is copied to `0000:0x24b`, `si` set past the length
  byte and the one space it assumes;
- **more than five digits is refused, and exactly five is compared against the
  string at `0x246` - which is `"65534"`, not the 30000 its own message
  advertises.** One to four digits are not range-checked at all, so the
  reachable range is 0-65534 rather than the documented 0-30000;
- the parse is `ax = 10^(digits-1)` by repeated `mul`, then digit by digit
  `bx += digit * place; place /= 10`. Sixteen-bit throughout - only `ax` of
  `mul`'s result is used - which is why the cap sits just under 0x10000;
- `INT 21h AX=2568h` sets **interrupt vector 0x68** to `ds:dx` with `ds` = 0,
  so the number lands in the *offset* word at `0000:01a0` and the segment is
  zero. No file, no environment variable: a spare interrupt slot used as a
  two-byte notepad, which is why the startup disk reads looked load-bearing
  and were not.

`read_speed_setting` at `0x5680` reads it back with `AH=35h`, subtracts one,
and patches the result into the `mov cx, N` of the busy-wait at `0x164c`. The
number *is* the loop count - hence lower is faster. 1 means "as fast as
possible" and puts a `ret` over the delay; 0 means POPSPEED was never run and
the default 0x6f applies, which is the readme's 110 after the decrement.

`inc bx` at `0x6d` is dead on both paths: `dx` already holds the value and
`bx` is never read again.

Shipped files. The working copy in `popcorn/` is not committed; the same
files are committed in `reconstruct/`, beside the port that reads them:
`popcorn.exe`, `popcorn.doc`, `popcorn.hsc` (high scores), `popspeed.exe`,
`popgen.exe`, `poptab.ppc`, `ltf.ppc`, and two batch files.

## The cheat

Typing **`LACRAL software`** and Enter **at the main menu** sets `[0x3f1b]`,
which is the no-lives-lost flag: `play_session` skips its `dec [0x13c9]` at
`1ac2:0363` and `ball_after_endgame` skips handing a life back at `1ac2:462c`.
The string is at `0x3f0b`, `cheat_match` at `0x5171` walks it a key at a time,
and every key pressed at the menu is fed to it. It is the authors' own company
name, and it is not the F10 "touche spéciale pour employés" the readme
mentions - that is a separate screen.

Typing it needs a shift, which the emulator's `KEYMAP` has no state for, so
the route syntax takes `^l` for shift+L: the same scan code with the
upper-case ASCII, which is what the BIOS would hand the game.
