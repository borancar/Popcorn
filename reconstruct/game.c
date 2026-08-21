/*
 * Popcorn, reconstructed from the disassembly of POPCORN.EXE
 * (Christophe Lacaze / Frédérick Raynal, LACRAL software, 1988).
 *
 * Every routine carries the image offset it was read from, so any line can be
 * checked against the binary.  Where a name or a type is a guess it says so.
 *
 * The original is hand-written assembly, not compiler output, so there are no
 * stack frames to recover and no calling convention to reverse: arguments
 * arrive in registers and are threaded across calls.  Written here as
 * structured C with named parameters, not as transliterated register
 * shuffling - and where a routine genuinely cannot be written that way, it is
 * written literally and the comment says why.
 */
#include <string.h>

#include "game.h"

unsigned char *g_image;

/* ------------------------------------------------------------------------
 * 1ac2:27d7  ball_step
 *
 * Advance one ball along its current straight segment.  This is a Bresenham
 * line stepper, and reading it is what pins down the whole ball structure.
 *
 * The original branches on which axis is major and multiplies out the minor
 * one.  In both branches the offsets it produces satisfy
 *
 *     x_offset / y_offset = [+0x17] / [+0x16]
 *
 * so +0x17 is the horizontal component of the slope and +0x16 the vertical -
 * the pair is stored (dy, dx), which is the opposite of how it reads.  The
 * accumulators at +0x1a/+0x1b count *steps along the major axis* away from
 * the anchor at +0x18/+0x19, which is where the segment began; the live
 * position written to +0x00/+0x01 is anchor plus offset.  So +0x18/+0x19 is
 * not where the ball is, and stays put until the next bounce.
 */
void ball_step(unsigned ball)
{
    unsigned char *b = g_image + ball;
    unsigned dy = b[B_DY], dx = b[B_DX];
    unsigned off_x, off_y;

    if (dx >= dy) {                     /* x is the major axis */
        off_x = b[B_ACC_X];
        off_y = dy ? (unsigned)(off_x * dy) / dx : 0;
        b[B_ACC_X]++;
    } else {                            /* y is the major axis */
        off_y = b[B_ACC_Y];
        off_x = dx ? (unsigned)(off_y * dx) / dy : 0;
        b[B_ACC_Y]++;
    }
    /* The direction flags negate each axis independently. The original does
     * this in 8-bit registers and lets the add wrap, which is what keeps a
     * ball at the left wall from running off into high coordinates. */
    int sx = b[B_DIR_X] ? -(int)off_x : (int)off_x;
    int sy = b[B_DIR_Y] ? -(int)off_y : (int)off_y;
    b[B_X] = (unsigned char)(b[B_ANCHOR_X] + sx);
    b[B_Y] = (unsigned char)(b[B_ANCHOR_Y] + sy);
}

/* ------------------------------------------------------------------------
 * 1ac2:1712  input_keyboard  (the tail of the handler at 1ac2:16d2; the head
 *                             deals with pause and the in-game function keys)
 *
 * Move the paddle by one pixel per repeat tick. [0x2d40] counts down to the
 * next allowed step and [0x2d4b] is the divider it reloads from - and that
 * divider is itself decremented on every step, so the paddle accelerates the
 * longer it is kept moving, down to one pixel per tick.
 *
 * Nothing here reads the keyboard: the three state bytes are maintained by the
 * INT 09h handler at 1ac2:03e3, which the platform layer stands in for.
 *
 * The equal case is not "do nothing". With neither key held the acceleration
 * is reset to 5 and the paddle is clamped to the right-hand limit; with
 * *both* held it moves in the direction of whichever key was pressed most
 * recently, which the INT 09h handler records at [0x2d4a].
 */
#define REPEAT_COUNT  0x2d40
#define REPEAT_DIV    0x2d4b
#define LAST_DIR      0x2d4a            /* 0 = left, 1 = right */
#define REPEAT_RESET  5

