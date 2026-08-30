/*
 * Popcorn - types, globals and the interface the game needs from below it.
 *
 * Addresses in comments are offsets into the unpacked load image, which is the
 * convention the whole repository uses: `0x2e54` is a data byte, `1ac2:16d2`
 * (image 0x1c2f2) is code.  See ../CLAUDE.md.
 */
#ifndef POPCORN_GAME_H
#define POPCORN_GAME_H

#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

/* ------------------------------------------------------------ the image ---
 *
 * The program is one flat address space: data from 0 to 0x1ac20, then a single
 * code segment.  It runs with DS = 0 throughout, so every data reference in the
 * disassembly is an offset into this array, and the port keeps that literal
 * correspondence rather than inventing a layout - it is what makes a
 * transcribed routine checkable against the binary.
 */
#define IMAGE_LEN   0x208b0
#define CODE_BASE   0x1ac20

extern uint8_t *g_image;                /* the whole unpacked load image */

/* One ball, the 0x1e bytes at gv.balls[i].
 *
 * Every byte is accounted for - the members below tile 0x00..0x1d with no
 * gaps - which is the corroboration that the layout is right and not merely
 * consistent. ENSURE_BALL_AT checks each offset and the size at compile time.
 *
 * The two four-word sprite arrays were reached a byte at a time, assembling
 * and splitting words by hand (`b[B_SPRITE + r*2] | b[B_SPRITE + r*2+1] << 8`).
 * As uint16_t they are just indexed, on the same little-endian assumption the
 * rest of the struct makes.
 */
typedef struct __attribute__((packed)) {
    uint8_t  x;                 /* 0x00 the live position */
    uint8_t  y;                 /* 0x01 */
    uint8_t  prev_x;            /* 0x02 where it was, so the XOR can undo it */
    uint8_t  prev_y;            /* 0x03 */
    uint16_t sprite[4];         /* 0x04 what is on screen now */
    uint16_t prev_spr[4];       /* 0x0c what to erase */
    uint8_t  dir_x;             /* 0x14 non-zero negates that axis */
    uint8_t  dir_y;             /* 0x15 */
    uint8_t  dy;                /* 0x16 the slope is stored (dy, dx) */
    uint8_t  dx;                /* 0x17 */
    uint8_t  anchor_x;          /* 0x18 where this straight segment began */
    uint8_t  anchor_y;          /* 0x19 */
    uint8_t  acc_x;             /* 0x1a Bresenham, counting from the anchor */
    uint8_t  acc_y;             /* 0x1b */
    uint8_t  state;             /* 0x1c 0 idle, 1-2 in play */
    uint8_t  bounces;           /* 0x1d */
} ball_t;

#define ENSURE_BALL_AT(field, off) \
    typedef char ensure_ball_at_##field[offsetof(ball_t, field) == (off) ? 1 : -1]
ENSURE_BALL_AT(x, 0x00);        ENSURE_BALL_AT(y, 0x01);
ENSURE_BALL_AT(prev_x, 0x02);   ENSURE_BALL_AT(prev_y, 0x03);
ENSURE_BALL_AT(sprite, 0x04);   ENSURE_BALL_AT(prev_spr, 0x0c);
ENSURE_BALL_AT(dir_x, 0x14);    ENSURE_BALL_AT(dir_y, 0x15);
ENSURE_BALL_AT(dy, 0x16);       ENSURE_BALL_AT(dx, 0x17);
ENSURE_BALL_AT(anchor_x, 0x18); ENSURE_BALL_AT(anchor_y, 0x19);
ENSURE_BALL_AT(acc_x, 0x1a);    ENSURE_BALL_AT(acc_y, 0x1b);
ENSURE_BALL_AT(state, 0x1c);    ENSURE_BALL_AT(bounces, 0x1d);
/* Sizes, the same way: a record's length is as much a fact from the
 * disassembly as any field's offset, and asserting it catches what the field
 * offsets alone cannot - a table declared with one entry too many. */
#define ENSURE_SIZE(type, n) \
    typedef char ensure_size_##type[sizeof(type) == (n) ? 1 : -1]

ENSURE_SIZE(ball_t, 0x1e);

/* Struct-land to offset-land, for the routines that still take an image
 * offset because that is what the original passed in a register. */
/* The level being played: the 176-byte record play_session copies out of the
 * table at 0xc46:0x000c, and the same shape every record in a .PPC file has.
 * See docs/level-format.md.
 */
typedef struct __attribute__((packed)) {
    uint8_t bricks;         /* 0x00 what is left to break - every non-zero
                             * cell except 3 and 9. The play loop watches it
                             * for zero to know the level is won */
    uint8_t teleports;      /* 0x01 how many teleport cells, 0 to 6 */
    uint8_t teleport[6];    /* 0x02 their indices into cells[] */
    uint8_t cells[168];     /* 0x08 twelve columns by fourteen rows, row
                             * major - and indexed **flat**, because the
                             * original multiplies a row by twelve as
                             * `row + (row >> 1)` with the row already scaled
                             * by eight, which no [row][col] spelling keeps */
} level_t;

ENSURE_SIZE(level_t, 0xb0);
#define ENSURE_LEVEL_AT(field, off) \
    typedef char ensure_level_at_##field[offsetof(level_t, field) == (off) ? 1 : -1]
ENSURE_LEVEL_AT(cells, 0x08);

/* One player's record, the 0x11b bytes at gv.players[i].
 *
 * It carries a whole game, not just a score: with more than one player the
 * game switches between them, and each has to come back to the level exactly
 * as they left it - which bricks are still standing and which capsules were
 * in flight. That is why level is a private copy and the entities are here.
 */
typedef struct __attribute__((packed)) {
    uint8_t  name[12];      /* 0x00 */
    uint8_t  lives;         /* 0x0c */
    uint16_t level_src;     /* 0x0d offset of their level in the 0xc46 table */
    uint8_t  level_number;  /* 0x0f */
    uint8_t  score[6];      /* 0x10 ASCII digits, as gv.score_text is */
    level_t  level;         /* 0x16 their copy of it, cells and all */
    uint16_t state[6];      /* 0xc6 twelve bytes from 0x30b0. Initialised to
                             * six 0xffff, which is the "six terminators" */
    uint8_t  ent_count;     /* 0xd2 */
    uint8_t  ents[6][12];   /* 0xd3 six entities, twelve bytes each */
} player_t;

ENSURE_SIZE(player_t, 0x11b);
#define ENSURE_PLAYER_AT(field, off) \
    typedef char ensure_player_at_##field[offsetof(player_t, field) == (off) ? 1 : -1]
ENSURE_PLAYER_AT(name, 0x00);   ENSURE_PLAYER_AT(lives, 0x0c);
ENSURE_PLAYER_AT(level_src, 0x0d); ENSURE_PLAYER_AT(level_number, 0x0f);
ENSURE_PLAYER_AT(score, 0x10);  ENSURE_PLAYER_AT(level, 0x16);
ENSURE_PLAYER_AT(state, 0xc6);  ENSURE_PLAYER_AT(ent_count, 0xd2);
ENSURE_PLAYER_AT(ents, 0xd3);

