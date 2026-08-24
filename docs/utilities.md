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

## POPGEN, the level editor

`POPGEN.EXE`, 42,354 bytes, is what wrote the `.PPC` sets. The readme says only
"Un générateur de Tableaux pour le jeu", so everything below is from running it.

It is a **plain EXE** - not EXEPACK'd, unlike the game - so the emulator loads
it directly:

```sh
uv run emulation.py --exe popcorn/popgen.exe --scale 3 --window
```

It asks the machine for very little: INT 10h for the mode and the cursor
shape, INT 16h for keys, and INT 21h AH=19h/47h for the current drive and
directory. **No mouse** - INT 33h is never called - so it is keyboard only.

### The menu, in text mode 03h

Two columns and an indications panel, in French:

| FICHIERS | TABLEAUX |
| --- | --- |
| Disque | Editer |
| Répertoire | Voir |
| Lecture | Copie:T→XX |
| Sauvegarde | Copie:XX→T |
| Effacer | Permuter |
| Catalogue | (ESC) SORTIR |

The arrows move the highlight and step between the columns, Enter chooses.
INDICATIONS carries the drive, the directory and the loaded file name -
`< Aucun >` until a `Lecture` - and LACRAL's logo is drawn underneath in
box-drawing characters.

### The editor, in CGA mode 05h

`TABLEAUX / Editer` switches to **mode 05h, the same mode the game plays in**,
and draws the field with the two side walls and a dot at every empty cell. The
header is `- Tableau n° 01 +` and `(ESC:sortir)`; down the left is the brick
palette, F1 to F10 with a swatch each, and `ESP:effac` at the bottom.

- The arrows move a cursor over the grid, which is **12 columns by 14 rows** -
  the game's own layout. It **clamps** at the edges rather than wrapping:
  thirteen presses of right leave the cursor in column 11.
- An **F-key writes its value at the cursor**. Pressing the same one twice
  writes the same value again; it is not a toggle.
- **Space** erases.
- **ESC** returns to the menu, and to text mode 03h.

### What the palette keys actually write

Measured, not read off the swatches: POPGEN keeps the level's cells at
**`0x2cef`**, twelve by fourteen, so pressing a key and diffing its memory
against a run that pressed nothing says exactly what that key does.

```sh
uv run drive.py --exe popcorn/popgen.exe \
    --keys right,return,f1,right,f2,right,f4 --dump /tmp/pg.bin
```

| key | cell | and so, in the game |
| --- | ---: | --- |
| F1 | 1 | 20 points and gone |
| F2 | 2 | the same, but drops a **capsule** |
| F3 | 3 | hardens into a 4, which nothing breaks |
| F4 | **10** | the ball comes down under a **parachute** |
| F5 | 5 | \ |
| F6 | 6 | the four-hit brick, 200 points across |
| F7 | 7 | / |
| F8 | 8 | the last hit of it |
| F9 | 9 | the **teleport** - but see below |
| F10 | 16-21 | the **animated brick**, laid down as one 2-wide by 3-tall block |
| space | 0 | erase |

Two things worth having: **F4 is 10, not 4** - the keys are a palette in the
authors' order, not the cell numbering - and **F10 places six cells at once**,
which is what `docs/level-format.md` means by "six pieces of one moving
picture".

The palette has no key for **4**, **11**, **12** or **24-29**, and it does not
need one: 4, 12 and 24-29 are values the game writes at run time. That leaves
11, and the shipped sets settle it - the cell values in `poptab.ppc` and
`ltf.ppc` are **exactly** 1, 2, 3, 5, 6, 7, 8, 9, 10 and 16-21, the palette and
nothing else. No `.PPC` here contains an 11.

**F9 is not pinned down.** It selects the teleport - it is the only key that
changes the brush word at `0x2a8b` without writing a cell - but it does not
place on every press. Eight presses across row 0 placed nothing at all, while
stepping down column 0 with `f9,down,f9,down,...` left 9s at rows 1, 3 and 5.
The rule was not identified. The level record caps teleports at six
(`+0x01` is 0 to 6), and the shipped sets never hold more than that, so a
limit of some kind is likely to be involved.

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
