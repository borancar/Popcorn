/*
 * The bot, in the port.
 *
 * A transcription of autoplay.py's `Bot`, which until now could only drive the
 * port from outside through the lockstep protocol - a Python process, an
 * emulator alongside it, and a pipe. `popcorn-dev --autoplay` needs none of
 * that: the same decisions, made in the same order, against the same image.
 *
 * It plays through the **mouse**, which is what makes the paddle absolute: the
 * pointer says where the paddle is and it is there on the very next frame, so
 * a decision does not have to be spread over several frames of travel. The
 * keyboard path exists in autoplay.py only to exercise 1ac2:16d2 and is not
 * transcribed here.
 *
 * Every address below is the game's own, and every one is read - nothing here
 * writes to the image. The bot sees exactly what a player sees, and then some:
 * a falling capsule is an entity in a chain rather than a table, so finding
 * one means walking the list the way the play loop does at 1ac2:1b4d.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

/* ------------------------------------------------------------- the state */
#define PADDLE_X        0x2e54          /* left edge, pixels */
#define PADDLE_MIN      0x2d3e
#define PADDLE_MAX      0x2d3f
#define PADDLE_WIDTH    0x2d3a          /* live; the morphs change it */
#define PADDLE_KIND     0x2d39
#define BOT_PADDLE_W    27              /* fallback only: 0x2d0d+2 */
#define PADDLE_Y        186
#define PADDLE_LIP      3               /* `left = paddle_x - 3` */
#define BOUNCE_Y        0xb5
#define CEILING_Y       6
#define WALL_L          9               /* the ball's turning points, */
#define WALL_R          195             /* measured in play */

#define BALLS           0x2ea1
#define BALL_STRIDE     0x1e
#define BALL_COUNT      4
#define B_X             0x00
#define B_Y             0x01
#define B_DIRX          0x14
#define B_DIRY          0x15
/* Stored (dy, dx), which is the opposite of how it reads: both branches of the
 * stepper at 1ac2:27d7 come out as x_offset / y_offset = [+0x17] / [+0x16].
 * The other way round makes every predicted landing wrong by the square of the
 * slope - the paddle sits somewhere plausible and catches the ball only when
 * the geometry happens to agree, which looks exactly like randomness. */
#define B_DY            0x16
#define B_DX            0x17
#define B_STATE         0x1c

#define ENTITY_HEAD     0x3144
#define E_NEXT          0x0c
#define H_CAPSULE       0x3273          /* entity_capsule */
#define H_PARACHUTE     0x37e0          /* entity_ball_hold */
#define C_X             0x02
#define C_Y             0x03
#define C_KIND          0x04
#define P_X             0x04
#define P_Y             0x05
#define PARACHUTE_BOTTOM 0xb8           /* where it lets go */
#define PARACHUTE_HALF  8
#define CAPSULE_W       0x0e
#define CATCH_Y         0xb6            /* the first row the paddle can take it */

#define LEVEL_CELLS     0x2f18          /* 0x2f10 + 8, past the header */
#define BRICK_COLS      12
#define BRICK_ROWS      14
#define BRICK_LEFT_PX   8
#define CELL_W          16
#define CELL_H          8
#define FIELD_TOP       6

#define SLOPE_TOP       0x2e2c          /* the paddle-end slope table */
#define SLOPE_N         11
#define LASER_PADDLE    2
#define LASER_CAPSULE   3
#define SHOT_SPACING    0x13

#define SAFETY_FRAMES   40
#define LAST_BALL_FRAMES 140            /* one ball: the whole descent */
/* A constant could not work. ball_paddle takes off = ball_x - (paddle_x - 3);
off <= 0x0a is the left end and off >= span - 0x0a the right, and each indexes
the slope table at 0x2e2c for a fresh (dy, dx). Anything between them - the
middle 0x0b..span-0x0b - **keeps the slope the ball arrived with**.

The bot centres the paddle, so the ball lands at off = width/2 + 3 - wander.
With the default 27-wide paddle that is 16 - wander, and the middle band is
11..19: a wander of +-3 lands in it every time. The jitter existed to break
the cycle where the paddle and the ball retrace one path, and it could not,
because it never changed the slope - it only moved where on the flat middle
the ball struck.

An end needs wander >= width/2 - 7 or <= 10 - width/2, and staying on the
paddle at all needs wander in [-width/2, width/2 + 3]. Both scale with the
width, which E changes from 27 to 39, so the range does too: width/2 - 3
reaches both ends on either paddle and lands short of falling off. */
#define JITTER_HOLD     24