/* One entity, the 0x0e bytes of a node in the chain at gv.entity_head.
 *
 * Only the head and the tail are typed, and that is not laziness. The handler
 * at +0x00 and the link at +0x0c are read as words by everything and as bytes
 * by nothing - 13 and 19 accesses, no exceptions. The ten bytes between them
 * are the original's **variant**: whether +0x02 is a word or two bytes depends
 * on which handler owns the node, and five of the six slots are read both ways
 * across the fifteen handlers. Naming them wants each handler read first, so
 * until then they are payload and reached as bytes, which claims nothing.
 */
/* The moving-sprite part of a node, at 0x04..0x09.
 *
 * Not a guess and not a convenience: bonus_update and bonus_hits_ball step
 * *both* a falling capsule and a parachute carrier, and they can because
 * these six bytes are in the same places with the same meanings in both arms.
 * Giving them a name is what lets those two routines take it and nothing else.
 */
typedef struct __attribute__((packed)) {
    uint8_t  x, y;      /* 0x04 0x05 where it is drawn */
    uint16_t frame;     /* 0x06 a cursor *into* a list of frame pointers, so
                         * one dereference gets the frame */
    uint8_t  timer;     /* 0x08 two counters in one byte: the low nibble paces
                         * the movement, the high one the frame */
    uint8_t  period;    /* 0x09 what the low nibble reloads to */
} ent_sprite_t;

/* Every arm of the variant must fill the payload exactly - a short one would
 * silently move `next`. */
#define ENSURE_ENTITY_ARM(arm, n) \
    typedef char ensure_entity_arm_##arm[sizeof(ent_##arm##_t) == (n) ? 1 : -1]

typedef struct __attribute__((packed)) {
            union {         /* 0x02 the handler's own, and **not one width**:
                             * repeat's counter is a byte, the others a word.
                             * 1ac2:3679 is `dec byte ptr [bx+2]`, and 0x03
                             * holds whatever the recycled slot left there -
                             * see brick_entity - so reading the pair as a
                             * word never reaches zero and the entity is never
                             * removed. crumble, plain and sparkle do not use
                             * it at all. */
                uint16_t cell;  /* soften: the cell to harden */
                uint16_t ball;  /* ball_arrive: the ball to put down */
                uint8_t  count; /* repeat: how many times round again */
            } arg;
            ent_sprite_t sprite; /* 0x04..0x09 */
            uint8_t  _r[2];
} ent_anim_t;

typedef struct __attribute__((packed)) {
            uint8_t  x, y;  /* 0x02 0x03 y is incremented as it falls */
            uint8_t  kind;  /* 0x04 which frame table it draws from */
            uint8_t  tick;  /* 0x05 masked & 7: only every eighth call moves */
            uint8_t  frame; /* 0x06 the frame being drawn */
            uint8_t  cycle; /* 0x07 the next one, copied into frame each step;
                             * kind 2 cycles it 0..0x0f */
            uint8_t  _r[4];
} ent_fall_t;

typedef struct __attribute__((packed)) {
            uint8_t  mode;  /* 0x02 how it moves: 0 right, 1 down, 2 left,
                             * 3 up, 4 follow the script at 0x0a */
            uint8_t  steps; /* 0x03 how long to keep going that way, from
                             * random(0x3c) + 9 - and **reused** as the x the
                             * script started from once mode becomes 4 */
            ent_sprite_t sprite; /* 0x04..0x09, the same six bytes */
            uint16_t script;/* 0x0a cursor into the movement script */
} ent_bonus_t;

typedef struct __attribute__((packed)) {
            uint16_t cell;  /* 0x02 the cell it opens in */
            uint8_t  x, y;  /* 0x04 0x05 */
            uint16_t wait;  /* 0x06 set to 0x12c and counted down */
            uint16_t phase; /* 0x08 counted down; every 0x23rd draws */
            uint16_t script;/* 0x0a cursor into a list, ending at 0xffff */
} ent_hatch_t;

typedef struct __attribute__((packed)) {
            uint8_t  _p[2];
            uint16_t left;  /* 0x04 at zero, cells_restore puts the field back */
            uint8_t  _r[6];
} ent_cells_t;

typedef struct __attribute__((packed)) {
            uint8_t  x, y;  /* 0x02 0x03 */
            uint8_t  piece; /* 0x04 which of the six */
            uint8_t  _r[7];
} ent_brick_t;

typedef struct __attribute__((packed)) {
            uint8_t  pending;/* 0x02 a second capsule arrived mid-morph */
            uint8_t  step;   /* 0x03 frame of the morph, counted down from 6 */
            uint16_t sprites;/* 0x04 the sprite list being walked */
            uint8_t  from;   /* 0x06 the kind it started as */
            uint8_t  to;     /* 0x07 the kind it is becoming */
            uint8_t  _p[2];
            uint8_t  bonus;  /* 0x0a the capsule kind to apply when it lands */
            uint8_t  _r;
} ent_morph_t;

typedef struct __attribute__((packed)) {
    uint16_t handler;       /* 0x00 the routine entity_call dispatches to */
    union {                 /* 0x02 the variant, chosen by handler */

        /* crumble, plain, sparkle, soften, repeat, ball_arrive, bonus - a
         * sprite animation stepped frame by frame by entity_anim. */
        ent_anim_t anim;

        /* capsule and popup - a sprite falling down the screen. Note x and y
         * are at 0x02 here, not 0x04: the two families disagree, which is why
         * this is a union and not a struct. */
        ent_fall_t fall;

        /* entity_bonus - a capsule falling under one of several movement
         * scripts. Shares x, y, frame, timer and period with `anim`, because
         * it is stepped the same way, but 0x02 and 0x03 are two bytes here
         * rather than anim's word, and it has a script cursor at 0x0a. */
        ent_bonus_t bonus;

        /* the hatch a capsule comes out of */
        ent_hatch_t hatch;

        /* entity_cells_timer - nothing but a countdown */
        ent_cells_t cells;

        /* entity_anim_brick - one of the six pieces of the moving picture */
        ent_brick_t brick;

        /* entity_paddle_fx - the grow or shrink between two paddle kinds */
        ent_morph_t morph;

        uint8_t raw[10];    /* for the handlers still reached by offset */
    } p;
    uint16_t next;          /* 0x0c 0xffff ends the chain */
} entity_t;

ENSURE_ENTITY_ARM(anim, 10);   ENSURE_ENTITY_ARM(fall, 10);
ENSURE_ENTITY_ARM(hatch, 10);  ENSURE_ENTITY_ARM(cells, 10);
ENSURE_ENTITY_ARM(brick, 10);  ENSURE_ENTITY_ARM(morph, 10);
ENSURE_ENTITY_ARM(bonus, 10);

ENSURE_SIZE(entity_t, 0x0e);
#define ENSURE_ENTITY_AT(field, off) \
    typedef char ensure_entity_at_##field[offsetof(entity_t, field) == (off) ? 1 : -1]
ENSURE_ENTITY_AT(handler, 0x00);
ENSURE_ENTITY_AT(p, 0x02);
ENSURE_ENTITY_AT(next, 0x0c);

/* A node by its image offset. The chain's links are the game's own 16-bit
 * offsets, stored in the image and ended by 0xffff, so a walk still carries
 * one - this types what it finds there. */
static inline entity_t *entity_at(uint32_t off)
{
    return (entity_t *)(g_image + off);
}

/* A ball by its image offset, for the routines that still carry one because
 * the original passed it in a register. */
