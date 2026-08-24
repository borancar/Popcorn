# Levels, and the files that hold them

The fifty levels the game ships with, the records they live in, and the
`.PPC` files POPGEN writes.

## The level format

Fifty levels of 176 bytes at image `0xc46c` - the block the program reaches as
segment `0xc46`. `play_session` copies one at a time to `0x2f10`, and the play
loop watches the first byte of that copy to know when the level is cleared.

| offset | what |
| --- | --- |
| `+0x00` | **brick count**: the non-zero cells **except values 3 and 9**. Exact on all 148 records there are - the fifty built in and the forty-nine in each shipped `.PPC` |
| `+0x01` | how many teleport cells this level has, 0 to 6 |
| `+0x02` | their cell indices, `[+0x01]` of them, six bytes of room |
| `+0x08` | the cells: **12 columns by 14 rows**, one byte each - 168 bytes, so a record is 8 + 168 = 176 |

A 3 hardens into a 4 that nothing breaks, and a 9 is the teleport rather than
something to clear, so neither counts towards finishing the level. The other
values do, the six animated pieces included. Only 1, 2, 3, 5, 6, 7, 8, 9, 10,
11 and 16-21 ever appear in a stored level; 4, 12 and 24-29 are values the game
writes at run time.

`brick_9` turns **every** cell in that list to 4 when the ball hits one of
them, so it cannot fall straight into another, and the entity at `0x36fb` puts
them all back to 9 afterwards. The list is there so neither has to scan the
field.

### The cell values

A cell is one byte, and the byte *is* the behaviour: `1ac2:3044` is thirty
words indexed by it, and hitting a cell calls the word it finds. Anything the
table has as zero cannot be hit at all.

| value | handler | what happens |
| ---: | --- | --- |
| 0 | - | empty |
| 1 | `1ac2:28cb` `brick_1` | 20 points and gone. Usually left to a crumbling entity; while fewer than three capsules are out, one hit in three removes it at once and leaves a score popup |
| 2 | `1ac2:2985` `brick_2` | the same, but that outright break drops a **capsule** - the only place the weighted eleven at `0x33b1` are drawn from |
| 3 | `1ac2:2a3f` `brick_3` | no score: **hardens into a 4**, and nothing breaks a 4 |
| 4 | `1ac2:3221` `brick_solid` | indestructible; the ball bounces and nothing else |
| 5 | `1ac2:2a73` `brick_5` | 20 points, becomes a 6 |
| 6 | `1ac2:2ab4` `brick_6` | 30 points, becomes a 7 |
| 7 | `1ac2:2af5` `brick_7` | 50 points, becomes an 8 |
| 8 | `1ac2:2b36` `brick_8` | 100 points and gone, leaving an entity at `0x366f` |
| 9 | `1ac2:2b9d` `brick_9` | 25 points. Takes the ball away and puts it back at a cell picked at random. **Every 9 in the level becomes a 4** while it is gone, so it cannot fall straight into another |
| 10 | `1ac2:2c59` `brick_10` | 50 points. The ball goes into state 4 and comes down under a parachute |
| 11 | `1ac2:2d68` `brick_11` | 72 points. The cell becomes **12**, which is not a brick - the drawing code has a case of its own for it |
| 12 | `1ac2:3221` `brick_solid` | the hole an 11 leaves. Indestructible |
| 16-21 | `1ac2:2ccd` `brick_animated` | 33 points. Six pieces of one moving picture; **adds eight** to the cell rather than clearing it |
| 24-29 | `1ac2:3221` `brick_solid` | what a hit animated piece became. Bounce only |
| 13-15, 22-23 | - | zero in the table, and no level uses them |

**5, 6, 7, 8 are one brick**, four hits deep and worth 200 altogether. Each hit
scores and steps the cell down the chain; only the last removes anything.

Only **3** and **9** are exempt from the brick count at `+0x00` - a 3 is on its
way to being unbreakable and a 9 is transport rather than something to clear.
Everything else in the table counts, the six animated pieces included, which is
why a level whose last cells are 3s can never be finished by breaking them.

### `.PPC` files

Exactly **8,630 bytes** (`0x21b6`), and the shape is the header the game
already has plus the table it already has:

| offset | what |
| --- | --- |
| `+0x0000` | six bytes: `4c 41 43 52 41 4c`, **`LACRAL`** |
| `+0x0006` | **forty-nine** records of 176 bytes, 8,624 in all |

`level_load_file` at `1ac2:08c8` opens the name at `0x1428`, reads all
`0x21b6` bytes to `0xc46:0x0006`, and checks the signature. The destination is
what makes the format make sense: the built-in table starts at `0xc46:0x000c`,
so the six signature bytes land in the twelve bytes *before* it and the records
land exactly on it. The file is the table with a label glued to its front.

**Forty-nine, not fifty.** `0x21b6 - 6` is 8,624, which is 49 x 176, and the
built-in table is 50 x 176. So a `.PPC` replaces levels 0 to 48 and the
fiftieth is whatever was built in - the last level cannot be replaced by a
level set.

**The signature check is weaker than it looks.** `1ac2:08f0` is `repne cmpsb`
over six bytes, comparing `0xc46:0x0000` - the game's own copy of `LACRAL`,
which the read at offset 6 does not touch - against the file's first six.
`repne` repeats while ZF is *clear*, so it stops at the first byte that
**matches**: one byte in six agreeing is enough to pass. `repe` (`f3` rather
than `f2`) would have meant all six. A file beginning with `L` is accepted
whatever follows it.

Both shipped sets, `POPTAB.PPC` and `LTF.PPC`, are exactly this and both carry
a real `LACRAL`.

### The animated bricks

Cell values 16 to 21 are not bricks but the six pieces of one picture, and nine
of the fifty levels use them - 7, 9, 20, 23, 30, 34, 39, 42 and 45, always all
six together. Hitting one runs `0x2ccd`, which **adds eight** to the cell
rather than clearing it, draws the piece from the level's animation script, and
leaves an entity running `0x3abf` behind; that entity redraws the piece every
time the script steps, so the picture goes on moving after it has been broken.
Hitting the marked cell again lands on table entries 24 to 29, which all point
back at the solid handler: it bounces and nothing more.

`0x3abf` is reachable only through an entity node - **nothing calls it** - so
anything following control flow will count its bytes as the tail of whatever
routine precedes it.

The geometry is not a guess. At twelve wide the first level reads as four bands
of two solid rows alternating between cell values 2 and 1, which is exactly
what the game draws; at any other width it is diagonal nonsense.

`POPGEN.EXE` writes `.PPC` files - see above for the format. `reconstruct/popcorn
--cmdline poptab` plays them, as does `emulation.py --cmdline poptab`.

## Level 10, and why a surviving bot is not a winning one

Level 10 is worth reading as a piece of design, because it is where an
autoplayer stops. Row 8 is **twelve cells of value 3**, and `brick_3` hardens a
3 into a 4 that nothing breaks - so the moment the ball touches that row the
top of the field is walled off for good, and 41 of the level's 77 bricks are
behind it.

The ball is served below the wall, so the top is unreachable by playing. Nor
does a `+` capsule save it: `bonus_release` picks one of **eight** kinds from
the table at `0xac60`, and `+` and V are not among them - a hatch never drops
either. What is left is the **9 at row 12, column 8**, reachable through the
one gap at row 11 column 8: brick 9 takes the ball away and puts it back
somewhere else, which is how a player gets above the wall.

The port is not wrong here. Every brick handler the level uses - 1, 2, 3, 4,
9, 10 - verifies identical on it. It is the bot that cannot aim at a
one-cell gap, and that is a different problem from transcription.
