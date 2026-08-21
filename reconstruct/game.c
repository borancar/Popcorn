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
 * 1ac2:16d2  input_keyboard  (the tail from 0x172f; the head handles pause
 *                             and the in-game function keys)
 *
 * Move the paddle by one pixel per repeat tick.  [0x2d40] counts down to the
 * next allowed step and [0x2d4b] is the divider it reloads from - which is
 * itself decremented while a key is held, so the paddle accelerates the
 * longer one direction is kept down, and starts over when it is not.
 *
 * Nothing here reads the keyboard: the three state bytes are maintained by
 * the INT 09h handler at 1ac2:03e3, which the platform layer stands in for.
 */
#define REPEAT_COUNT  0x2d40
#define REPEAT_DIV    0x2d4b

void input_keyboard(void)
{
    if (--g_image[REPEAT_COUNT] != 0)
        return;
    if (g_image[REPEAT_DIV] != 1)
        g_image[REPEAT_DIV]--;
    g_image[REPEAT_COUNT] = g_image[REPEAT_DIV];

    unsigned char left = g_image[KEY_LEFT], right = g_image[KEY_RIGHT];
    if (left == right)                  /* neither, or both: no movement */
        return;
    if (!left) {                        /* right is the one held */
        unsigned x = g_image[PADDLE_X] + 1;
        if (x > g_image[PADDLE_MAX])
            x = g_image[PADDLE_MAX];
        g_image[PADDLE_X] = (unsigned char)x;
    } else {
        unsigned x = (unsigned char)(g_image[PADDLE_X] - 1);
        if (x < g_image[PADDLE_MIN])
            x = g_image[PADDLE_MIN];
        g_image[PADDLE_X] = (unsigned char)x;
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

void save_screen(void)
{
    memcpy(g_image + SCREEN_SAVE, g_vram, CGA_SIZE);
}

void restore_screen(void)
{
    memcpy(g_vram, g_image + SCREEN_SAVE, CGA_SIZE);
}