static inline ball_t *ball_at(uint32_t off)
{
    return (ball_t *)(g_image + off);
}

static inline uint32_t img_off(const void *p)
{
    return (uint32_t)((const uint8_t *)p - g_image);
}

/* ------------------------------------------------------------------------
 * The load image as a **structure**, laid over the same bytes as g_image.
 *
 * The alternative - a `#define` per address and `g_image[FOO]` / `img_w(FOO)`
 * at each use - has two problems the compiler cannot see. The width of a
 * field is chosen at every call site rather than declared once, so a byte read
 * as a word is a bug nothing catches; and a wrong address is simply a wrong
 * address. Here the offset of every field is checked at **compile time**
 * against what the disassembly says, so a mistake fails the build instead of
 * reading the wrong byte at run time. Get one padding length wrong and every
 * field after it shifts, and the build says which.
 *
 * The struct is packed and grows a field at a time: name an offset, split the
 * padding around it, add its ENSURE_IMG_AT. It does not have to cover the image, and
 * anything not yet named is still reached through g_image.
 *
 * **The packing and the padding are load-bearing, not tidiness.** The struct
 * has to land on the game's own addresses, and those are what they are:
 * frame_delay is at an odd offset, and so are most of the words. Drop the
 * `packed` attribute and let the compiler align things naturally and the
 * fields move - which is not a subtle failure, because the ENSURE_ macros
 * refuse to compile it. There is no unpacked layout that is still the image,
 * so there is no build-time choice to be made here.
 *
 * **Endianness.** The fields are the image's own little-endian words, so this
 * assumes a little-endian host, where `img_w`'s explicit `lo | hi << 8` did
 * not. That is a deliberate trade: the program being ported is a DOS binary,
 * every machine it ran on was little-endian, and so is every machine this is
 * likely to be built on. It would need byte-swapping accessors on a
 * big-endian host.
 */
typedef struct __attribute__((packed)) {
    uint8_t  _pad_00[5056];
    uint16_t eog_screen_at;             /* 0x13c0 end-of-game screen cursor */
    uint16_t eog_build_at;              /* 0x13c2 */
    uint8_t  banner_state;              /* 0x13c4 the menu's scrolling text */
    uint16_t banner_ptr;                /* 0x13c5 where it has got to, as an image offset */
    uint8_t  _pad_01[2];
    uint8_t  lives;                     /* 0x13c9 */
    uint16_t level_src;                 /* 0x13ca offset of the current level within the 0xc46 block */
    uint8_t  level_number;              /* 0x13cc */
    uint8_t  score_text[6];             /* 0x13cd the score, six ASCII digits - the game keeps no binary copy */
    uint16_t extra_at;                  /* 0x13d3 the next extra life, as the two ASCII digits the score has to reach - and stored **byte-swapped** against the score, which is why the comparison at 1ac2:2435 swaps before it compares. Reached as SCORE_TEXT + 6 and so looked like two more score digits; it is not */
    uint8_t  _pad_02[20];
    uint8_t  name_index;                /* 0x13e9 how far the player has typed their name */
    uint8_t  _pad_03[38];
    uint16_t level_num_text;            /* 0x1410 the level number as two ASCII digits, tens in the low byte */
    uint8_t  _pad_04[1];
    uint16_t particle_count;            /* 0x1413 the menu's fountain */
    uint8_t  _pad_05[83];
    uint16_t walker_anim;               /* 0x1468 a pointer into the walking figure's frame list, stepped by two */
    uint8_t  _pad_06[27];
    uint8_t  speed_step;                /* 0x1485 the ball's move-this-frame counter, reloaded from speed_limit */
    uint8_t  speed_limit;               /* 0x1486 its reload value: the ball steps on (limit-1) frames in limit */
    uint16_t frame_delay;               /* 0x1487 empty loops left this frame */
    uint16_t frame_delay_set;           /* 0x1489 what it is reloaded with */
    uint16_t speed_timer;               /* 0x148b frames until speed_limit rises, so a level speeds up */
    uint8_t  _pad_07[6315];
    uint8_t  paddle_step;               /* 0x2d38 how much the width changes per morph frame */
    uint8_t  paddle_kind;               /* 0x2d39 which of the four sprite sets is current */
    uint8_t  paddle_width;              /* 0x2d3a in pixels */
    uint8_t  paddle_morphing;           /* 0x2d3b a grow or shrink is running. Was two names for one byte: PADDLE_SUPPRESS, because the play loop stops drawing the paddle itself, and PADDLE_FORCE_DRAW, because draw_paddle_shifted redraws even when x has not moved */
    uint16_t morph_owner;               /* 0x2d3c the entity running it, so a second capsule does not fight the first */
    uint8_t  paddle_min;                /* 0x2d3e 8. Was also PADDLE_LOW */
    uint8_t  paddle_max;                /* 0x2d3f 172, and it moves as the paddle grows. Was also PADDLE_HIGH */
    uint8_t  repeat_count;              /* 0x2d40 frames until the held key moves the paddle again */
    uint8_t  _pad_08[4];
    uint16_t input_active;              /* 0x2d45 the input routine in use: 0x1654 mouse, 0x16d2 keyboard, 0x1785 demo */
    uint16_t input_selected;            /* 0x2d47 what the menu has chosen, copied to input_active at F1 */
    uint8_t  last_make;                 /* 0x2d49 the last make code the INT 09h handler saw; 1 is Esc, which pauses */
    uint8_t  last_dir;                  /* 0x2d4a which of left/right was pressed most recently, for when both are held */
    uint8_t  repeat_div;                /* 0x2d4b the reload for repeat_count; falls to 1, so a held key accelerates */
    uint8_t  key_action;                /* 0x2d4c held flags. These three are in the **reverse** order of the scan codes below, which is what int09_handler's `2 - i` was for */
    uint8_t  key_right;                 /* 0x2d4d */
    uint8_t  key_left;                  /* 0x2d4e */
    uint8_t  key_scan_l;                /* 0x2d4f the configured scan codes. Defaults 0x24, 0x25, 0x39 - **J**, **K** and space, read from the image rather than assumed */
    uint8_t  key_scan_r;                /* 0x2d50 */
    uint8_t  key_scan_a;                /* 0x2d51 */
    uint8_t  _pad_09[258];
    uint8_t  paddle_x;                  /* 0x2e54 left edge, pixels */
    uint8_t  paddle_prev_x;             /* 0x2e55 where it was last frame, so the old one can be erased */
    uint8_t  hold_offset;               /* 0x2e56 a caught ball's x relative to the paddle */
    uint8_t  _pad_10[28];
    uint8_t  ball_alive;                /* 0x2e73 clear when the last ball is lost */
    uint8_t  hit_count;                 /* 0x2e74 */
    uint8_t  caught;                    /* 0x2e75 the C capsule: the ball sticks to the paddle */
    uint16_t hold_timer;                /* 0x2e76 how much holding is left before it is released anyway */
    uint8_t  game_over;                 /* 0x2e78 */
    uint8_t  extra_on;                  /* 0x2e79 the extra-ball hatch is open */
    uint16_t serve_timeout;             /* 0x2e7a */
    uint16_t extra_timer;               /* 0x2e7c */
    uint8_t  laser_on;                  /* 0x2e7e the laser paddle. bonus_laser sets this and laser_y */
    uint8_t  laser_y;                   /* 0x2e7f the shot in flight; starts at 0xb3, the paddle's row */
    uint8_t  laser_x;                   /* 0x2e80 */
    uint8_t  net_on;                    /* 0x2e81 the safety net across the floor. **These four were named SHOT_ON, SHOT_LIFE, SHOT_TIMER and SHOT_POS, as though they were the laser's** - but 1ac2:3119, bonus_net, writes 0x2e81, 0x2e82 and 0x2e84, and the play loop's countdown over them ends in the same flash_bar(0x1554) the net's arrival plays. The laser is 0x2e7e-0x2e80 and nothing else */
    uint16_t net_life;                  /* 0x2e82 frames the net lasts; bonus_net sets 0x1388, five thousand */
    uint8_t  net_timer;                 /* 0x2e84 its redraw counter, reloaded with 0xc8 */
    uint16_t net_pos;                   /* 0x2e85 where it is drawn */
    uint16_t extra_pos;                 /* 0x2e87 */
    uint8_t  _pad_11[16];
    uint16_t hit_dirs[4];               /* 0x2e99 the four directions a brick hit can send the ball, indexed by which slot matched */
    ball_t   balls[3];                  /* 0x2ea1 the ball pool. **Three**, not four: 0x2ea1 + 3*0x1e ends exactly where backdrop_phase begins, and every loop over it is i < 3. BALL_COUNT said 4 and was never used */
    uint8_t  backdrop_phase;            /* 0x2efb the level intro's reveal, counted by kernel zero's timer */
    uint8_t  _pad_12[16];
    uint8_t  sweep_y[4];                /* 0x2f0c the four popcorn kernels sweeping the field during the level intro; kernel zero paces the reveal */
    level_t  level;                     /* 0x2f10 the level being played, copied out of the table at 0xc46:0x000c */
    uint8_t  _pad_13[372];
    uint8_t  anim_count;                /* 0x3134 the animated bricks */
    uint8_t  anim_rate;                 /* 0x3135 */
    uint16_t anim_ptr;                  /* 0x3136 */
    /* 0x3138  **The list head is a node**, and these declarations are it.
     *
     * What is at 0x3144 is not the head - it is this node's `next`, and the
     * head is the node itself. Reading it that way is what makes the chain
     * work without special cases: a walk starts at entity_head and follows
     * links, and an unlink writes entity_prev->next whether the node it is
     * removing is the first or the fortieth.
     *
     * Read off the binary rather than assumed. 1ac2:323e is `mov bx, 0x3138`
     * - the address, not its contents - and the loop after it reads
     * [bx + 0xc], so its first read is the word at 0x3144. That 0x3144 is
     * where the chain starts is settled by the six routines that begin a walk
     * by loading it (1ac2:1b53 is the play loop's own entity walk, 1ac2:055e
     * is entities_clear) and the clear paths that store an immediate into it
     * at 1ac2:0554, 0591 and 0e11.
     *
     * The other three variables live *inside* the head node's payload, which
     * is why they are spelled as a union rather than described in a comment:
     * if those seven bytes ever turn out to be doing a handler's work too,
     * this is where it would show. The pool's first real node is one stride
     * on - the shipped free list runs 0x3146, 0x3154, 0x3162, ... - so the
     * head is element -1 of the array. */
    union {
        entity_t entity_head;           /* 0x3138 the head, as a node */
        struct {
            uint16_t entity_free;       /* 0x3138 = entity_head.handler:
                                         * the free list's first node */
            uint8_t  entity_remove;     /* 0x313a a handler asking to be
                                         * taken out of the list */
            uint8_t  _pad_head[7];
            uint16_t entity_prev;       /* 0x3142 trails one node behind a
                                         * walk, so an unlink needs no
                                         * second pass */
        };                              /* 0x3144 is entity_head.next */
    };
    uint8_t  _pad_14[574];
    uint8_t  bonus_cap;                 /* 0x3384 */
    uint8_t  _pad_15[77];
    uint16_t rng_state;                 /* 0x33d2 */
    uint8_t  hit_kind;                  /* 0x33d4 */
    uint8_t  _pad_16[1];
    uint8_t  bonus_live;                /* 0x33d6 capsules on screen; the play loop's pause shortens as it rises */
    uint8_t  _pad_17[28];
    uint8_t  hatch_x;                   /* 0x33f3 */
    uint8_t  hatch_y;                   /* 0x33f4 */
    uint8_t  _pad_18[90];
    player_t players[9];                /* 0x344f nine of them - screen_player_names stops at nine, and a tenth record would run into player_count at 0x3f08 */
    uint8_t  _pad_19[198];
    uint8_t  player_count;              /* 0x3f08 how many were entered */
    uint8_t  live_count;                /* 0x3f09 how many are still in. next_player hands over while this is more than one */
    uint8_t  cur_player;                /* 0x3f0a */
} game_vars;