static int32_t indestructible(uint8_t v)
{
    return v == 4 || v == 12 || (v >= 24 && v <= 29);
}

/* Higher is chased first; below zero is never chased at all. L is top - the
 * laser clears bricks on its own and is the only capsule that survives
 * collecting another. Then + for the level, F for the net, then V and E, which
 * buy survival. R is refused: it pays a hundred points and then cancels the
 * net and the stopped-monsters state, undoing the one capsule that gets a
 * parachuted ball back. S is refused because it slows the ball. */
static int32_t capsule_want(uint8_t kind)
{
    switch (kind) {
    case 3:  return 5;                  /* L, the laser */
    case 8:  return 4;                  /* +, the level is over */
    case 5:  return 3;                  /* F, the net */
    case 7:  return 2;                  /* V, a life */
    case 2:  return 2;                  /* E, a wider paddle */
    case 10: return 1;                  /* M, the monsters stop */
    case 1:  return 1;                  /* C, catch */
    case 4:  return 1;                  /* T, more balls */
    case 6:  return 0;                  /* I, reverse */
    case 0:  return -1;                 /* R, cancels the net */
    case 9:  return -1;                 /* S, slows the ball */
    default: return 0;
    }
}

static uint8_t rd(uint32_t off)        { return g_image[off]; }
static uint32_t rw(uint32_t off)       { return img_w(off); }

/* ------------------------------------------------------------ the reading */
struct ball { int32_t x, y, dy_up, dx_neg, dx, dy; };

static int32_t live_balls(struct ball *out)
{
    int32_t n = 0;
    for (int32_t i = 0; i < BALL_COUNT; i++) {
        uint32_t b = BALLS + (uint32_t)i * BALL_STRIDE;
        uint8_t st = rd(b + B_STATE);
        if (st != 1 && st != 2)         /* 3 is brick 9, 4 the parachute */
            continue;
        out[n].x = rd(b + B_X);
        out[n].y = rd(b + B_Y);
        out[n].dy_up = rd(b + B_DIRY) != 0;
        out[n].dx_neg = rd(b + B_DIRX) != 0;
        out[n].dx = rd(b + B_DX) ? rd(b + B_DX) : 1;
        out[n].dy = rd(b + B_DY) ? rd(b + B_DY) : 1;
        n++;
    }
    return n;
}

/* Every live node running this handler. Entities are not in a table - they are
 * a chain whose handler word says what each one is - so this is the same walk
 * the play loop does. */
static int32_t entities(uint32_t handler, uint32_t *out, int32_t max)
{
    int32_t n = 0;
    uint32_t bx = rw(ENTITY_HEAD);
    for (int32_t guard = 0; guard < 64 && bx != 0xffff; guard++) {
        if (rw(bx) == handler && n < max)
            out[n++] = bx;
        bx = rw(bx + E_NEXT);
    }
    return n;
}

static int32_t paddle_width(void)
{
    uint8_t w = rd(PADDLE_WIDTH);
    return w ? w : BOT_PADDLE_W;
}

/* ---------------------------------------------------------- the geometry */

/* Where a descending ball reaches the paddle row, walls included. The stepper
 * is Bresenham over (dx, dy), so net travel is dx across for every dy down; a
 * bounce is then a fold of the straight-line answer back into the playfield,
 * which handles a shallow angle crossing more than once without a loop. */