void input_keyboard(void)
{
    if (--g_image[REPEAT_COUNT] != 0)
        return;
    if (g_image[REPEAT_DIV] != 1)
        g_image[REPEAT_DIV]--;
    g_image[REPEAT_COUNT] = g_image[REPEAT_DIV];

    int go_left = g_image[KEY_LEFT] != 0;
    if (g_image[KEY_LEFT] == g_image[KEY_RIGHT]) {
        if (!g_image[KEY_LEFT]) {              /* neither key held */
            g_image[REPEAT_COUNT] = REPEAT_RESET;
            g_image[REPEAT_DIV] = REPEAT_RESET;
            if (g_image[PADDLE_X] > g_image[PADDLE_MAX])
                g_image[PADDLE_X] = g_image[PADDLE_MAX];
            return;
        }
        go_left = g_image[LAST_DIR] == 0;      /* both: the most recent wins */
    }

    /* Both limits are compared unsigned, after an 8-bit inc or dec that is
     * allowed to wrap. At x = 0 the decrement gives 0xff, which is `jae` the
     * minimum and so is kept - the original has no guard against that, and it
     * never comes up because x never gets below the minimum in the first
     * place. Transcribed as it is rather than as it should be. */
    if (!go_left) {
        unsigned char x = (unsigned char)(g_image[PADDLE_X] + 1);
        if (x > g_image[PADDLE_MAX])
            x = g_image[PADDLE_MAX];
        g_image[PADDLE_X] = x;
    } else {
        unsigned char x = (unsigned char)(g_image[PADDLE_X] - 1);
        if (x < g_image[PADDLE_MIN])
            x = g_image[PADDLE_MIN];
        g_image[PADDLE_X] = x;
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:169f  input_mouse  (the tail of the mouse handler at 1ac2:1654)
 *
 * Absolute, and it lands on the very next frame:
 *
 *     mov ax,3 / int 33h        CX = x in the 640-wide virtual screen
 *     and bx,3 / setnz          either button is the action key
 *     shr cx,1                  paddle x = mouse x / 2
 *     clamp cl to [0x2d3e]..[0x2d3f]
 *
 * Note the clamp is on CL alone, after the shift - the high byte is discarded,
 * not saturated.  Kept here because a pointer beyond 510 would wrap rather
 * than pin to the right wall, and that is the game's behaviour.
 */
void input_mouse(unsigned mouse_x, unsigned buttons)
{
    g_image[KEY_ACTION] = (buttons & 3) ? 1 : 0;

    unsigned char x = (unsigned char)((mouse_x >> 1) & 0xff);
    if (x > g_image[PADDLE_MAX])
        x = g_image[PADDLE_MAX];
    else if (x < g_image[PADDLE_MIN])
        x = g_image[PADDLE_MIN];
    g_image[PADDLE_X] = x;
}

/* ------------------------------------------------------------------------
 * 1ac2:5099 / 1ac2:50bc  save_screen / restore_screen
 *
 * Both halves of the CGA aperture to and from a 32,000-byte buffer at image
 * 0x10250 (the original reaches it as 0xc46:0x3df0, one of the 35 relocated
 * segment constants).  Used around anything that draws over the menu.
 */
#define SCREEN_SAVE  0x10250
/* Two `rep movsw` of 0xfa0 words each, from 0xb800:0000 and 0xb800:2000. That
 * is 8,000 bytes a half - the 200 visible scan lines at 80 bytes for every
 * other one - not the 8,192 each half of the aperture spans. The 192 bytes of
 * padding at the end of each half are neither saved nor restored, and the two
 * halves land adjacent in the buffer rather than 0x2000 apart, so a save is
 * 16,000 bytes and not a copy of the aperture. */
#define SCREEN_HALF  8000

void save_screen(void)
{
    memcpy(g_image + SCREEN_SAVE, g_vram, SCREEN_HALF);
    memcpy(g_image + SCREEN_SAVE + SCREEN_HALF, g_vram + CGA_PLANE,
           SCREEN_HALF);
}

void restore_screen(void)
{
    memcpy(g_vram, g_image + SCREEN_SAVE, SCREEN_HALF);
    memcpy(g_vram + CGA_PLANE, g_image + SCREEN_SAVE + SCREEN_HALF,
           SCREEN_HALF);
}

/* ------------------------------------------------------------------------
 * The drawing primitives.
 *
 * Everything the game draws goes through XOR: it keeps the bytes it last put
 * on screen, XORs them off, then XORs the new ones on. That is why each
 * moving object owns two buffers - the current bitmap and the previous one -
 * and two tables of row offsets to go with them. It also means there is no
 * background to restore and no dirty-rectangle bookkeeping: a sprite drawn
 * twice is gone.
 *
 * Sprites are stored four times over, pre-shifted by one pixel each, because
 * at two bits per pixel a byte holds four pixels and the CPU has no barrel
 * shifter worth using. The copy for a given x is at `sprite + (x & 3) *
 * bytes_per_image`.
 */

/* 1ac2:22de  paddle_row_offsets
 *
 * Seven CGA offsets, one per scan line of the paddle, from its x.
 *
 *     ax = (x >> 2) + 0x1cc0
 *
 * `x >> 2` because four pixels share a byte; 0x1cc0 is the offset of the
 * paddle's first row - 7360 bytes into the even half, which is 92 rows of 80,
 * so scan line 184. The alternating `+0x2000` / `-0x1fb0` after it is the
 * interlace step, the same one cga_next_row() spells out.
 */
#define PADDLE_ROW_BASE 0x1cc0
#define PADDLE_ROWS          7
#define PADDLE_BYTES        11          /* five words and a byte: 44 pixels */
#define PADDLE_IMAGE      0x4d          /* PADDLE_ROWS * PADDLE_BYTES */

void paddle_row_offsets(unsigned x, unsigned rows_out)
{
    unsigned off = (x >> 2) + PADDLE_ROW_BASE;
    for (int r = 0; r < PADDLE_ROWS; r++) {
        img_setw(rows_out + r * 2, off);
        off = cga_next_row(off);
    }
}

/* 1ac2:2281  blit_xor
 *
 * XOR one paddle-shaped bitmap into the framebuffer: seven rows of eleven
 * bytes, each row at the offset the table gives. The original runs with
 * interrupts off, because it writes CGA memory without waiting for retrace and
 * a timer tick in the middle would show as a tear.
 */
void blit_xor(unsigned pixels, unsigned rows)
{
    for (int r = 0; r < PADDLE_ROWS; r++) {
        unsigned di = img_w(rows + r * 2);
        for (int b = 0; b < PADDLE_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^=
                g_image[pixels + r * PADDLE_BYTES + b];
    }
}

/* 1ac2:221a  draw_paddle
 *
 * Move the paddle to its current x. `sprite` is the base of a four-way
 * pre-shifted image - the game passes a different one for the paddle carrying
 * a ball, for the laser paddle, and so on.
 *
 * The early-out is the whole reason the previous position is kept: if the
 * paddle has not moved and no redraw was forced by [0x2d3b], there is nothing
 * to do, and XOR-ing it off and back on would flicker it.
 */
#define PADDLE_FORCE_DRAW  0x2d3b
#define PADDLE_PREV_X      0x2e55
#define PADDLE_ROWS_CUR    0x2e57       /* seven words */
#define PADDLE_ROWS_PREV   0x2e65
#define PADDLE_PIX_CUR     0x2d8c       /* PADDLE_IMAGE bytes */
#define PADDLE_PIX_PREV    0x2ddc

void draw_paddle(unsigned sprite)
{
    if (!g_image[PADDLE_FORCE_DRAW] &&
        g_image[PADDLE_X] == g_image[PADDLE_PREV_X])
        return;

    /* `sub word [0x1487], 0x1e0`, taken only when the paddle actually moves.
     * [0x1487] is a countdown the play loop reloads from [0x1489] (0x1f4 at
     * the start of a level), and 0x2187 also touches it. Most likely the level
     * time bonus, charged for moving; transcribed as it is because what it
     * means does not change what it does. */
    img_setw(0x1487, (img_w(0x1487) - 0x1e0) & 0xffff);

    /* What is on screen now becomes what has to be erased. */
    memcpy(g_image + PADDLE_ROWS_PREV, g_image + PADDLE_ROWS_CUR,
           PADDLE_ROWS * 2);
    memcpy(g_image + PADDLE_PIX_PREV, g_image + PADDLE_PIX_CUR,
           PADDLE_IMAGE + 1);

    unsigned x = g_image[PADDLE_X];
    g_image[PADDLE_PREV_X] = (unsigned char)x;
    paddle_row_offsets(x, PADDLE_ROWS_CUR);

    /* Pick the copy pre-shifted to this pixel within its byte. */
    memcpy(g_image + PADDLE_PIX_CUR,
           g_image + sprite + (x & 3) * PADDLE_IMAGE, PADDLE_IMAGE + 1);

    blit_xor(PADDLE_PIX_PREV, PADDLE_ROWS_PREV);   /* erase where it was */
    blit_xor(PADDLE_PIX_CUR, PADDLE_ROWS_CUR);     /* draw where it is */
}

/* ------------------------------------------------------------------------
 * 1ac2:0c64  draw_char
 *
 * One glyph of the score-panel font at `di`, an offset into the framebuffer.
 * The table at 0x9020 holds 24 bytes per glyph: twelve rows of one word,
 * which at two bits per pixel is an 8x12 cell.
 *
 * The character-to-glyph map is the original's, verbatim - it has no glyphs
 * for lower case and none for punctuation beyond a dash and a colon, so
 * anything else lands on the space, which is a solid block of colour 2 rather
 * than blank. That is deliberate: it is how the red bars behind the headings
 * are painted.
 */
#define FONT       0x9020
#define FONT_ROWS      12
#define FONT_GLYPH     24

static unsigned glyph_of(unsigned char c)
{
    if (c == ':')  return 0x26;
    if (c == 0xff) return 0x27;         /* the text-entry cursor */
    if (c == '-')  return 0x0b;
    if (c >= '0' && c <= '9') return c - 0x2f;
    if (c >= 'A' && c <= 'Z') return c - 0x35;
    return 0;                           /* space, and everything unmapped */
}

void draw_char(unsigned char c, unsigned di)
{
    const unsigned char *g = g_image + FONT + glyph_of(c) * FONT_GLYPH;
    for (int r = 0; r < FONT_ROWS; r++) {
        g_vram[di & (CGA_SIZE - 1)] = g[r * 2];
        g_vram[(di + 1) & (CGA_SIZE - 1)] = g[r * 2 + 1];
        di = cga_next_row(di);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:40c0  game_random
 *
 * Returns a value in 0..limit-1. The original stirs together the BIOS tick
 * counter at 0040:006c, ten words of live game state at 0x3164, and a counter
 * at 0x33d2 that it advances by 0x5ec5 each call, then divides and takes the
 * remainder.
 *
 * The tick counter is the only part the port has to stand in for; everything
 * else is in the image. `ticks` is passed in rather than read here so that a
 * replay can be made deterministic by feeding it a fixed sequence.
 */
#define RNG_STIR   0x3164               /* ten words folded in */
#define RNG_STATE  0x33d2

unsigned game_random(unsigned ticks, unsigned limit)
{
    unsigned ax = ticks & 0xffff;
    for (int i = 0; i < 10; i++)
        ax = (ax + img_w(RNG_STIR + i * 2)) & 0xffff;
    ax = (ax + img_w(RNG_STATE)) & 0xffff;
    img_setw(RNG_STATE, (img_w(RNG_STATE) + 0x5ec5) & 0xffff);

    /* `add al,ah` then `xor ah,ah` then `div dl`: folded to eight bits before
     * the divide, so the result really is only ever 0..255 wide. */
    unsigned al = ((ax & 0xff) + (ax >> 8)) & 0xff;
    return limit ? al % limit : 0;
}