/* The same bytes as g_image, which stays the buffer everything else - memcpy,
 * the snapshot loader, the verifier, exepack - works through. */
#define gv (*(game_vars *)g_image)

/* offsetof checked at compile time. _Static_assert is C11 and this is C99, so
 * it is the negative-array-size trick; the failure message names the field. */
#define ENSURE_IMG_AT(field, off) \
    typedef char ensure_img_at_##field[offsetof(game_vars, field) == (off) ? 1 : -1]

/* @generated-asserts begin - genvars.py rewrites between these markers */
ENSURE_IMG_AT(eog_screen_at, 0x13c0);
ENSURE_IMG_AT(eog_build_at, 0x13c2);
ENSURE_IMG_AT(banner_state, 0x13c4);
ENSURE_IMG_AT(banner_ptr, 0x13c5);
ENSURE_IMG_AT(lives, 0x13c9);
ENSURE_IMG_AT(level_src, 0x13ca);
ENSURE_IMG_AT(level_number, 0x13cc);
ENSURE_IMG_AT(score_text, 0x13cd);
ENSURE_IMG_AT(extra_at, 0x13d3);
ENSURE_IMG_AT(name_index, 0x13e9);
ENSURE_IMG_AT(level_num_text, 0x1410);
ENSURE_IMG_AT(particle_count, 0x1413);
ENSURE_IMG_AT(walker_anim, 0x1468);
ENSURE_IMG_AT(speed_step, 0x1485);
ENSURE_IMG_AT(speed_limit, 0x1486);
ENSURE_IMG_AT(frame_delay, 0x1487);
ENSURE_IMG_AT(frame_delay_set, 0x1489);
ENSURE_IMG_AT(speed_timer, 0x148b);
ENSURE_IMG_AT(paddle_step, 0x2d38);
ENSURE_IMG_AT(paddle_kind, 0x2d39);
ENSURE_IMG_AT(paddle_width, 0x2d3a);
ENSURE_IMG_AT(paddle_morphing, 0x2d3b);
ENSURE_IMG_AT(morph_owner, 0x2d3c);
ENSURE_IMG_AT(paddle_min, 0x2d3e);
ENSURE_IMG_AT(paddle_max, 0x2d3f);
ENSURE_IMG_AT(repeat_count, 0x2d40);
ENSURE_IMG_AT(input_active, 0x2d45);
ENSURE_IMG_AT(input_selected, 0x2d47);
ENSURE_IMG_AT(last_make, 0x2d49);
ENSURE_IMG_AT(last_dir, 0x2d4a);
ENSURE_IMG_AT(repeat_div, 0x2d4b);
ENSURE_IMG_AT(key_action, 0x2d4c);
ENSURE_IMG_AT(key_right, 0x2d4d);
ENSURE_IMG_AT(key_left, 0x2d4e);
ENSURE_IMG_AT(key_scan_l, 0x2d4f);
ENSURE_IMG_AT(key_scan_r, 0x2d50);
ENSURE_IMG_AT(key_scan_a, 0x2d51);
ENSURE_IMG_AT(paddle_x, 0x2e54);
ENSURE_IMG_AT(paddle_prev_x, 0x2e55);
ENSURE_IMG_AT(hold_offset, 0x2e56);
ENSURE_IMG_AT(ball_alive, 0x2e73);
ENSURE_IMG_AT(hit_count, 0x2e74);
ENSURE_IMG_AT(caught, 0x2e75);
ENSURE_IMG_AT(hold_timer, 0x2e76);
ENSURE_IMG_AT(game_over, 0x2e78);
ENSURE_IMG_AT(extra_on, 0x2e79);
ENSURE_IMG_AT(serve_timeout, 0x2e7a);
ENSURE_IMG_AT(extra_timer, 0x2e7c);
ENSURE_IMG_AT(laser_on, 0x2e7e);
ENSURE_IMG_AT(laser_y, 0x2e7f);
ENSURE_IMG_AT(laser_x, 0x2e80);
ENSURE_IMG_AT(net_on, 0x2e81);
ENSURE_IMG_AT(net_life, 0x2e82);
ENSURE_IMG_AT(net_timer, 0x2e84);
ENSURE_IMG_AT(net_pos, 0x2e85);
ENSURE_IMG_AT(extra_pos, 0x2e87);
ENSURE_IMG_AT(hit_dirs, 0x2e99);
ENSURE_IMG_AT(balls, 0x2ea1);
ENSURE_IMG_AT(backdrop_phase, 0x2efb);
ENSURE_IMG_AT(sweep_y, 0x2f0c);
ENSURE_IMG_AT(level, 0x2f10);
ENSURE_IMG_AT(anim_count, 0x3134);
ENSURE_IMG_AT(anim_rate, 0x3135);
ENSURE_IMG_AT(anim_ptr, 0x3136);
ENSURE_IMG_AT(entity_head, 0x3138);
ENSURE_IMG_AT(entity_free, 0x3138);
ENSURE_IMG_AT(entity_remove, 0x313a);
ENSURE_IMG_AT(entity_prev, 0x3142);
ENSURE_IMG_AT(bonus_cap, 0x3384);
ENSURE_IMG_AT(rng_state, 0x33d2);
ENSURE_IMG_AT(hit_kind, 0x33d4);
ENSURE_IMG_AT(bonus_live, 0x33d6);
ENSURE_IMG_AT(hatch_x, 0x33f3);
ENSURE_IMG_AT(hatch_y, 0x33f4);
ENSURE_IMG_AT(players, 0x344f);
ENSURE_IMG_AT(player_count, 0x3f08);
ENSURE_IMG_AT(live_count, 0x3f09);
ENSURE_IMG_AT(cur_player, 0x3f0a);
/* @generated-asserts end */

