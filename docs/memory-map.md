# The image, and what is where in it

Addresses into `popcorn.unpacked.exe`'s load image, which is the convention
every note and every reconstructed routine uses. `DS = 0` for the whole
program, so a data reference `[0x2d4f]` is image offset `0x2d4f`.

## It is EXEPACK-compressed

`popcorn.exe` is 103,848 bytes on disk and expands to 133,296. The MZ header
carries **zero** relocations and an entry point 16 bytes from the end of the
file — both belong to the unpacker stub. `unpack_popcorn.py` recovers it:

```sh
venv/bin/python unpack_popcorn.py     # -> popcorn.unpacked.exe
venv/bin/python validate.py           # round-trips it against the stub
```

EXEPACK header (16-byte variant, no `skip_len`): `real_cs:ip = 1ac2:0113`,
`real_ss:sp = 1aa2:0200`, `dest_len = 0x208b` paragraphs, `exepack_size =
0x178`. 35 relocations, agreed on by two independent readings — the stub's own
table, and a diff of two unpacks at different load segments.

**The unpack relies on the 8086 wrapping addresses at 1 MB.** The stub walks its
pointers downwards and renormalises with `or si,0xfff0`, which drives the
segment register below zero; on real hardware the address wraps, in Unicorn's
flat memory it escapes to 0x10eea1 and the stub prints "Packed file is corrupt".
Loading at segment 0x2000 instead of the 0x110 DOS would pick keeps the segment
non-negative and sidesteps it. The image is normalised back to segment 0 before
being written out.

## Layout of the unpacked image

Linear offsets into `popcorn.unpacked.exe`'s load image, which is the address
convention every note and every reconstructed routine here uses.

| range | what |
| --- | --- |
| `0x00000`-`0x1ac20` | data: sprites, fonts, level tables, strings, buffers |
| `0x1ac20`-`0x208b0` | **the code**, one segment `0x1ac2`, 23,696 bytes |

`DS = 0` for the whole program, so a data reference `[0x2d4f]` is image offset
`0x2d4f`, and a code address `1ac2:03e3` is image offset `0x1b003`.

### Data addresses identified so far

| offset | what |
| --- | --- |
| `0x13a0` | PSP command tail, copied there at startup |
| `0x1405` | saved `SP` for the return-to-menu longjmp |
| `0x1428` | level filename being built (`<tail>.PPC`) |
| `0x13e9` | which player-name box is being edited, ASCII `'1'`.. |
| `0x2d41` | saved BIOS INT 09h vector (offset, then segment at `0x2d43`) |
| `0x2d45` | current screen handler pointer |
| `0x2d47` | selected input handler: `0x16d2` = keyboard, `0x1654` = mouse |
| `0x2d49` | last make scan code seen by the INT 09h handler |
| `0x2d4a` | last direction: 0 = left, 1 = right |
| `0x2d4c` | action key held |
| `0x2d4d` | right key held |
| `0x2d4e` | left key held |
| `0x2d4f` | **left** key scan code (default `0x24`, K) |
| `0x2d50` | **right** key scan code (default `0x25`, L) |
| `0x2d51` | **action** key scan code (default `0x39`, Space) |
| `0x344f` | player-name table, `0x11b` bytes per player: the name at `+0`, the lives at `+0x0c`, the score as ASCII digits at `+0x10` |
| `0x3f08` | players entered so far |
| `0x3f09` | how many are still in - it reaching zero is what ends the game |
| `0x33b1` | the capsule odds: eleven cumulative weights out of 255, walked by `bonus_kind` |
| `0x2d40` | paddle repeat counter, counts down to the next allowed step |
| `0x2d4b` | paddle repeat divider; decremented while a key is held, so the paddle accelerates |
| `0x2e54` | **paddle x**, the left edge in pixels |
| `0x2d3e` | lowest paddle position (8) |
| `0x2d3f` | highest paddle position (172) |
| `0x2ea1` | **ball pool**: four entries of `0x1e` bytes |
| `0x3138` | head node of the entity list; its `+0x0c` link is `0x3144` |
| `0x3142` | previous-node cursor, for unlinking |
| `0x3144` | first entity link; `0xffff` terminates the chain |
| `0x3146` | entity node pool, stride `0x0e` |
| `0x313a` | "remove me" flag an entity handler sets |
| `0x33d2` | PRNG state, advanced by `0x5ec5` per call |
| `0x3164` | ten words the PRNG folds in |
| `0x9020` | **font**: 40 glyphs of 8x12, 24 bytes each |
| `0x2f10` | the level being played: an 8-byte header then 12x14 cells |
| `0x3044` | brick behaviour table, **thirty** words indexed by cell value |
| `0x3080` | what each hit animated brick became, indexed by the new cell value |
| `0x3134`, `0x3135` | the animation script's counter and its reload |
| `0x3136` | the animation script pointer, into the block at segment `0x14a1` |
| `0x13c9` | lives |
| `0x13ca` | offset of the current level in the table |
| `0x13cc` | level number, 0-0x31 |
| `0x13cd` | the score, as eight ASCII digits |
| `0x13d5` | the player's name, 12 characters |
| `0x1487` | **the frame delay**, reloaded from `0x1489` every frame |
| `0x1485`, `0x1486` | how often the ball is allowed to step, and the limit |
| `0x2d0d` | four (paddle sprite base, width) pairs: 27, 39, laser, catch |
| `0x2d2d` | capsule kind -> paddle kind, eleven bytes |
| `0x2d39` | the paddle kind in play |
| `0x2d3a` | the paddle's live **width**, which the morphs change |
| `0x3385` | capsule frame tables, by kind - the letters are drawn here |
| `0xac60` | the eight capsules a hatch can release: (sprite, flags) |
| `0x2e73` | clear when the last ball is lost |
| `0x4903` | the paddle sprites: four sets of four pre-shifted 7x44 images |
| `0xc46c` | **the level table**: fifty records of 176 bytes |
| `0x10250` | 32,000-byte backup of the CGA screen (`0xc46:0x3df0`) |