static int32_t predict(int32_t x, int32_t y, int32_t dx, int32_t dy,
                       int32_t x_neg)
{
    int32_t drop = PADDLE_Y - y;
    if (drop <= 0 || dy == 0)
        return x;
    int32_t end = x_neg ? x - drop * dx / dy : x + drop * dx / dy;
    int32_t span = WALL_R - WALL_L;
    if (span <= 0)
        return x;
    int32_t k = (end - WALL_L) % (2 * span);
    if (k < 0)
        k += 2 * span;
    return WALL_L + (k <= span ? k : 2 * span - k);
}

/* Roughly how many frames until this ball reaches the paddle row. Both parts
 * are approximations; what they have to be right about is the *order* of "a
 * long way off" and "nearly here". */
static int32_t frames_to_paddle(const struct ball *b)
{
    int32_t major = b->dx > b->dy ? b->dx : b->dy;
    if (major <= 0)
        major = 1;
    if (b->dy <= 0)
        return 9999;
    int32_t drop = b->dy_up ? (b->y - CEILING_Y) + (PADDLE_Y - CEILING_Y)
                            : PADDLE_Y - b->y;
    if (drop <= 0)
        return 0;
    /* drop / (dy/major) * 1.5, in integers. */
    return drop * major * 3 / (b->dy * 2);
}

/* Where a ball leaving (bx, BOUNCE_Y) at this slope crosses row ty. */
static int32_t travel_to(int32_t bx, int32_t dy, int32_t dx, int32_t dir_x,
                         int32_t ty)
{
    int32_t rise = BOUNCE_Y - ty;
    if (rise <= 0 || dy == 0)
        return bx;
    int32_t end = dir_x ? bx - rise * dx / dy : bx + rise * dx / dy;
    int32_t span = WALL_R - WALL_L;
    if (span <= 0)
        return bx;
    int32_t k = (end - WALL_L) % (2 * span);
    if (k < 0)
        k += 2 * span;
    return WALL_L + (k <= span ? k : 2 * span - k);
}

/* The lowest breakable cell with nothing directly under it - the field is
 * approached from below, so a cell with something under it cannot be reached.
 * Nearest the middle wins, so the aim is a correction rather than a lunge. */
static int32_t aim_target(int32_t *tx, int32_t *ty)
{
    for (int32_t r = BRICK_ROWS - 1; r >= 0; r--) {
        int32_t best = -1, bx = 0, by = 0;
        for (int32_t c = 0; c < BRICK_COLS; c++) {
            uint8_t v = rd(LEVEL_CELLS + (uint32_t)(r * BRICK_COLS + c));
            if (!v || indestructible(v))
                continue;
            if (r + 1 < BRICK_ROWS
                && rd(LEVEL_CELLS + (uint32_t)((r + 1) * BRICK_COLS + c)))
                continue;
            int32_t x = BRICK_LEFT_PX + c * CELL_W + CELL_W / 2;
            int32_t y = FIELD_TOP + r * CELL_H + CELL_H / 2;
            int32_t d = x > 100 ? x - 100 : 100 - x;
            if (best < 0 || d < best) {
                best = d; bx = x; by = y;
            }
        }
        if (best >= 0) {
            *tx = bx; *ty = by;
            return 1;                   /* the lowest row that has one wins */
        }
    }
    return 0;
}

/* The paddle x that sends the ball nearest (tx, ty), or -1. The paddle's two
 * ends choose the bounce: ball_paddle indexes 0x2e2c by how far in from an end
 * the ball lands, and the middle keeps whatever slope the ball arrived with -
 * which is why a bot that always centres can return a ball but never place
 * one. */