/* The two facts the chain rests on, checked rather than described: the head
 * node sits at 0x3138, and its `next` therefore lands on 0x3144 - the word
 * every walk starts from. Change the declarations inside the union, or the
 * seven bytes of payload between entity_remove and entity_prev, and the second
 * one stops holding; after that entity_alloc appends to the wrong place and
 * entity_unlink corrupts the list, both silently. This way the build stops. */
typedef char ensure_head_next_lands_on_3144[
    offsetof(game_vars, entity_head.next) == 0x3144 ? 1 : -1];
extern const char *g_dir;               /* "" - everything is relative to the
                                         * current directory, as it was in DOS */

/* The player's POPCORN.EXE: $POPCORN_EXE, else the usual places relative to
 * where the port was started. NULL if there is none. */
const char *find_exe(void);
/* find_exe, then unpack into g_image. Says what it found, or why it found
 * nothing. Returns the length, or 0. */
size_t popcorn_load_image(void);

/* Read the game's data out of the player's own POPCORN.EXE. */
uint8_t *exepack_load(const char *path, size_t *out_len);

/* --------------------------------------------------------------- video ---
 *
 * CGA mode 05h: 320x200, four colours, two bits per pixel, most significant
 * pair leftmost.  Memory is interlaced - even scan lines from offset 0, odd
 * from 0x2000, 80 bytes to a row either way - and the game's own row-stepping
 * idiom is all over its code:
 *
 *     cmp di,0x2000 / jb + / sub di,0x1fb0 / jmp ++ / +: add di,0x2000
 *
 * The port keeps that layout.  Flattening it would mean rewriting every
 * drawing routine instead of transcribing it, and the interlace is visible in
 * the game's arithmetic, not hidden behind an accessor.
 */
#define CGA_W        320
#define CGA_H        200
#define CGA_STRIDE    80               /* bytes per scan line */
#define CGA_PLANE 0x2000               /* offset of the odd-line half */
#define CGA_SIZE  0x4000

extern uint8_t g_vram[CGA_SIZE];

/* Step a CGA offset on by one scan line, the way the game does. */
static inline uint32_t cga_next_row(uint32_t di)
{
    return di < CGA_PLANE ? di + CGA_PLANE : di - (CGA_PLANE - CGA_STRIDE);
}

/* And back up one. The test is `cmp di,0x2000` and then whichever branch the
 * routine happens to use, and they are not all the same - which matters only
 * at exactly 0x2000, and there it matters a lot. Counted over the whole code
 * segment: 103 sites step down with `jb`, five step up treating 0x2000 as the
 * odd half (`jae` at 0xc4c, 0x46e5, 0x483a and `jb` at 0x9f3, 0x1320), and
 * four - all inside intro_logo - use `ja` and treat 0x2000 as the even half.
 *
 * Getting this wrong sends an offset of 0x2000 to 0x3fb0, past the bottom of
 * the visible screen into the padding at the end of the plane. That is what
 * the frame's scroll was doing. */
static inline uint32_t cga_prev_row(uint32_t di)
{
    return di >= CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
}

/* intro_logo's pair: `ja`, so 0x2000 goes the other way. Only 1ac2:54d6 uses
 * these, at 54f6, 5535, 557a and 55b6. */
static inline uint32_t cga_prev_row_ja(uint32_t di)
{
    return di > CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
}

static inline uint32_t cga_next_row_ja(uint32_t di)
{
    return di > CGA_PLANE ? di - (CGA_PLANE - CGA_STRIDE) : di + CGA_PLANE;
}

/* One of the 35 relocated segment constants: the program reaches a block of
 * its own data as segment 0xc46, which is image offset 0xc460. */
#define SEG_C46 0xc460

/* The four colours mode 05h displays on an RGB monitor: the colour-burst-kill
 * bit selects background / cyan / red / white regardless of the palette bit. */
extern uint32_t g_palette[4];

/* --------------------------------------------------------------- input ---
 *
 * The three bytes the game's INT 09h handler maintains, and the only thing its
 * keyboard input routine at 1ac2:16d2 reads.  Named here because they are the
 * whole keyboard interface.
 */

