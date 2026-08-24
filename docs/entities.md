# Entities: the ball, the bricks, the capsules

Everything the play loop has in the air at once. None of it is in a table -
an entity is a node in a chain whose handler word says what it is - which is
why nothing that follows control flow can find any of it.

## The entity system

The play loop at `0x1873` walks a **linked list** and calls each node's handler:

```
1b4d  mov word [0x3142], 0x3138   ; the previous-node cursor, at the head
1b53  mov bx, [0x3144]            ; the first link
1b57  cmp bx, 0xffff / je done
1b5d  push bx / call word ptr [bx] / pop bx
1b61  cmp byte [0x313a], 0        ; did the handler ask to be removed?
1b68  mov [0x3142], bx / mov bx, [bx+0xc] / jmp 1b57
1b71  mov cx, [bx+0xc] / call 0x3257 / mov bx, cx / jmp 1b57
```

So a node is `+0x00` its per-frame handler and `+0x0c` the next link, the pool
is at `0x3146` with stride `0x0e`, and `0xffff` ends the chain. **Nothing that
follows control flow can reach a handler**, which is why static reachability
stopped at 62.4%; walking the list while the game played found eight of them
(`0x3273`, `0x3386`, `0x3561`, `0x3717`, `0x390d`, `0x39fa`, `0x3aee`,
`0x3b2a`) and took it to 76.9%.