static int32_t aim_at(int32_t bx, int32_t tx, int32_t ty)
{
    int32_t lo = rd(PADDLE_MIN), hi = rd(PADDLE_MAX);
    int32_t span = (paddle_width() + PADDLE_LIP) & 0xff;
    int32_t best = -1, best_px = -1;
    for (int32_t off = 0; off < SLOPE_N; off++) {
        uint32_t w = rw(SLOPE_TOP + (uint32_t)off * 2);
        int32_t dy = (int32_t)(w & 0xff), dx = (int32_t)(w >> 8);
        if (!dy)
            continue;
        for (int32_t end = 0; end < 2; end++) {
            /* Left end: off is measured from paddle_x - 3. Right end: the
             * same table indexed from the other side. dir_x 1 sends the ball
             * left, 0 right. */
            int32_t px = end ? bx - (span - off) + PADDLE_LIP
                             : bx - off + PADDLE_LIP;
            if (px < lo || px > hi)
                continue;
            int32_t at = travel_to(bx, dy, dx, end ? 0 : 1, ty);
            int32_t miss = at > tx ? at - tx : tx - at;
            if (best < 0 || miss < best) {
                best = miss; best_px = px;
            }
        }
    }
    return best_px;
}

/* Columns worth shooting at, as the pixel centre of each. A shot meets the
 * **lowest** brick in its column, so a column whose lowest brick is
 * indestructible swallows every shot for nothing. */
static int32_t laser_columns(int32_t *out)
{
    int32_t n = 0;
    for (int32_t c = 0; c < BRICK_COLS; c++) {
        for (int32_t r = BRICK_ROWS - 1; r >= 0; r--) {
            uint8_t v = rd(LEVEL_CELLS + (uint32_t)(r * BRICK_COLS + c));
            if (!v)
                continue;
            if (!indestructible(v))
                out[n++] = BRICK_LEFT_PX + c * CELL_W + CELL_W / 2;
            break;
        }
    }
    return n;
}

/* ---------------------------------------------------------- the decision */
static int32_t enabled;
static uint32_t rng_state = 1;
static int32_t wander, held;

/* A seeded generator, so a run is reproducible. */
static int32_t next_wander(int32_t width)
{
    /* width/2 - 7 is the least that reaches both ends; a little more than
     * that, because the ball lands where the *prediction* said and the
     * prediction is not exact. Not much more: every pixel of wander is a
     * pixel less margin for the prediction being wrong. This leaves nine on
     * the near side of either paddle. */
    int32_t j = width / 2 - 6;
    if (j < 6)
        j = 6;
    rng_state = rng_state * 1103515245u + 12345u;
    return (int32_t)((rng_state >> 16) % (uint32_t)(2 * j + 1)) - j;
}

void autoplay_enable(uint32_t seed)
{
    enabled = 1;
    rng_state = seed ? seed : 1;
}

int32_t autoplay_on(void)
{
    return enabled;
}