/* --------------------------------------------------------------- state ---
 *
 * Offsets of the game variables identified so far.  These are not C
 * declarations: they are indices into g_image, so that a transcribed routine
 * reads the same address the disassembly shows.
 */
#define PADDLE_W          28
#define PADDLE_ROW       186

/* The ball pool: four entries of 0x1e bytes at 0x2ea1, walked by the play loop
 * at 1ac2:1873 and stepped by the Bresenham routine at 1ac2:27d7. */
#define BALL_STRIDE   0x1e
#define BALL_COUNT       3             /* was 4, and never used - the pool is three */
#define B_X           0x00             /* the LIVE position */
#define B_Y           0x01
#define B_DIR_X       0x14             /* non-zero negates that axis */
#define B_DIR_Y       0x15
#define B_DY          0x16             /* the slope is stored (dy, dx) */
#define B_DX          0x17
#define B_ANCHOR_X    0x18             /* where this straight segment began */
#define B_ANCHOR_Y    0x19
#define B_ACC_X       0x1a             /* Bresenham, counting from the anchor */
#define B_ACC_Y       0x1b
#define B_STATE       0x1c             /* 0 idle, 1-2 in play */
#define B_PREV_X      0x02             /* where it was, so XOR can undo it */
#define B_PREV_Y      0x03
#define B_SPRITE      0x04             /* four words: what is on screen now */
#define B_PREV_SPR    0x0c             /* four words: what to erase */

/* The entity list the play loop walks: a chain from the head link at 0x3144,
 * each node carrying its handler at +0x00 and the next link at +0x0c, with
 * 0xffff terminating.  The node pool is at 0x3146, stride 0x0e. */
#define ENTITY_POOL   0x3146
#define ENTITY_STRIDE 0x0e

/* --------------------------------------------------------- the backend ---
 *
 * What the game needs from the platform.  sdl_io.c is the one implementation;
 * the split exists so that the game code below it never mentions SDL.
 */
int32_t  io_init(int32_t scale);
void io_shutdown(void);
void io_present(void);                 /* g_vram to the screen */

/* Lockstep, for sidebyside.py: the port runs a frame and hands the whole
 * machine over, and the driver hands back the input for the next one. */
int32_t  io_lockstep(void);
uint32_t io_lockstep_mouse_x(void);
uint32_t io_lockstep_buttons(void);
void io_lockstep_warp(uint32_t x);
void io_frame_sync(void);              /* at 1ac2:1c3f, the frame's close */
void io_log_random(uint32_t dl);       /* one game_random, for sidebyside */
int32_t  lockstep_main(const char *state_path);
uint64_t io_ms(void);             /* wall clock, for measurement only */
int32_t  io_pump(void);                    /* poll input; 0 means quit */
void io_wait_retrace(void);            /* what `in al,0x3da` / test 8 did */
void io_sound(uint32_t divisor);       /* PIT channel 2; 0 silences */
void io_delay_cycles(uint32_t cycles); /* what the busy-wait at 0x164c cost */
void io_frame_pace(void);              /* one play-loop frame, against the 60 Hz screen */
extern uint32_t g_play_hz;             /* measured play-loop rate, 326 Hz */
int32_t  io_key_ready(void);               /* INT 16h AH=01 */
uint32_t io_get_key(void);             /* INT 16h AH=00: scan<<8 | ascii */
void io_flush_keys(void);
void io_script_key_shift(uint32_t scan, uint32_t ms, int32_t shift);
#define io_script_key(scan, ms) io_script_key_shift((scan), (ms), 0)
int32_t  io_save_shot(const char *path);
void io_set_deadline(uint32_t ms, const char *shot, const char *vram);
void io_set_deadline_image(const char *path);
void io_set_int09_installed(int32_t on);

/* Little-endian accessors, so a transcribed `mov ax,[0x3144]` reads the way it
 * reads in the disassembly. */