### Code addresses identified so far

Segment-relative (add `0x1ac20` for the image offset).

| offset | what |
| --- | --- |
| `0x0085` | `speaker_on` — PIT ch2 to mode 3, gate and data bits on |
| `0x0090` | `speaker_off` |
| `0x0097` | `sound_tick` — steps the current tune, writes PIT ch2 |
| `0x0106` | `flush_keys` — drain the BIOS INT 16h buffer |
| `0x0113` | **entry point**; startup, then the main menu loop at `0x0206` |
| `0x03b0` | `install_int09` — save the BIOS vector, install `0x03e3` |
| `0x03d1` | `restore_int09` |
| `0x03e3` | the INT 09h handler (see below) |
| `0x02d4` | F1: the play path |
| `0x10de` | the player-name boxes |
| `0x13b8` | one name field, via INT 21h AH=07h |
| `0x164c` | `delay` — `push cx; mov cx,N; loop $; pop cx`, N patched by POPSPEED |
| `0x1654` | mouse input handler |
| `0x16d2` | keyboard input handler |
| `0x5099` | `save_screen` — 0xb800 both halves to `0xc46:0x3df0` |
| `0x50bc` | `restore_screen` |
| `0x0c64` | `draw_char` — one 8x12 glyph to `ES:DI`, stepping the interlace |
| `0x1873` | **the play loop**: serve, entity walk, ball stepping, collision |
| `0x169f` | `input_mouse` tail: `paddle = clamp(mouse x / 2)`, buttons = action |
| `0x172f` | `input_keyboard` tail: one pixel per repeat tick |
| `0x27d7` | `ball_step` — the Bresenham stepper |
| `0x3257` | unlink an entity from the list |
| `0x40c0` | `random` — BIOS ticks, ten words at `0x3164`, and an LCG at `0x33d2`; returns `AH = value % DL` |
| `0x1c4f` | the level intro: the border, the lives, and a figure walking the paddle row |
| `0x1e50` | one 12x7 sprite, shifted to a pixel x and XORed in at the paddle row |
| `0x2281` | `blit_xor` - seven rows of eleven bytes, XORed |
| `0x22de` | `paddle_row_offsets` - seven CGA offsets from an x |
| `0x2f5` | `play_session` - a whole game, level by level |
| `0x5680` | `read_speed_setting` - POPSPEED's value, out of **interrupt vector 0x68** |
| `0x14b3` | `build_shifted_sprites` - generates three of every four sprite phases at startup |
| `0x5630` | the screen blit: waits on 0x3da bit 3, then `rep movsb` per row |
| `0x2ccd` | `brick_animated` - cells 16-21, the pieces of a running picture |
| `0x3abf` | the entity that keeps one of those pieces animating |
| `0x3bac` | `draw_anim_cell` - eight rows of four bytes, copied not XORed |

### The INT 09h handler, `0x03e3`

It does **not** chain to the BIOS, so the INT 16h buffer stops filling while it
is installed. The game therefore installs it for play and takes it out again for
the menus, and anything feeding keys in has to follow that. It:

- reads port 0x60, acknowledges via port 0x61 (set bit 7, restore)
- sets `ah` = 1 for a make, 0 for a break
- notes the direction at `0x2d4a` if the code matches the left or right key
- toggles the sound-enable flag `cs:[0x84]` on scan code `0xc3` (F9 break)
- stores the make code at `0x2d49`
- `repne scasb` over the three configured keys and stores the make/break flag
  into `0x2d4c + cx` — so left lands at `0x2d4e`, right at `0x2d4d`, action at
  `0x2d4c`
- EOI to port 0x20, `iret`