void autoplay_step(void)
{
    if (!g_image)
        return;
    int32_t lo = rd(PADDLE_MIN), hi = rd(PADDLE_MAX);
    int32_t px = rd(PADDLE_X);
    int32_t width = paddle_width();

    /* Everything the paddle has to be under, soonest first. A ball under a
     * parachute is not among the live ones - its own x and y stop moving while
     * the carrier does - so it comes in as the carrier's position, or the bot
     * simply watches it fall. */
    int32_t spare = 0, aim = 0, count = 0;
    struct ball live[BALL_COUNT];
    int32_t nlive = live_balls(live);
    for (int32_t i = 0; i < nlive; i++) {
        int32_t f = frames_to_paddle(&live[i]);
        int32_t at = live[i].dy_up ? live[i].x
                   : predict(live[i].x, live[i].y, live[i].dx, live[i].dy,
                             live[i].dx_neg);
        if (!count || f < spare) { spare = f; aim = at; }
        count++;
    }
    uint32_t nodes[16];
    int32_t np = entities(H_PARACHUTE, nodes, 16);
    for (int32_t i = 0; i < np; i++) {
        int32_t cy = rd(nodes[i] + P_Y), cx = rd(nodes[i] + P_X);
        int32_t f = PARACHUTE_BOTTOM - cy;
        if (f < 0)
            f = 0;
        if (!count || f < spare) { spare = f; aim = cx + PARACHUTE_HALF; }
        count++;
    }

    if (!count) {
        /* Between lives and between levels. Hold the button so the serve goes
         * out the moment the game asks for it, and leave the paddle alone. */
        io_pin_mouse((uint32_t)(2 * px), 1);
        return;
    }

    int32_t grabbing = 0;
    int32_t margin = count == 1 ? LAST_BALL_FRAMES : SAFETY_FRAMES;
    if (spare > margin) {
        int32_t laser = rd(PADDLE_KIND) == LASER_PADDLE;
        int32_t cols[BRICK_COLS];
        int32_t ncols = laser ? laser_columns(cols) : 0;

        /* Holding the laser is worth more than anything a capsule gives, and
         * there is no way to take one without losing it: every kind maps to
         * some paddle through 0x2d2d and only L maps back. So while it is held
         * the bot collects nothing else - but only while the laser can still
         * do something. Level 10 walls its own top off with a row of cell 3,
         * which brick_3 hardens into an unbreakable 4; once every column ends
         * in one, a bot that holds the laser and refuses everything survives
         * there for ever without clearing it. */
        int32_t nc = entities(H_CAPSULE, nodes, 16);
        int32_t best = 0, best_x = 0, best_y = -1;
        for (int32_t i = 0; i < nc; i++) {
            uint8_t kind = rd(nodes[i] + C_KIND);
            int32_t want = capsule_want(kind);
            int32_t cy = rd(nodes[i] + C_Y);
            if (want <= 0 || cy >= CATCH_Y)
                continue;
            if (laser && ncols && kind != LASER_CAPSULE)
                continue;
            /* Best first, and among equals the one that lands soonest. */
            if (want > best || (want == best && cy > best_y)) {
                best = want; best_x = rd(nodes[i] + C_X); best_y = cy;
            }
        }
        if (best) {
            /* Centre on centre: the capsule spans cx..cx+0x0e and the paddle
             * px..px+width, so px = cx + (0x0e - width) / 2. */
            aim = best_x + (CAPSULE_W - width) / 2 + width / 2;
            grabbing = 1;
        } else if (laser && ncols) {
            /* Nothing to collect and time to spare: put a shot into a column
             * that still has something breakable at the bottom of it. The
             * action button is held permanently, so standing in the right
             * place *is* firing. */
            int32_t near = cols[0];
            for (int32_t i = 1; i < ncols; i++) {
                int32_t d = cols[i] > px ? cols[i] - px : px - cols[i];
                int32_t bd = near > px ? near - px : px - near;
                if (d < bd)
                    near = cols[i];
            }
            /* Either end of the paddle can do it; use whichever is less of a
             * move from where the paddle already is. */
            int32_t left = near - SHOT_SPACING;
            int32_t dn = near > px ? near - px : px - near;
            int32_t dl = left > px ? left - px : px - left;
            aim = (dn <= dl ? near : left) + width / 2;
        }
    }

    /* Placing the ball rather than merely returning it, and only when there is
     * exactly one thing to watch and time to spare: the aim puts the paddle
     * where the ball meets an *end*, which is a smaller target than the
     * middle. */
    int32_t want = -1;
    if (count == 1 && spare > SAFETY_FRAMES && !grabbing) {
        int32_t tx, ty;
        if (aim_target(&tx, &ty))
            want = aim_at(aim, tx, ty);
    }

    /* A few pixels of wander, held for a while so the aim is steady across one
     * approach rather than shaking every frame. Without it the bot returns the
     * ball off the same part of the paddle every time and the two settle into
     * a cycle that clears nothing. */
    if (held <= 0) {
        wander = next_wander(width);
        held = JITTER_HOLD;
    }
    held--;
    if (want < 0)
        want = aim - width / 2 + wander;
    if (want < lo) want = lo;
    if (want > hi) want = hi;

    /* The game reads only CL after `shr cx,1`, so the pointer has to stay
     * inside 0..510 for the paddle position to survive the truncation. */
    int32_t mx = 2 * want;
    if (mx > 510)
        mx = 510;
    io_pin_mouse((uint32_t)mx, 1);
}