static inline uint32_t img_w(uint32_t off)
{
    return (uint32_t)g_image[off] | ((uint32_t)g_image[off + 1] << 8);
}
static inline void img_setw(uint32_t off, uint32_t v)
{
    g_image[off] = (uint8_t)v;
    g_image[off + 1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------- the game code ---
 *
 * Each carries the image offset it was transcribed from; verify.c maps those
 * offsets to these calls, and the mapping of registers to arguments is part of
 * what the check asserts.
 */
void ball_step(ball_t *b);                          /* 1ac2:27d7 */
void input_keyboard(void);                              /* 1ac2:1712 */
void input_mouse(uint32_t mouse_x, uint32_t buttons);   /* 1ac2:169f */
void save_screen(void);                                 /* 1ac2:5099 */
void restore_screen(void);                              /* 1ac2:50bc */
void paddle_row_offsets(uint32_t x, uint32_t rows_out); /* 1ac2:22de */
void blit_xor(uint32_t pixels, uint32_t rows);          /* 1ac2:2281 */
void draw_paddle(uint32_t sprite);                      /* 1ac2:221a */
void draw_char(uint8_t c, uint32_t di);           /* 1ac2:0c64 */
uint32_t game_random(uint32_t ticks, uint32_t limit);   /* 1ac2:40c0 */
void speaker_on(void);                                  /* 1ac2:0085 */
void speaker_off(void);                                 /* 1ac2:0090 */
void sound_tick(void);                                  /* 1ac2:0097 */
void io_set_grab(int32_t on);
/* Opt-in lockstep sync points, one bit each, for comparing inside a screen
 * that has a loop of its own and so reaches io_frame_sync not at all. */
#ifndef SYNC_SCROLL
#define SYNC_SCROLL   1                 /* screen_scroll_up, once a row */
#define SYNC_ENDGAME  2                 /* ball_after_endgame, once a step */
#define SYNC_RESULTS  4                 /* screen_results' wait, once a pass */
#define SYNC_CURTAIN  8                 /* the ending animation, once a pass */
#define SYNC_ENDING  16                 /* after level 49, once a pass */
#define SYNC_INTRO   32                 /* the level intro, once a pass */
#endif
void io_frame_sync_extra(int32_t which);
void io_lockstep_extra_sync(int32_t mask);
int32_t io_grabbed(void);
void game_delay(void);                                  /* 1ac2:164c */
void read_speed_setting(uint32_t speed);                /* 1ac2:5680 */
void build_shifted_sprites(void);                       /* 1ac2:14b3 */
void load_high_scores(const char *dir);                 /* 1ac2:4d96 */
void intro_curtain(void);                               /* 1ac2:078b */
void intro_logo(void);                                  /* 1ac2:54d6 */
void intro_reveal(void);                                /* 1ac2:55e5 */
void intro_scroll(void);                                /* 1ac2:4a7a */
void intro_paddle(void);                                /* 1ac2:49bc */
void int09_handler(uint32_t scan);                      /* 1ac2:03e3 */
int32_t  drive_check(void);                                 /* 1ac2:4dea */
int32_t  drive_writable(void);                              /* 1ac2:4e04 */
void game_main(const char *dir, const char *levels);

/* --------------------------------------------------------- not yet done ---
 * Implemented as no-ops in stubs.c; see the note at the top of that file.
 */
void menu_particles_init(uint32_t ax_in);   /* 1ac2:5476 */
void plot_pixel(uint32_t x, uint32_t y, uint32_t colour);
void plot_pixel_xor(uint32_t x, uint32_t y, uint32_t colour);
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void banner_shift(void);          /* 1ac2:5140 */
void brick_11_after(uint32_t x, uint32_t y);  /* 1ac2:4c4b */
uint32_t particle_random(uint32_t ax, uint32_t ticks, uint32_t limit); /* 1ac2:5448 */
uint32_t particle_init(uint32_t si, uint32_t ax_in);  /* 1ac2:548a */
#define PARTICLES 0x148d
void menu_arrow(void);            /* 1ac2:490d */
void arrow_head(uint32_t di);     /* 1ac2:492f */
void arrow_tail(uint32_t di);     /* 1ac2:4957 */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void hsc_sort(void);              /* 1ac2:4d37 */
void hsc_save(const char *dir);   /* 1ac2:4dbb */
void border_draw(uint32_t di);    /* 1ac2:4ff1 */
void border_erase(uint32_t di);   /* 1ac2:4fd3 */
uint32_t border_step(uint32_t di);/* 1ac2:4fa7 */
void border_animate(void);        /* 1ac2:4f58 */
void border_row(uint32_t di);     /* 1ac2:5019 */
void border_block(uint32_t di);   /* 1ac2:5045 */
void palette_cycle(void);         /* 1ac2:5196 */
void flush_keys(void);            /* 1ac2:0106 */
void install_int09(void);         /* 1ac2:03b0 */
void restore_int09(void);         /* 1ac2:03d1 */
void input_and_draw_paddle(void); /* 1ac2:48af */
void cheat_match(uint8_t c);/* 1ac2:5171 */
void io_cga_mode(uint32_t v);
void io_cga_colour(uint32_t v);
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void cell_hole_draw(uint32_t x, uint32_t y);  /* 1ac2:4cc1 */
void screen_unstash(void);        /* 1ac2:4c13 */
void border_setup(void);          /* 1ac2:4f73 */
int32_t  tall_sprite(uint32_t *si, uint32_t di);   /* 1ac2:538d */
void screen_scroll_up(void);      /* 1ac2:4878 */
void level_tally(void);           /* 1ac2:48ce */
void screen_stash(void);          /* 1ac2:4ba9 */
void screen_restore(void);        /* 1ac2:4b4f */
void demo_start(void);
void input_demo(void);            /* 1ac2:1785 */
int32_t  cheat_sequence(uint8_t key); /* 1ac2:58b3 */            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */

int32_t  level_load_file(const char *dir);  /* 1ac2:08c8 */
void set_palette_registers(uint32_t table);  /* 1ac2:4b7a */
uint8_t screen_player_names(void);  /* 1ac2:10de */
int32_t  name_field(uint32_t di, uint8_t *abort); /* 1ac2:13b8 */
void play_frame(void);            /* 1ac2:1212 */
uint32_t frame_band(uint32_t di, uint32_t fill);  /* 1ac2:1354 */
void panel_reveal(void);          /* 1ac2:0911 */
void field_marks(void);           /* 1ac2:0598 */
void field_marks_wide(uint32_t di, uint32_t rows);  /* 1ac2:0a1d */
uint32_t ending_particle_init(uint32_t si, uint32_t ax_in); /* 1ac2:59f7 */
void ending_blob(uint32_t pos);   /* 1ac2:5c36 */
uint32_t ending_blobs(void);          /* 1ac2:5b80 */
void ending_column(void);         /* 1ac2:5317 */

/* A word into the framebuffer, wrapping like the 16-bit offset it is. */
static inline void img_vram_setw(uint32_t di, uint32_t v)
{
    g_vram[di & (CGA_SIZE - 1)] = (uint8_t)v;
    g_vram[(di + 1) & (CGA_SIZE - 1)] = (uint8_t)(v >> 8);
}
void panel_finish(void);          /* 1ac2:09c5 */

/* Called by play_loop; the ones still empty live in stubs.c too. */
extern int32_t g_resume_at_frame_top;   /* lockstep: skip play_loop's prologue */
extern int32_t g_resume_in_session;     /* lockstep: rejoin play_session's retry loop */
extern int32_t g_resume_in_bonus;       /* lockstep: rejoin inside the bonus */
extern int32_t g_start_level;           /* --level N, or -1 for the first */
extern int32_t g_in_bonus;              /* the end-of-level bonus is running */
int32_t  play_loop(void);             /* 1ac2:1873 - transcribed */
uint32_t draw_text(uint32_t src, uint32_t count, uint32_t di);  /* 1ac2:10d1 */
void level_draw(void);            /* 1ac2:1c4f */
void walker_draw(uint32_t x);     /* 1ac2:1e50 */
void walker_step(uint32_t x);     /* 1ac2:1e23 */
void ball_draw(const void *rows, uint32_t x, uint32_t y);    /* 1ac2:2881 */
int32_t  ball_redraw(ball_t *b);  /* 1ac2:2827 */
int32_t  ball_on_paddle(ball_t *b); /* 1ac2:2e1e */
void read_new_key(uint32_t which);  /* 1ac2:1614 */
int32_t  score_before(const uint8_t *a, uint32_t di); /* 1ac2:108c */
void ball_after(ball_t *b);   /* 1ac2:247f */
int32_t  ball_after_endgame(ball_t *b);  /* 1ac2:45a1 */
void ball_bricks(ball_t *b);  /* 1ac2:254d */
void brick_hit(uint32_t slot, uint32_t cell, ball_t *ball);
void xor_sprite_16xn(uint32_t x, uint32_t y, uint32_t src, uint32_t rows); /* 1ac2:40f2 */
void brick_1(uint32_t slot, ball_t *ball);     /* 1ac2:28cb */
void brick_2(uint32_t slot, ball_t *ball);     /* 1ac2:2985 */
void brick_3(uint32_t slot, ball_t *ball);     /* 1ac2:2a3f */
void brick_solid(uint32_t slot, ball_t *ball);       /* 1ac2:3221 */
void brick_animated(uint32_t slot, ball_t *ball);   /* 1ac2:2ccd */
void entity_anim_brick(ent_brick_t *a);                 /* 1ac2:3abf */
void draw_anim_cell(uint32_t si, uint32_t x, uint32_t y); /* 1ac2:3bac */
void brick_5(uint32_t slot, ball_t *ball);     /* 1ac2:2a73 */
void brick_6(uint32_t slot, ball_t *ball);     /* 1ac2:2ab4 */
void brick_7(uint32_t slot, ball_t *ball);     /* 1ac2:2af5 */
void brick_8(uint32_t slot, ball_t *ball);     /* 1ac2:2b36 */
void brick_9(uint32_t slot, ball_t *ball);     /* 1ac2:2b9d */
void entity_soften(ent_anim_t *a);      /* 1ac2:365e */
void entity_repeat(ent_anim_t *a);      /* 1ac2:366f */
void entity_plain(ent_anim_t *a);       /* 1ac2:3696 */
void entity_ball_arrive(ent_anim_t *a); /* 1ac2:36a1 */
void entity_cells_timer(ent_cells_t *a); /* 1ac2:36f6 */
void brick_10(uint32_t slot, ball_t *ball);    /* 1ac2:2c59 */
void brick_11(uint32_t slot, ball_t *ball);    /* 1ac2:2d68 */
void xor_sprite_16x7(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:3b64 */
void score_add(void);             /* 1ac2:413d */
void extra_life(void);            /* 1ac2:318b */
void fill_column(uint32_t di, uint32_t value);  /* 1ac2:41b1 */
void bonus_points(void);          /* 1ac2:2daa */
void bonus_catch(void);           /* 1ac2:2def */
void bonus_laser(void);           /* 1ac2:2e03 */
void bonus_multiball(void);       /* 1ac2:2e16 */
void bonus_wider_paddle(void);         /* 1ac2:3231 */
void bonus_net(void);             /* 1ac2:3119 */
void bonus_reverse(void);         /* 1ac2:315b */
void bonus_slower_ball(void);     /* 1ac2:31e8 */
void bonus_stop_monsters(void);   /* 1ac2:3200 */
int32_t bonus_end_level(void);    /* 1ac2:2da0 */
int32_t bonus_end_level_body(void); /* 1ac2:4210 */
void bonus_effect(uint32_t kind);
void scroll_up_band(void);        /* 1ac2:2109 */
void scroll_down_band(void);      /* 1ac2:2148 */
void draw_paddle_raw(uint32_t src);      /* 1ac2:22a9 */
void draw_paddle_shifted(uint32_t src);  /* 1ac2:2187 */
void ball_paddle(ball_t *b);  /* 1ac2:2316 */
void laser_fire(void);            /* 1ac2:2ee3 */
void probe_cell_at(uint32_t x, uint32_t y, uint32_t slot); /* 1ac2:2755 */
void play_teardown(void);         /* 1ac2:41d4 */
void entity_call(entity_t *e);  /* the call at 1ac2:1b5e */
void entity_capsule(entity_t *e);   /* 1ac2:3273 */
void entity_paddle_fx(entity_t *e); /* 1ac2:3386 */
void morph_begin(ent_morph_t *m, uint32_t table, uint32_t kind); /* 1ac2:34c5 */
void morph_step(ent_morph_t *m);       /* 1ac2:34d7 */
void entity_popup(entity_t *e);     /* 1ac2:3561 */
void entity_capsule_frames(entity_t *e, uint32_t table);
void entity_ball_hold(ent_anim_t *a); /* 1ac2:37e0 */
void ball_place(ball_t *ball, uint32_t x, uint32_t y);
void bonus_update(ent_sprite_t *s, uint32_t nx, uint32_t ny); /* 1ac2:3df1 */
uint32_t pixel_xor(uint32_t x, uint32_t y);        /* 1ac2:30dd */
void shot_xor(uint32_t x, uint32_t y);             /* 1ac2:306b */
void bonus_hits_ball(const ent_sprite_t *s, const ball_t *ball);  /* 1ac2:3f20 */
void entity_bonus(entity_t *e);     /* 1ac2:39fa */
void entity_unknown(uint32_t bx);
void entity_multiball(entity_t *e);  /* 1ac2:3717 */
void entity_unlink(uint32_t node);/* 1ac2:3257 */
uint32_t entity_alloc(void);      /* 1ac2:3232 */
uint32_t draw_run(uint8_t c, uint32_t count, uint32_t di); /* 1ac2:10c5 */
void draw_cursor(uint32_t di);    /* 1ac2:14a7 */
void define_keys_prompt(uint32_t src, uint32_t dst);            /* 1ac2:1642 */
void flash_bar(uint32_t pattern); /* 1ac2:3146 */
void cell_set_three(uint32_t node);/* 1ac2:3668 */
void cells_restore(void);         /* 1ac2:36fb */

void bonus_spawn(void);           /* 1ac2:3d95 */
void xor_sprite_20x16(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:406a */
void sprite_shift_draw(uint32_t x, uint32_t y, uint32_t src);/* 1ac2:3f4f */
void entity_sparkle(ent_anim_t *a); /* 1ac2:3aee */
void entity_crumble(ent_anim_t *a); /* 1ac2:3b2a */
void entity_hatch(entity_t *e);   /* 1ac2:390d */
void bonus_release(const ent_hatch_t *h);  /* 1ac2:39a1 */
int32_t  bonus_move_right(ent_bonus_t *b, uint32_t *px, uint32_t *py); /* 1ac2:3c66 */
int32_t  bonus_move_left(ent_bonus_t *b, uint32_t *px, uint32_t *py);  /* 1ac2:3cf3 */
int32_t  bonus_move_up(ent_bonus_t *b, uint32_t *px, uint32_t *py);    /* 1ac2:3caf */
int32_t  bonus_move_down(ent_bonus_t *b, uint32_t *px, uint32_t *py);  /* 1ac2:3d3c */
int32_t  bonus_steer(ent_bonus_t *b, uint32_t *px, uint32_t *py);  /* 1ac2:3bf7 */
int32_t  bonus_script(ent_bonus_t *b, uint32_t *px, uint32_t *py); /* 1ac2:3c35 */
void demo_input_step(void);       /* 1ac2:1a6f */
void drop_duplicate_hits(void);   /* 1ac2:27b7 */
uint32_t hsc_bubble(uint32_t si, uint32_t di); /* 1ac2:4d5d */
void game_input(void);            /* calls whichever routine [0x2d45] names */
void io_mouse_warp(uint32_t x, uint32_t y);
uint32_t io_ticks(void);
void io_set_ticks(uint32_t t);
void io_pin_mouse(uint32_t x, uint32_t buttons);
/* The bot, in autoplay.c. --autoplay on popcorn-dev; it plays through
 * io_pin_mouse, which is the same door lockstep uses. */
void autoplay_enable(uint32_t seed);
int32_t autoplay_on(void);
void autoplay_step(void);
void io_pin_key(uint32_t k);
uint32_t io_mouse_x(void);
uint32_t io_mouse_buttons(void);

extern jmp_buf g_back_to_menu;
extern jmp_buf g_bonus_done;
void play_session(void);          /* 1ac2:02f5 */
void panel_draw(void);            /* 1ac2:0b0b */
void level_colours(void);         /* 1ac2:044b */
void level_intro(void);           /* 1ac2:1eb9 */
uint32_t draw_brick_row(uint32_t y);  /* 1ac2:2034 */
void draw_sprite_20x6(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:20b9 */
void cell_special(uint32_t row, uint32_t col, uint32_t di); /* 1ac2:41e5 */
void field_backdrop(uint32_t y);  /* 1ac2:1fc1 */
void life_lost(void);             /* 1ac2:0735 */
void entities_clear(void);        /* 1ac2:055e */
void level_between(void);         /* 1ac2:05f8 */
void screen_game_over(void);      /* 1ac2:0473 */
void ending_plot(uint32_t x, uint32_t y);     /* 1ac2:5add */
void ending_particles_init(uint32_t ax); /* 1ac2:5a43 */
void ending_particles_tick(void); /* 1ac2:5a56 */
int32_t next_player(const char *dir);/* 1ac2:0d2e */
void screen_results(const char *dir);   /* 1ac2:0ea3 */
void screen_end_of_game(void);
void screen_level_done(void);     /* 1ac2:0521 */
void screen_all_levels_done(void);/* 1ac2:5940 */
uint32_t ending_walk(uint32_t bl, uint32_t bh, uint32_t dx); /* 1ac2:5bb5 */

int32_t verify_main(const char *in_path, const char *out_path);

#endif /* POPCORN_GAME_H */