The node layout, as far as `0x39fa` (a ball's handler) reads it:

| offset | what |
| --- | --- |
| `+0x00` | the per-frame handler — **and it is rewritten in place**, so a node is a small state machine |
| `+0x04`, `+0x05` | two bytes passed to the draw call as `cl` and `al`; position, most likely |
| `+0x06` | a pointer to a pointer to the sprite |
| `+0x08` | flags; the low nibble is the kind |
| `+0x0c` | the next link |

`0x39fa` is worth reading whole, because it independently confirms the ball
structure: on a bounce it sets the anchor from the live position with one
`mov ax,[di] / mov [di+0x18],ax`, zeroes the accumulators at `+0x1a`, flips
both direction flags, and picks a fresh slope with two `random(7) + 1` into
`+0x16` and `+0x17`. It also writes `cs:[0xf4] = 6` to start a sound, and
replaces its own handler with `0x3aee`. `[0x33d4]` carries the collision result
the step produced.

## The ball structure

Four entries of `0x1e` bytes at `0x2ea1`, stepped by `ball_step` at `0x27d7`.

| offset | what |
| --- | --- |
| `+0x00`, `+0x01` | **the live position**, in pixels |
| `+0x14`, `+0x15` | direction flags; non-zero negates that axis (`+0x15` set = moving up) |
| `+0x16`, `+0x17` | the slope, stored **(dy, dx)** |
| `+0x18`, `+0x19` | the anchor: where the current straight segment began |
| `+0x1a`, `+0x1b` | Bresenham accumulators, counting away from the anchor |
| `+0x1c` | state: 0 idle, 1-2 in play |

Two traps, each of which cost a debugging round:

- **`+0x18`/`+0x19` is not the position.** It is the anchor, and it does not
  move again until the next bounce. The live position is `+0x00`/`+0x01`, which
  matches the drawn sprite to the pixel.
- **The slope pair is stored (dy, dx).** Both branches of the stepper come out
  as `x_offset / y_offset = [+0x17] / [+0x16]`. Reading it the other way round
  makes a predicted landing point wrong by the square of the slope.

## A slip in the original, at `1ac2:267d`

`ball_bricks` decides how a ball leaves a brick from which of its four corners
were inside one. The slots are four words at `0x2e89`, `0x2e8d`, `0x2e91`,
`0x2e95`, and the direction table follows at `0x2e99`.

With two corners hit and the **first** one clear, the tree at `0x2676` asks
whether the second is set and then tests **`[0x2e99]`** - one slot too far,
which is `HIT_DIRS[0]`, and that word is a constant zero. So the both-axes
bounce is unreachable from that branch and the ball always takes the x-only
one at `0x26b3`. The three neighbouring tests all read real slots, so this
looks like an offset written four too high.

It has to be reproduced. Reading slot 3 there instead sends a ball that clips
two corners in the wrong direction, and it took eleven thousand frames of a
level 6 game for the two to disagree about anything.

## The parachute

**Brick 10** - cell value 10, the red block with the white grid - does not
just break. `0x2c59` puts the ball into **state 4** and allocates an entity
running `0x37e0` which carries it down the screen under a parachute, a pixel a
frame, from where the brick was to `y = 0xb8`. Then it lets go: the ball comes
back **upwards if the safety net is up**, and otherwise it is lost.

The paddle can catch it. `entity_ball_hold` runs the carrier through
`bonus_update` at `0x3df1`, the same collision a falling capsule uses, so the
paddle, another ball or the laser all release it early - `[0x33d4]` says
which. The catch window is the capsule one: `y <= 0xbe && y + 0x0f >= 0xb8`,
and the sprite spans `x..x+0x0f`.

The trap for anything reading the game's state is that **a held ball's own
`+0x00`/`+0x01` stop moving**. The position on screen is the carrier's `+4`
and `+5`; the ball array says the ball is still where the brick was. A bot
that only reads the ball array does not see the parachute at all.

Brick 9 (`0x2b9d`) is the same idea with state 3: the ball is taken away and
put back somewhere else.

## The capsules

Eight of the eleven kinds can come out of a hatch (the table at `0xac60`);
`bonus_release` at `0x39a1` picks one. A falling capsule is an **entity running
`0x3273`** with `+2` x, `+3` y and `+4` its kind - it is in no table, so the
only way to find one is to walk the entity list. Caught, it rewrites its own
handler to `0x3386` and the paddle morphs; the effect fires when the morph
finishes, out of the table at `0x33bc`.

The letters are French, and two of them are the opposite of what they look
like:

| kind | letter | what it does |
| --- | --- | --- |
| 0 | R | a hundred points, and it **cancels the net** and the stopped-monsters state. The trap of the set: it undoes an F |
| 1 | C | the paddle catches the ball |
| 2 | E | **the wider paddle** - 39 pixels against 27. The effect routine at `0x3231` is empty; the widening is the morph, through `0x2d2d` |
| 3 | L | the laser |
| 4 | T | more balls |
| 5 | F | *filet*, the safety net across the bottom |
| 6 | I | every ball reverses vertically |
| 7 | V | *vie*, an extra life |
| 8 | + | the level is over |
| 9 | S | **the ball moves less often** - `0x31e8` counts `[0x1486]` *down* to 2, so two frames in three becomes one in two |
| 10 | M | **the monsters stop** - no hatch opens while its timer runs. It does not slow the game down |

`dump_data.py` will not render these; the letters were read by printing only
the colour-3 pixels of frame 7 of each kind's table at `0x3385`, which is the
frame where the capsule is fully open.

**There are two sources, with different odds, and that matters.** A hatch
(`bonus_release` at `0x39a1`) picks `random(8)` from the table at `0xac60` -
**eight** of the eleven kinds, and V, I and + are not among them. A **brick 2**
that breaks outright instead drops one chosen by `bonus_kind`, a weighted pick
over the cumulative table at `0x33b1`, which can be any of the eleven:

| | R | C | E | L | T | F | I | V | + | S | M |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| /255 | 25 | 38 | 25 | 20 | 20 | 25 | 27 | **7** | **2** | 38 | 28 |

So `+` is **0.8%** of brick-2 capsules and V is 2.7%, and on a level whose
brick 2s are gone or unreachable neither can appear at all. That is why the
bot never sees a `+` on level 10 - the wall leaves only hatches - and why
`extra_life` at `0x318b` is the hardest routine in the game to make run.
