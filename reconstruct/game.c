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
#include <setjmp.h>
#include <stdio.h>
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
     *
     * [0x1487] is the frame's delay count: the play loop reloads it from
     * [0x1489] (0x1f4) at the top of every frame and spends it at the bottom
     * with `mov cx,[0x1487] / loop $`. Taking 0x1e0 off it here leaves 0x14
     * instead of 0x1f4, so a frame in which the paddle moved waits almost not
     * at all - this is compensation for what the redraw itself cost, which is
     * what keeps the game running at one speed whether the paddle is moving
     * or not. It is not a score. */
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

/* ========================================================================
 * Startup, timing and sound.
 * ===================================================================== */

/* Data the program keeps in its own code segment, reached as `cs:[...]`.
 * There are only a handful, and they are all here: the sound state and the
 * two bytes POPSPEED patches. */
#define CS_BASE        0x1ac20
#define SOUND_ON       (CS_BASE + 0x84)   /* F9 toggles this */
#define SOUND_REQUEST  (CS_BASE + 0xf4)   /* an id to start, 0 = nothing */
#define SOUND_TIMER    (CS_BASE + 0xf5)   /* ticks left on the current note */
#define SOUND_PTR      (CS_BASE + 0xf6)   /* where in the tune we are */
#define SOUND_TUNES    (CS_BASE + 0xf8)   /* table of tune addresses */
#define DELAY_ENTRY    (CS_BASE + 0x164c) /* patched to 0xc3 to disable */
#define DELAY_COUNT    (CS_BASE + 0x164e) /* the `mov cx,N` immediate */

static int g_speaker_on;

/* 1ac2:0085 / 1ac2:0090  speaker_on / speaker_off
 *
 * `mov al,0xb6 / out 0x43` puts PIT channel 2 in square-wave mode; port 0x61
 * bits 0 and 1 gate the counter and connect it to the speaker. The port has
 * no PIT, so this is just a flag the tone generator consults.
 */
void speaker_on(void)  { g_speaker_on = 1; }
void speaker_off(void) { g_speaker_on = 0; io_sound(0); }

/* 1ac2:0097  sound_tick
 *
 * Called once per frame from the play loop. A tune is a list of words, each
 * one a note - low byte the high byte of the PIT divisor, high byte how many
 * ticks to hold it - terminated by a zero word. `out 0x42,1` then
 * `out 0x42,al` makes the divisor `(al << 8) | 1`, so only the high byte ever
 * varies and the pitch is 1193182 / ((al << 8) | 1).
 *
 * A request is left at cs:[0xf4] as an id; the tick picks it up, looks the
 * tune's address out of the table at cs:0xf8 and starts it. Requests are
 * honoured even with sound off to the extent of being cleared - the flag is
 * checked after, so turning sound off does not leave a backlog.
 */
void sound_tick(void)
{
    unsigned char req = g_image[SOUND_REQUEST];

    if (req) {
        g_image[SOUND_REQUEST] = 0;
        if (!g_image[SOUND_ON])
            return;
        img_setw(SOUND_PTR, img_w(SOUND_TUNES + (req - 1) * 2));
        speaker_on();
        /* fall through and play the first note straight away */
    } else if (--g_image[SOUND_TIMER] != 0) {
        return;                         /* still holding the current note */
    }

    unsigned si = img_w(SOUND_PTR);
    unsigned note = img_w(si);
    if (note == 0) {                    /* end of tune */
        speaker_off();
        return;
    }
    img_setw(SOUND_PTR, si + 2);
    g_image[SOUND_TIMER] = (unsigned char)(note >> 8);
    io_sound(((note & 0xff) << 8) | 1);
}

/* 1ac2:164c  game_delay
 *
 * `push cx / mov cx,N / loop $ / pop cx / ret`, and N is what POPSPEED sets.
 * This is the game's entire notion of time apart from the retrace wait, so
 * getting its magnitude right is what makes the port run at the speed the
 * original did rather than merely at some speed.
 *
 * On an 8086 a taken `loop` is 17 cycles, so at the 8 MHz the readme names,
 * one iteration is 17/8e6 seconds. The platform layer accumulates these and
 * sleeps when enough has built up, because the game calls this in tight
 * loops - `mov cx,0x32 / call delay / loop` - and spinning would burn a core
 * to no purpose.
 */
#define CYCLES_PER_LOOP 17
#define CPU_HZ          8000000.0

void game_delay(void)
{
    if (g_image[DELAY_ENTRY] == 0xc3)   /* patched to a bare `ret` */
        return;
    io_delay_cycles(img_w(DELAY_COUNT) * CYCLES_PER_LOOP);
}

/* 1ac2:5680  read_speed_setting
 *
 * POPSPEED.EXE does not write a file: it stores its value in the **offset
 * half of interrupt vector 0x68**, and the game reads it back with INT 21h
 * AH=35h. 1 means "as fast as possible" and patches the delay's first byte to
 * a `ret`; 0 means POPSPEED was never run, and the default is 0x6f. Either
 * way the value is decremented and written into the `mov cx,N` immediate,
 * which is where the readme's "default 110" comes from.
 *
 * Nothing sets that vector under the port, so `speed` comes from the command
 * line or defaults the way an unrun POPSPEED would.
 */
void read_speed_setting(unsigned speed)
{
    if (speed == 1) {
        g_image[DELAY_ENTRY] = 0xc3;
        img_setw(DELAY_COUNT, 0);
        return;
    }
    if (speed == 0)
        speed = 0x6f;
    img_setw(DELAY_COUNT, speed - 1);
}

/* 1ac2:14b3  build_shifted_sprites
 *
 * The paddle images are stored once each and the other three pixel phases are
 * generated here, at startup, by shifting right two bits at a time - two bits
 * being one pixel at this depth. Four sprite sets, three shifts each, eleven
 * bytes by seven rows apiece.
 *
 * The shift is a `shr` on the first byte followed by ten `rcr`s, so a pixel
 * leaving one byte enters the next through the carry, and the leftmost two
 * bits of the row come in as zero. That is why sprite sets are laid out as
 * four consecutive 0x4d-byte images: `sprite + (x & 3) * 0x4d` indexes them.
 */
#define SPRITE_BASE   0x4903
#define SPRITE_SETS        4
#define SPRITE_SET_LEN 0x134            /* four images of 0x4d */

void build_shifted_sprites(void)
{
    unsigned bp = SPRITE_BASE;
    for (int set = 0; set < SPRITE_SETS; set++, bp += SPRITE_SET_LEN) {
        unsigned si = bp;
        for (int shift = 0; shift < 3; shift++, si += PADDLE_IMAGE) {
            unsigned di = si + PADDLE_IMAGE;
            memcpy(g_image + di, g_image + si, PADDLE_IMAGE);
            for (int row = 0; row < PADDLE_ROWS; row++) {
                unsigned r = di + row * PADDLE_BYTES;
                for (int twice = 0; twice < 2; twice++) {
                    unsigned carry = 0;
                    for (int b = 0; b < PADDLE_BYTES; b++) {
                        unsigned v = g_image[r + b];
                        g_image[r + b] = (unsigned char)((v >> 1) | (carry << 7));
                        carry = v & 1;
                    }
                }
            }
        }
    }
}

/* 1ac2:4d96  load_high_scores
 *
 * Opens the name at [0x141c] - "popcorn.hsc", with the drive letter the
 * program is running from patched into [0x141b] - and reads 0xb4 bytes into
 * [0x3e42]. A missing file is not an error: the table keeps whatever the
 * image shipped with, which is a full set of default entries.
 */
#define HSC_NAME   0x141c
#define HSC_TABLE  0x3e42
#define HSC_LEN     0xb4

void load_high_scores(const char *dir)
{
    char path[512];
    const char *name = (const char *)(g_image + HSC_NAME);
    snprintf(path, sizeof path, "%s%s", dir ? dir : "", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fread(g_image + HSC_TABLE, 1, HSC_LEN, f);
    fclose(f);
}

/* ========================================================================
 * The opening sequence.
 *
 * Four animations draw the title screen a piece at a time, and when they are
 * done main() saves the result. Everything the menu shows afterwards is that
 * saved screen restored - which is why these have to exist before anything
 * appears at all, cosmetic though they look.
 *
 * They all read from the block the program reaches as segment 0xc46.
 * ===================================================================== */

/* 1ac2:078b  intro_curtain
 *
 * Two things, in one routine. The first sweeps a dithered bar across the
 * screen: 0x1a columns, four phases each, and in every phase `ah` gains two
 * more set bits so that `al = 0x55 & ah` fills in one more pixel of the four
 * in a byte. Each phase draws two 49-pixel vertical runs, one in each half of
 * the interlace, spaced 0x50 bytes apart so they step a row at a time.
 *
 * The second grows the title lettering into the top right corner, a row at a
 * time from one to 0x69, reading backwards from 0x8318 so the picture grows
 * upwards from its own bottom edge.
 *
 * Between the two sits a colour remap that is easy to misread. It walks the
 * copy in the work buffer taking two source bits at a time and emitting two:
 * a leading 0 emits `00` and **discards the bit after it**, a leading 1 emits
 * `1` followed by that bit. So pixel value 1 becomes 0 and 2 and 3 are kept -
 * it strips the cyan out of the leading edge, which is what makes the
 * lettering fade in rather than snap on. Only the first 0xbd bytes get it,
 * seven rows, which is exactly the leading edge.
 */
#define CURTAIN_SRC   0x8318            /* read backwards from here */
#define CURTAIN_WORK  0x1aef            /* scratch, 0x1b * 0x69 bytes */
#define CURTAIN_ROW      0x1b           /* 27 bytes: 108 pixels */

void intro_curtain(void)
{
    unsigned ah = 0, dl = 0xff, di0 = 0x213f;

    for (int bx = 0x1a; bx > 0; bx--, di0--) {
        ah = 0;
        for (int dh = 4; dh > 0; dh--) {
            for (int i = 0; i < 0x32; i++)
                game_delay();
            ah = ((ah << 2) | 3) & 0xff;        /* two `stc; rcl ah,1` */
            unsigned al = 0x55 & ah;

            io_wait_retrace();
            unsigned di = di0;
            for (int cx = 0x31; cx > 0; cx--, di += 0x50)
                g_vram[di & (CGA_SIZE - 1)] = (unsigned char)al;
            di = (di0 - 0x1fb0) & 0xffff;
            for (int cx = 0x31; cx > 0; cx--, di += 0x50)
                g_vram[di & (CGA_SIZE - 1)] = (unsigned char)al;

            if (bx == 1) {
                /* The last column is drawn a second time from `dl`, which is
                 * rotated with carry *clear*, so it empties as `ah` fills. */
                dl = (dl << 2) & 0xff;
                al = 0x55 & dl;
                di = 0x213f;
                for (int cx = 0x31; cx > 0; cx--, di += 0x50)
                    g_vram[di & (CGA_SIZE - 1)] = (unsigned char)al;
                di = (0x213f - 0x1fb0) & 0xffff;
                for (int cx = 0x31; cx > 0; cx--, di += 0x50)
                    g_vram[di & (CGA_SIZE - 1)] = (unsigned char)al;
            }
        }
    }
    for (int i = 0; i < 0x32; i++)
        game_delay();

    for (unsigned rows = 1; rows != 0x6a; rows++) {
        unsigned n = (CURTAIN_ROW * (rows & 0xff)) & 0xffff;
        memcpy(g_image + CURTAIN_WORK, g_image + CURTAIN_SRC - n, n);

        for (unsigned i = 0; i < n && i < 0xbd; i++) {
            unsigned al = g_image[CURTAIN_WORK + i], out = 0;
            for (int k = 0; k < 4; k++) {
                unsigned hi = (al >> 7) & 1;
                al = (al << 1) & 0xff;
                unsigned lo = (al >> 7) & 1;
                al = (al << 1) & 0xff;
                out = hi ? (((out << 1) | 1) << 1 | lo) & 0xff
                         : (out << 2) & 0xff;
            }
            g_image[CURTAIN_WORK + i] = (unsigned char)out;
        }

        unsigned si = CURTAIN_WORK, di = 0x34;
        io_wait_retrace();
        for (unsigned r = 0; r < rows; r++) {
            for (int b = 0; b < CURTAIN_ROW; b++)
                g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[si + b];
            si += CURTAIN_ROW;
            di = cga_next_row(di);
        }
        for (int i = 0; i < 0x28; i++)
            game_delay();
    }
}

/* 1ac2:54d6  intro_logo
 *
 * The title lettering, drawn as four columns that grow from opposite corners.
 * Each pass copies a 24-byte slice of the source per scan line and drags a
 * twenty-byte white bar along beside it - the bar is drawn with `stosw` under
 * a set direction flag, so it trails the slice rather than leading it. The
 * second pass XORs the bar off again.
 *
 * `std` is in force for the first two passes: source and destination both walk
 * backwards, so the source pointer runs continuously up through the image
 * while the destination steps one scan line at a time.
 */
static void logo_pass(unsigned src, unsigned di0, int rows, int erase, int back)
{
    unsigned si = src, di = di0;
    for (int n = rows; n > 0; n--) {
        unsigned bx = di;
        for (int i = 0; i < 12; i++) {          /* 12 words */
            /* `movsw` copies the word **at** si and si+1 and *then* steps
             * both pointers - by +2 with the direction flag clear, by -2 with
             * it set. Writing at si-1 and si-2 for the backward case instead
             * shifts the whole picture by two bytes, which drew the
             * background and none of the lettering. */
            g_vram[di & (CGA_SIZE - 1)] = g_image[si];
            g_vram[(di + 1) & (CGA_SIZE - 1)] = g_image[si + 1];
            si = back ? si - 2 : si + 2;
            di = back ? di - 2 : di + 2;
        }
        di = back ? cga_prev_row(bx) : cga_next_row(bx);
        bx = di;

        /* The white bar that trails the slice. Backwards it starts four bytes
         * behind the row and walks down; forwards it starts at the row and
         * walks up. Twenty bytes either way. */
        unsigned w = back ? di - 4 : di;
        for (int i = 0; i < 10; i++) {
            unsigned a = w & (CGA_SIZE - 1), b = (w + 1) & (CGA_SIZE - 1);
            if (erase) {
                g_vram[a] ^= 0xff;
                g_vram[b] ^= 0xff;
            } else {
                g_vram[a] = 0xff;
                g_vram[b] = 0xff;
            }
            w = back ? w - 2 : w + 2;
        }
        di = bx;
        for (int i = 0; i < 0x32; i++)
            game_delay();
    }
}

void intro_logo(void)
{
    /* Two passes down with the direction flag set, then two up with it clear.
     * Each pair draws the slice then rubs the bar out again, so what is left
     * on screen is the picture and not the bar. */
    logo_pass(SEG_C46 + 0x7c6e, 0x3f3f, 0x5b, 0, 1);
    logo_pass(SEG_C46 + 0x7c6e, 0x3f3f, 0x5a, 1, 1);
    logo_pass(SEG_C46 + 0x6b60, 0x3119, 0x5b, 0, 0);
    logo_pass(SEG_C46 + 0x6b60, 0x1119, 0x5c, 1, 0);
}

/* 1ac2:55e5  intro_reveal
 *
 * Two halves. The first paints a dithered wipe down 0x34 columns in four
 * phases; the second reveals the picture band by band, widening the slice
 * copied from one byte to 0x34 and waiting for retrace on each step.
 */
void intro_reveal(void)
{
    unsigned bx0 = 0x230;
    for (int dl = 0x34; dl > 0; dl--, bx0++) {
        unsigned al = 0xc0;
        unsigned di0 = bx0;
        for (int dh = 4; dh > 0; dh--) {
            al &= 0x55;
            unsigned di = di0;
            for (int cl = 7; cl > 0; cl--, di += 0x370)
                g_vram[di & (CGA_SIZE - 1)] = (unsigned char)al;
            for (int i = 0; i < 0x19; i++)
                game_delay();
            al = ((al >> 1) | 0x80) & 0xff;     /* stc; rcr al,1 */
            al = ((al >> 1) | 0x80) & 0xff;
        }
    }

    unsigned si0 = SEG_C46 + 0x4db7, bp = 0xa0;
    for (int dh = 7; dh > 0; dh--, si0 += 0x444, bp += 0x370) {
        for (unsigned bx = 1; bx < 0x35; bx++) {
            unsigned si = si0 - (bx - 1), di = bp;
            io_wait_retrace();
            for (int dl = 0x15; dl > 0; dl--) {
                for (unsigned i = 0; i < bx; i++)
                    g_vram[(di + i) & (CGA_SIZE - 1)] = g_image[si + i];
                si += 0x34;
                di = cga_next_row(di);
            }
            for (int i = 0; i < 0x0a; i++)
                game_delay();
        }
    }
}

/* 1ac2:4a7a  intro_scroll
 *
 * The character at the bottom right, scrolled up into place: 0x1a frames, each
 * moving a 0x31-byte by 0x19-row block up one scan line and feeding a fresh
 * row in at the bottom from 0xc46:0x488a.
 */
void intro_scroll(void)
{
    unsigned bp = SEG_C46 + 0x488a;
    for (int bl = 0x1a; bl > 0; bl--) {
        unsigned di = 0x1b33;
        io_wait_retrace();
        for (int bh = 0x19; bh > 0; bh--) {
            unsigned src = cga_next_row(di);
            for (int i = 0; i < 0x31; i++)
                g_vram[(di + i) & (CGA_SIZE - 1)] =
                    g_vram[(src + i) & (CGA_SIZE - 1)];
            di = src;
        }
        for (int i = 0; i < 0x31; i++)
            g_vram[(0x3ef3 + i) & (CGA_SIZE - 1)] = g_image[bp + i];
        bp += 0x31;
        for (int i = 0; i < 0x19; i++)
            game_delay();
    }
}

/* ========================================================================
 * 1ac2:0113  game_main
 *
 * The program's own entry point, minus the parts that were the machine's job:
 * setting up its stack, copying the PSP command tail, and asking DOS for the
 * current drive. What is left is the startup sequence and the menu loop.
 *
 * One instruction is deliberately not transcribed. At 1ac2:01fb the original
 * does `mov ax,0xffff / mov cx,3 / mov di,0 / rep movsw` with ES still zero,
 * which copies six bytes over interrupt vectors 0 and 1 from wherever the
 * previous routine left SI - and the `mov ax` before it is dead, because
 * `movsw` does not read AX. It writes into the interrupt table and nothing
 * reads the result; there is no vector table here to write into, and
 * reproducing it would mean inventing one.
 * ===================================================================== */

/* Which input routine the menu is set to, and which the game will use. */
#define INPUT_SELECTED  0x2d47          /* 0x16d2 keyboard, 0x1654 mouse */
#define INPUT_ACTIVE    0x2d45
#define INPUT_KEYBOARD  0x16d2
#define INPUT_MOUSE     0x1654

#define BANNER_PTR      0x13c5          /* the scrolling text's cursor */
#define BANNER_STATE    0x13c4
#define PARTICLE_COUNT  0x1413

static void menu_redraw(void)
{
    speaker_off();
    io_flush_keys();
    restore_screen();
    if (img_w(INPUT_SELECTED) != INPUT_KEYBOARD)
        menu_arrow();
    img_setw(PARTICLE_COUNT, 0x50);
    menu_particles_init();
    g_image[BANNER_STATE] = 2;
    img_setw(BANNER_PTR, 0x3f1e);
}

void game_main(const char *dir, unsigned speed)
{
    read_speed_setting(speed);
    img_setw(INPUT_SELECTED, INPUT_KEYBOARD);
    g_image[0x3f1b] = 0;
    g_image[SOUND_ON] = 1;
    load_high_scores(dir);
    build_shifted_sprites();

    /* INT 10h AX=0005 - the window is the mode set. */
    intro_curtain();
    intro_logo();
    intro_reveal();
    intro_scroll();
    save_screen();

    for (;;) {
        menu_redraw();
        int back_to_menu = 0;
        while (!back_to_menu) {
            if (!io_pump())
                return;

            if (!io_key_ready()) {
                /* The idle path: step the decoration, and when the banner
                 * runs out of text start the demo, which is how the attract
                 * mode comes on by itself. */
                if (g_image[img_w(BANNER_PTR)] == 0) {
                    demo_prepare();
                    demo_start();
                    back_to_menu = 1;
                    continue;
                }
                menu_particles_tick();
                menu_banner_tick();
                io_present();
                io_wait_retrace();
                continue;
            }

            unsigned key = io_get_key();
            switch (key >> 8) {
            case 0x43:                                  /* F9: sound */
                g_image[SOUND_ON] ^= 1;
                break;
            case 0x42:                                  /* F8: palette */
                palette_cycle();
                break;
            case 0x41:                                  /* F7 */
                g_image[0x3f1b] = 0;
                img_setw(0x3f1c, 0x3f0b);
                break;
            case 0x44:                                  /* F10 */
                employee_enter();
                while (io_key_ready() && (io_get_key() >> 8) == 0x44)
                    ;
                employee_leave();
                break;
            case 0x3d:                                  /* F3: mouse */
                if (img_w(INPUT_SELECTED) != INPUT_MOUSE) {
                    img_setw(INPUT_SELECTED, INPUT_MOUSE);
                    menu_arrow();
                }
                break;
            case 0x3e:                                  /* F4: keyboard */
                if (img_w(INPUT_SELECTED) != INPUT_KEYBOARD) {
                    img_setw(INPUT_SELECTED, INPUT_KEYBOARD);
                    menu_arrow();
                }
                break;
            case 0x3f:                                  /* F5: define keys */
                screen_define_keys();
                back_to_menu = 1;
                break;
            case 0x40:                                  /* F6: high scores */
                screen_high_scores();
                back_to_menu = 1;
                break;
            case 0x3c:                                  /* F2: demo */
                demo_prepare();
                demo_start();
                back_to_menu = 1;
                break;
            case 0x3b:                                  /* F1: play */
                img_setw(INPUT_ACTIVE, img_w(INPUT_SELECTED));
                speaker_on();
                if (screen_player_names() == 0xff) {
                    back_to_menu = 1;
                    break;
                }
                play_prepare();
                /* play_session() never returns normally: it is left by the
                 * stack-throwing jump the original does at 1ac2:167e. */
                if (setjmp(g_back_to_menu) == 0)
                    play_session();
                back_to_menu = 1;
                break;
            case 0x01:                                  /* Esc: quit */
                return;
            default:
                break;
            }
            if (g_image[0x3f1b] != 1)
                menu_extra();
            io_present();
        }
    }
}

/* 1ac2:1ab1 and 1ac2:1a4f  game_input
 *
 * `call word ptr [0x2d45]` - whichever of the two input routines the menu
 * selected. The mouse one needs the pointer, which is the platform's; the
 * keyboard one reads only the three state bytes, which the platform maintains.
 */
void game_input(void)
{
    if (img_w(INPUT_ACTIVE) == INPUT_MOUSE)
        input_mouse(io_mouse_x(), io_mouse_buttons());
    else
        input_keyboard();
}

/* ========================================================================
 * 1ac2:1873  play_loop
 *
 * A level, from the moment it is drawn to the moment it is won or lost.
 * Returns 1 (the original's carry) when the last ball was lost and 0 when the
 * bricks ran out.
 *
 * The shape of a frame:
 *
 *     reload the frame delay from [0x1489]
 *     step the recorded-input cursor, if a demo is playing
 *     if [0x2e73] is clear the ball is gone      -> lost
 *     if [0x2f10] is clear the bricks are gone   -> won
 *     read the input through [0x2d45]
 *     draw the paddle
 *     step each of the three balls: move, collide, react
 *     walk the entity list, calling each node's handler
 *     age the laser shot and the extra ball
 *     sound_tick, then spend the frame delay
 *
 * The entity walk is the part worth reading twice: a handler may ask to be
 * taken out of the list by setting [0x313a], and the unlink needs the node
 * *before* it, which is why [0x3142] trails one step behind.
 * ===================================================================== */
#define LEVEL_NUMBER    0x13cc
#define LEVEL_NUM_TEXT  0x1410
#define LEVEL_CELLS     0x2f10          /* the copy the level is played from */
#define FRAME_DELAY     0x1487
#define FRAME_DELAY_SET 0x1489
#define BALL_ALIVE      0x2e73          /* clear when the last one is lost */
#define GAME_OVER       0x2e78
#define PADDLE_SUPPRESS 0x2d3b
#define PADDLE_KIND     0x2d39
#define PADDLE_SPRITES  0x2d0d          /* four-entry table of sprite bases */
#define LASER_ON        0x2e7e
#define SHOT_ON         0x2e81
#define SHOT_TIMER      0x2e84
#define SHOT_POS        0x2e85
#define SHOT_LIFE       0x2e82
#define EXTRA_ON        0x2e79
#define EXTRA_TIMER     0x2e7c
#define EXTRA_POS       0x2e87
#define SERVE_TIMEOUT   0x2e7a
#define CAUGHT          0x2e75
#define ENTITY_REMOVE   0x313a
#define ENTITY_PREV     0x3142
#define SPEED_TIMER     0x148b
#define SPEED_STEP      0x1485
#define SPEED_LIMIT     0x1486

static void erase_shot(unsigned pos_var, unsigned reload, unsigned timer)
{
    unsigned di = img_w(pos_var);
    g_vram[di & (CGA_SIZE - 1)] = 0;
    g_vram[(di + 1) & (CGA_SIZE - 1)] = 0;
    di = cga_next_row(di);
    img_setw(pos_var, di);
    g_image[timer] = (unsigned char)reload;
}

int play_loop(void)
{
    /* The level number, drawn into the header bar as two digits. */
    unsigned n = (g_image[LEVEL_NUMBER] + 1) & 0xff;
    img_setw(LEVEL_NUM_TEXT, ((n % 10) << 8 | (n / 10)) + 0x3030);

    unsigned di = 0x177e;
    for (int i = 0; i < 12; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] = 0xaa;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = 0xaa;
    }
    draw_text(0x1407, 0xc, 0x377e);
    di = 0x377e + 0x1e0;
    for (int i = 0; i < 12; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] = 0xaa;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = 0xaa;
    }

    level_draw();                       /* 1ac2:1c4f */

    /* Wipe the header bar again, upwards. */
    di = 0x177e;
    for (int dl = 0xe; dl > 0; dl--) {
        for (int i = 0; i < 24; i++)
            g_vram[(di + i) & (CGA_SIZE - 1)] = 0;
        di = cga_next_row((di - 0x18) & 0xffff);
    }

    g_image[PADDLE_X] = g_image[PADDLE_PREV_X] = 0x64;
    g_image[PADDLE_KIND] = 0;
    g_image[0x2d3a] = g_image[PADDLE_SPRITES + 3];
    g_image[PADDLE_MAX] = 0xac;
    g_image[PADDLE_MIN] = 0x08;
    g_image[REPEAT_COUNT] = 5;
    g_image[REPEAT_DIV] = 5;
    io_mouse_warp(0x64 * 2, 0xb8);

    paddle_row_offsets(g_image[PADDLE_X], PADDLE_ROWS_CUR);
    memcpy(g_image + PADDLE_PIX_CUR, g_image + SPRITE_BASE, 0x27 * 2);
    g_image[BALL_ALIVE] = 1;
    memcpy(g_image + BALLS + 4, g_image + 0x48fb, 8);
    img_setw(FRAME_DELAY_SET, 0x1f4);
    g_image[KEY_RIGHT] = g_image[KEY_LEFT] = 0;
    g_image[REPEAT_DIV] = 0;
    g_image[KEY_ACTION] = 0;
    g_image[SPEED_STEP] = g_image[SPEED_LIMIT] = 0xfa;
    if (g_image[DELAY_ENTRY] != 0xc3) {
        g_image[SPEED_STEP] = 3;
        g_image[SPEED_LIMIT] = 3;
    }
    img_setw(SPEED_TIMER, 0x4e20);
    g_image[ENTITY_REMOVE] = 0;
    g_image[0x33d5] = g_image[0x33d6] = g_image[0x3384] = 0;
    g_image[PADDLE_SUPPRESS] = 0;
    g_image[SHOT_ON] = g_image[CAUGHT] = 0;
    g_image[GAME_OVER] = g_image[EXTRA_ON] = g_image[LASER_ON] = 0;

    /* The serve: ball 0 on the paddle, the other two idle. */
    unsigned char *b = g_image + BALLS;
    b[0x00] = b[0x02] = 0x70;
    b[0x01] = b[0x03] = 0xb5;
    b[B_DY] = 1;
    b[B_DX] = 2;
    b[B_DIR_X] = 0;
    b[B_DIR_Y] = 1;
    b[B_ANCHOR_X] = b[0x00];
    b[B_ANCHOR_Y] = b[0x01];
    b[B_ACC_X] = b[B_ACC_Y] = 0;
    b[B_STATE] = 1;
    b[0x1d] = 0;
    b[0x1c + BALL_STRIDE] = 0;          /* +0x3a: ball 1 idle */
    b[0x1c + BALL_STRIDE * 2] = 0;      /* +0x58: ball 2 idle */
    ball_draw(BALLS + B_SPRITE, b[0x00], b[0x01]);

    io_flush_keys();

    /* Wait for the action key, or two thousand ticks, before serving. */
    if (img_w(INPUT_ACTIVE) != 0x1785) {
        img_setw(SERVE_TIMEOUT, 0x7d0);
        for (;;) {
            img_setw(SERVE_TIMEOUT, img_w(SERVE_TIMEOUT) - 1);
            if (img_w(SERVE_TIMEOUT) == 0)
                break;
            for (int i = 0; i < 0xf; i++)
                game_delay();
            game_input();
            if (g_image[KEY_ACTION] == 1)
                break;
            draw_paddle(SPRITE_BASE);
            io_present();
            if (!io_pump())
                return 1;
        }
    }

    for (;;) {                          /* one iteration is one frame */
        img_setw(FRAME_DELAY, img_w(FRAME_DELAY_SET));
        demo_input_step();

        if (g_image[BALL_ALIVE] == 0) {
            play_teardown();
            g_image[GAME_OVER] = 1;
            return 1;                   /* the original's `stc` */
        }
        if (g_image[LEVEL_CELLS] == 0) {
            play_teardown();
            return 0;                   /* `clc`: the bricks are gone */
        }

        game_input();
        if (g_image[PADDLE_SUPPRESS] == 0)
            draw_paddle(img_w(PADDLE_SPRITES + g_image[PADDLE_KIND] * 4));
        if (g_image[LASER_ON])
            laser_fire();

        /* Every 0x4e20 frames the ball is allowed to move one step more
         * often, up to the limit - the level speeds up the longer it runs. */
        img_setw(SPEED_TIMER, img_w(SPEED_TIMER) - 1);
        if (img_w(SPEED_TIMER) == 0) {
            img_setw(SPEED_TIMER, 0x61a8);
            if (g_image[SPEED_LIMIT] != 0xff)
                g_image[SPEED_LIMIT]++;
            g_image[SPEED_STEP] = g_image[SPEED_LIMIT];
        }

        if (--g_image[SPEED_STEP] != 0) {
            for (int i = 0; i < 3; i++) {
                unsigned ball = BALLS + i * BALL_STRIDE;
                unsigned char st = g_image[ball + B_STATE];
                if (st == 0 || st >= 3)
                    continue;
                if (g_image[CAUGHT] == 1 && !ball_on_paddle(ball))
                    continue;
                ball_step(ball);
                if (!ball_redraw(ball)) {
                    play_teardown();
                    g_image[GAME_OVER] = 1;
                    return 1;
                }
                if (g_image[LEVEL_CELLS] == 0) {
                    play_teardown();
                    return 0;
                }
                ball_after(ball);
            }
        } else {
            g_image[SPEED_STEP] = g_image[SPEED_LIMIT];
        }

        /* The entity list. [0x3142] trails one node behind so a handler that
         * asks to be removed can be unlinked without walking the list again. */
        img_setw(ENTITY_PREV, 0x3138);
        unsigned bx = img_w(ENTITY_HEAD);
        while (bx != 0xffff) {
            entity_call(bx);
            if (g_image[ENTITY_REMOVE] == 0) {
                img_setw(ENTITY_PREV, bx);
                bx = img_w(bx + E_NEXT);
            } else {
                unsigned next = img_w(bx + E_NEXT);
                entity_unlink(bx);
                bx = next;
            }
        }

        if (g_image[SHOT_ON]) {
            if (--g_image[SHOT_TIMER] == 0)
                erase_shot(SHOT_POS, 0xc8, SHOT_TIMER);
            img_setw(SHOT_LIFE, img_w(SHOT_LIFE) - 1);
            if (img_w(SHOT_LIFE) == 0) {
                g_image[SHOT_ON] = 0;
                entity_spawn(0x1554);
            }
        }
        if (g_image[EXTRA_ON]) {
            img_setw(EXTRA_TIMER, img_w(EXTRA_TIMER) - 1);
            if (img_w(EXTRA_TIMER) == 0)
                erase_shot(EXTRA_POS, 0, EXTRA_TIMER), img_setw(EXTRA_TIMER, 0x190);
            img_setw(SERVE_TIMEOUT, img_w(SERVE_TIMEOUT) - 1);
            if (img_w(SERVE_TIMEOUT) == 0)
                g_image[EXTRA_ON] = 0;
        }

        /* A pause that gets shorter as [0x33d6] rises: three passes of 0xb4
         * empty loops, minus one per point of it. */
        for (int i = 3 - g_image[0x33d6]; i > 0; i--)
            io_delay_cycles(0xb4 * CYCLES_PER_LOOP);

        if (g_image[EXTRA_ON] != 1 && g_image[0x33d5] != 3 &&
            game_random(io_ticks(), 0x86) == 0)
            bonus_spawn();

        sound_tick();
        io_delay_cycles(img_w(FRAME_DELAY) * CYCLES_PER_LOOP);
        game_delay();

        io_present();
        if (!io_pump())
            return 1;
    }
}

/* ========================================================================
 * 1ac2:02f5  play_session
 *
 * A whole game: pick the starting level, then loop over levels until the
 * lives run out. It has no exit of its own - the original leaves it the way
 * the mouse handler does at 1ac2:167e, `mov sp,[0x1405] / jmp 0x1d1`, which
 * throws away the stack and lands back in the menu. That is a longjmp, and it
 * is written as one here rather than pretended away, because the routines it
 * unwinds through really are abandoned mid-call.
 * ===================================================================== */
#define LIVES         0x13c9
#define LEVEL_SRC     0x13ca            /* offset of the level in the table */
#define PLAYER_NAME   0x13d5
#define SCORE_TEXT    0x13cd
#define LEVEL_TABLE   0x000c            /* within the 0xc46 block */
#define LEVEL_BYTES     0xb0
#define LEVEL_COUNT     0x32

jmp_buf g_back_to_menu;

void play_session(void)
{
    memcpy(g_image + PLAYER_NAME, g_image + 0x344f, 12);
    img_setw(SCORE_TEXT + 0, 0x3030);
    img_setw(SCORE_TEXT + 2, 0x3030);
    img_setw(SCORE_TEXT + 4, 0x3030);
    img_setw(SCORE_TEXT + 6, 0x3032);
    g_image[0x3f0a] = 0;
    g_image[0x3f09] = g_image[0x3f08];
    g_image[LIVES] = 5;

    /* A demo starts on a random level; a game always starts on the first. */
    unsigned lv = game_random(io_ticks(), 0x1e);
    if (img_w(INPUT_ACTIVE) != 0x1785)
        lv = 0;
    g_image[LEVEL_NUMBER] = (unsigned char)lv;
    img_setw(LEVEL_SRC, lv * LEVEL_BYTES + LEVEL_TABLE);
    panel_draw();

    for (;;) {
        level_colours();                        /* 1ac2:044b */
        memcpy(g_image + LEVEL_CELLS,
               g_image + SEG_C46 + img_w(LEVEL_SRC), LEVEL_BYTES);

        for (;;) {                              /* one level, retried on death */
            level_intro();                      /* 1ac2:1eb9 */
            for (;;) {
                int lost = play_loop();
                speaker_off();
                if (!lost)
                    goto level_done;
                life_lost();                    /* 1ac2:0735 */
                if (g_image[0x3f1b] != 1)
                    g_image[LIVES]--;
                if (g_image[GAME_OVER] == 1)
                    break;
            }
            screen_game_over();                 /* 1ac2:0473 */
            screen_end_of_game();               /* 1ac2:0d2e */
        }

    level_done:
        screen_level_done();                    /* 1ac2:0521 */
        img_setw(LEVEL_SRC, img_w(LEVEL_SRC) + LEVEL_BYTES);
        g_image[LEVEL_NUMBER]++;
        if (g_image[LEVEL_NUMBER] == LEVEL_COUNT) {
            g_image[LEVEL_NUMBER] = 0;
            img_setw(LEVEL_SRC, LEVEL_TABLE);
            screen_all_levels_done();           /* 1ac2:5940 */
        }
    }
}

/* ========================================================================
 * The playfield.
 * ===================================================================== */

/* The address of a pixel, the way the game computes it everywhere:
 *
 *     di = x >> 2                       four pixels to a byte
 *     if (y & 1) di += 0x2000           odd scan lines live in the far half
 *     di += (y >> 1) * 80               `shl ax,4` then `shl ax,2` twice more
 */
static unsigned cga_at(unsigned x, unsigned y)
{
    unsigned di = x >> 2;
    if (y & 1)
        di += CGA_PLANE;
    return di + (y >> 1) * CGA_STRIDE;
}

/* 1ac2:2034  draw_brick_row
 *
 * One scan line of the brick field. The playfield starts two bytes in - eight
 * pixels - and six scan lines down, and a brick is **16 pixels wide and eight
 * scan lines tall**: `(y - 6) >> 3` picks the row of cells and `((y - 6) & 7)
 * * 4` picks which of its eight lines to copy. Twelve cells across, four bytes
 * each, which is where the 12x14 grid in the level record comes from.
 *
 * Cell 0x0c is not a brick - it hands off to 0x41e5. Cells from 0x18 up have
 * their bitmaps in the block the program reaches as segment 0x14a1 rather than
 * in the table at 0x3080.
 */
#define BRICK_TOP        6              /* first scan line of the field */
#define BRICK_LEFT       2              /* bytes, so eight pixels */
#define BRICK_COLS      12
#define BRICK_HEIGHT     8              /* scan lines */
#define BRICK_BYTES      4              /* 16 pixels */
#define CELL_TABLE  0x3080              /* cell value -> bitmap pointer */
#define SEG_14A1   0x14a10

void draw_brick_row(unsigned y)
{
    unsigned di = cga_at(0, y) + BRICK_LEFT;
    unsigned row = (y - BRICK_TOP) & 0xff;
    unsigned sub = (row & 7) * 4;
    unsigned si = LEVEL_CELLS + 8 + (row >> 3) * BRICK_COLS;

    for (int c = 0; c < BRICK_COLS; c++, si++, di += BRICK_BYTES) {
        unsigned cell = g_image[si];
        if (cell == 0x0c) {
            cell_special(row & 0xff, di);
            continue;
        }
        unsigned idx = cell * 2;
        unsigned base = (idx >= 0x30 ? SEG_14A1 : 0) + img_w(CELL_TABLE + idx);
        const unsigned char *src = g_image + base + sub;
        for (int b = 0; b < BRICK_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = src[b];
    }
}

/* 1ac2:20b9  draw_sprite_20x6
 *
 * Six rows of five bytes at a pixel position - the popcorn kernels the level
 * intro sweeps down the screen.
 */
void draw_sprite_20x6(unsigned x, unsigned y, unsigned src)
{
    unsigned di = cga_at(x, y);
    for (int r = 0; r < 6; r++) {
        for (int b = 0; b < 5; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[src + r * 5 + b];
        di = cga_next_row(di);
    }
}

/* 1ac2:1eb9  level_intro, second phase (from 0x1f57)
 *
 * Paints the brick field one scan line at a time from the top, sweeping four
 * kernels along ahead of it. The first phase, which sweeps them the other way
 * to clear the previous level, is not transcribed yet.
 */
#define SWEEP_Y     0x2f0c              /* four kernel positions */
#define SWEEP_STATE 0x2efc              /* four records of four bytes */

void level_intro(void)
{
    g_image[SWEEP_Y + 3] = 0xc2;
    g_image[SWEEP_Y + 2] = 0xbd;
    g_image[SWEEP_Y + 1] = 0xb8;
    g_image[SWEEP_Y + 0] = 0xb3;

    /* The original runs this as two sweeps of four popcorn kernels: phase one
     * counts [0x2f0c] down from 0xb3 to 0x0c clearing the previous level,
     * phase two counts it back up painting the new one. The loop is not driven
     * by a counter of its own - [0x2f0c] *is* kernel zero's position, and it
     * advances only when that kernel's timer at [0x2efc] runs out, which is
     * what paces the reveal.
     *
     * Those timer records are set up by 0x2109, which is not transcribed yet,
     * so the reveal is driven directly here instead: the field is painted a
     * scan line at a time, in order, with no kernels. The picture that leaves
     * on screen is right; the way it arrives is not, and this is the first
     * thing to put back when 0x2109 lands.
     */
    /* Phase one, upwards: nothing but the backdrop, which is what wipes the
     * previous screen. The original paces it on kernel zero's timer; here it
     * is stepped directly, for the same reason as phase two below. */
    for (unsigned p = 0xb3; p != 0x0c; p--) {
        field_backdrop((p - 7) & 0xff);
        for (int i = 0; i < 0xa; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }

    for (unsigned p = 0x0c; p != 0xb3; p++) {
        unsigned y = (p - 6) & 0xff;
        draw_brick_row(y);
        field_backdrop((y + 1) & 0xff);
        for (int i = 0; i < 0xa; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }
    g_image[SWEEP_Y] = 0xb3;
}

/* ------------------------------------------------------------------------
 * 1ac2:2881  ball_draw
 *
 * Four rows of one word - 16 pixels by 4 - XORed in at a pixel position, so
 * drawing the same sprite twice erases it. Everything that moves in this game
 * is drawn this way.
 */
void ball_draw(unsigned sprite, unsigned x, unsigned y)
{
    unsigned di = cga_at(x, y);
    for (int r = 0; r < 4; r++) {
        g_vram[di & (CGA_SIZE - 1)] ^= g_image[sprite + r * 2];
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= g_image[sprite + r * 2 + 1];
        di = cga_next_row(di);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:2827  ball_redraw
 *
 * Move a ball on screen: erase it where it was, draw it where it is. The
 * structure carries both the sprite it last drew (+0x0c) and the one it is
 * about to (+0x04), because with XOR drawing you have to erase exactly the
 * bits you put down - a freshly shifted sprite would not cancel the old one.
 *
 * The shift is a `ror` of each row by `(x & 3) * 2` bits, one pixel per two,
 * done to a fresh copy taken from 0x48fb every time. `ror` and not `shr`: a
 * pixel leaving the right edge comes back in at the left, which is what lets
 * one 16-pixel word cover a ball straddling a byte boundary.
 *
 * It returns 1 always. The play loop tests the carry after calling this, but
 * the carry that comes out is left over from the `cmp di,0x2000` inside
 * ball_draw and means nothing - a ball is lost by its entity handler clearing
 * [0x2e73], not here.
 */
#define BALL_SPRITE_SRC 0x48fb

int ball_redraw(unsigned ball)
{
    unsigned char *b = g_image + ball;

    memcpy(b + B_PREV_SPR, b + B_SPRITE, 8);
    memcpy(b + B_SPRITE, g_image + BALL_SPRITE_SRC, 8);

    unsigned shift = (b[B_X] & 3) * 2;
    if (shift) {
        for (int r = 0; r < 4; r++) {
            unsigned w = b[B_SPRITE + r * 2] | (b[B_SPRITE + r * 2 + 1] << 8);
            w = ((w >> shift) | (w << (16 - shift))) & 0xffff;
            b[B_SPRITE + r * 2] = (unsigned char)w;
            b[B_SPRITE + r * 2 + 1] = (unsigned char)(w >> 8);
        }
    }

    ball_draw(ball + B_PREV_SPR, b[B_PREV_X], b[B_PREV_Y]);
    b[B_PREV_X] = b[B_X];
    b[B_PREV_Y] = b[B_Y];
    ball_draw(ball + B_SPRITE, b[B_X], b[B_Y]);
    return 1;
}

/* ------------------------------------------------------------------------
 * 1ac2:247f  ball_after
 *
 * What happens to a ball after it has moved: walls, bricks, paddle, floor.
 *
 * The playfield is x in (8, 0xc4) and y from 4 down to 0xc3; y == 0xc4 is the
 * floor. Every bounce sets the anchor at the point of contact and restarts the
 * Bresenham accumulators, which is the same thing the ball's entity handler
 * does and the reason `+0x18`/`+0x19` is where the last bounce was rather than
 * where the ball is.
 *
 * `+0x1d` counts bounces, and every 0x23 of them the slope is picked afresh
 * from `random(5) + 1` on each axis. That is what stops a ball settling into a
 * loop that never reaches the remaining bricks.
 *
 * [0x2e81] is the safety net - the extra floor a bonus can put up. With it
 * live the ball bounces off the bottom instead of being lost.
 */
#define B_BOUNCES   0x1d
#define WALL_LEFT   0x08
#define WALL_RIGHT  0xc4
#define WALL_TOP    0x04
#define FLOOR       0xc4
#define SOUND_BOUNCE   2
#define SAFETY_NET  0x2e81

void ball_after(unsigned ball)
{
    unsigned char *b = g_image + ball;

    if (b[B_BOUNCES] >= 0x23) {
        b[B_BOUNCES] = 0;
        b[B_ANCHOR_X] = b[B_X];
        b[B_ANCHOR_Y] = b[B_Y];
        b[B_ACC_X] = b[B_ACC_Y] = 0;
        b[B_DY] = (unsigned char)(game_random(io_ticks(), 5) + 1);
        b[B_DX] = (unsigned char)(game_random(io_ticks(), 5) + 1);
    }

    unsigned x = b[B_X], y = b[B_Y];
    if (x <= WALL_LEFT || x >= WALL_RIGHT) {
        g_image[SOUND_REQUEST] = SOUND_BOUNCE;
        b[B_DIR_X] = (x <= WALL_LEFT) ? 0 : 1;
        b[B_ACC_X] = 1;
        b[B_ACC_Y] = 0;
        b[B_ANCHOR_X] = (unsigned char)(x <= WALL_LEFT ? 9 : 0xc3);
        b[B_ANCHOR_Y] = (unsigned char)y;
        b[B_BOUNCES]++;
    }
    if (y <= WALL_TOP) {
        g_image[SOUND_REQUEST] = SOUND_BOUNCE;
        b[B_DIR_Y] = 0;                 /* downwards */
        b[B_BOUNCES]++;
        b[B_ACC_X] = 0;
        b[B_ACC_Y] = 1;
        b[B_ANCHOR_X] = (unsigned char)x;
        b[B_ANCHOR_Y] = (unsigned char)(y + 1);
    }

    ball_bricks(ball);                  /* 1ac2:254d */

    if (b[B_Y] != FLOOR) {
        ball_paddle(ball);              /* 1ac2:2316 */
        return;
    }
    if (g_image[SAFETY_NET] == 1) {     /* the net catches it */
        g_image[SOUND_REQUEST] = SOUND_BOUNCE;
        b[B_ANCHOR_X] = b[B_X];
        b[B_ANCHOR_Y] = 0xc3;
        b[B_DIR_Y] = 1;                 /* upwards */
        b[B_ACC_X] = 1;
        b[B_ACC_Y] = 1;
        return;
    }
    /* Lost. Erase it, mark it idle, and take one off the live count - the
     * play loop notices [0x2e73] reaching zero at the top of the next frame. */
    b[B_STATE] = 0;
    ball_draw(ball + B_SPRITE, b[B_X], b[B_Y]);
    g_image[BALL_ALIVE]--;
}

/* ------------------------------------------------------------------------
 * 1ac2:2316  ball_paddle
 *
 * The paddle is 0xb5 to 0xbe deep and [0x2d3a] wide. A ball reaching it comes
 * off at an angle that depends on where it hit, which is what makes the game
 * playable rather than a coin toss: the outgoing slope is looked up in a table
 * by distance from the near end.
 *
 *   0x2e2c   eleven slopes, indexed by how far in from either end the ball
 *            struck the top - shallow at the ends, steep in the middle
 *   0x2e42   ten slopes for a hit on the side, indexed by depth
 *
 * Each entry is a word stored (dy, dx), matching the ball's own `+0x16`,
 * `+0x17` pair. The middle of the paddle has no table entry at all: a ball
 * landing there keeps the slope it arrived with and only reverses.
 *
 * A ball moving upwards is treated as having hit the top even when it is
 * level with the side, which is what stops one that has just come off from
 * immediately catching the side on the way out.
 */
#define PADDLE_TOP    0xb5
#define PADDLE_BOTTOM 0xbe
#define PADDLE_WIDTH  0x2d3a
#define SLOPE_TOP     0x2e2c
#define SLOPE_SIDE    0x2e42
#define SOUND_PADDLE     1

/* The common tail of every top-of-paddle bounce: reverse vertically, anchor
 * one pixel clear of the paddle, and restart the accumulators. */
static void paddle_bounce_up(unsigned char *b)
{
    unsigned ah = b[B_Y];
    if (b[B_Y] == PADDLE_BOTTOM) {
        b[B_DIR_Y] = 0;                 /* it came from below: send it down */
        ah++;
    } else {
        b[B_DIR_Y] = 1;
        ah--;
    }
    b[B_ANCHOR_X] = b[B_X];
    b[B_ANCHOR_Y] = (unsigned char)ah;
    b[B_ACC_X] = b[B_ACC_Y] = 1;
    b[B_BOUNCES] = 0;
    g_image[SOUND_REQUEST] = SOUND_PADDLE;
}

static void paddle_slope(unsigned char *b, unsigned table, unsigned index)
{
    unsigned w = img_w(table + index * 2);
    b[B_DY] = (unsigned char)w;
    b[B_DX] = (unsigned char)(w >> 8);
}

void ball_paddle(unsigned ball)
{
    unsigned char *b = g_image + ball;
    unsigned y = b[B_Y];

    if (y < PADDLE_TOP || y > PADDLE_BOTTOM)
        return;

    if (y > PADDLE_TOP && b[B_DIR_Y] != 1) {
        /* The sides. Only the two single columns just outside the paddle
         * count, which is why this is an equality and not a range. */
        unsigned left = (g_image[PADDLE_X] - 3) & 0xff;
        unsigned bx = b[B_X];
        int from_left = 1;
        if (bx != left) {
            unsigned off = (bx - left) & 0xff;
            if (off != ((g_image[PADDLE_WIDTH] + 3) & 0xff))
                return;
            from_left = 0;
        }
        unsigned depth = (y - 0xb6) & 0xff;
        b[B_DIR_Y] = (depth <= 5) ? 1 : 0;
        b[B_DIR_X] = (unsigned char)from_left;
        b[B_ANCHOR_X] = from_left
            ? (unsigned char)(g_image[PADDLE_X] - 4)
            : (unsigned char)(g_image[PADDLE_X] + g_image[PADDLE_WIDTH] + 1);
        b[B_ANCHOR_Y] = (unsigned char)y;
        b[B_ACC_X] = b[B_ACC_Y] = 1;
        b[B_BOUNCES] = 0;
        paddle_slope(b, SLOPE_SIDE, depth);
        g_image[SOUND_REQUEST] = SOUND_PADDLE;
        return;
    }

    /* The top. */
    unsigned left = (g_image[PADDLE_X] - 3) & 0xff;
    if (b[B_X] < left)
        return;
    unsigned off = (b[B_X] - left) & 0xff;

    if (off <= 0x0a) {                          /* the left end */
        paddle_slope(b, SLOPE_TOP, off);
        b[B_DIR_X] = 1;                         /* away to the left */
        paddle_bounce_up(b);
        return;
    }
    unsigned span = (g_image[PADDLE_WIDTH] + 3) & 0xff;
    if (off > span)
        return;
    unsigned from_right = (span - off) & 0xff;
    if (from_right <= 0x0a) {                   /* the right end */
        paddle_slope(b, SLOPE_TOP, from_right);
        b[B_DIR_X] = 0;                         /* away to the right */
        paddle_bounce_up(b);
        return;
    }
    /* The middle: no table, so the slope it arrived with is kept. */
    paddle_bounce_up(b);
}

/* ------------------------------------------------------------------------
 * 1ac2:2755  probe_cell
 *
 * Is there a brick at this pixel? The arithmetic is the clearest statement in
 * the program of what a level record means:
 *
 *     di  = (y & 0xf8) + ((y & 0xf8) >> 1)      the row: (y >> 3) * 12
 *     di += x >> 4                              the column: 16 pixels a brick
 *     di += 0x2f18                              the cells
 *
 * so the grid is twelve wide, a brick is sixteen pixels by eight, and the
 * cells start eight bytes into the record. Cell 0 is empty and cell 0x0c is
 * not a brick either.
 *
 * A hit records the cell's address in the slot and the brick's centre after
 * it, and counts itself in [0x2e74].
 */
#define HIT_COUNT  0x2e74
#define HIT_SLOTS  0x2e89               /* four of four bytes */
#define HIT_DIRS   0x2e99               /* the direction to leave in, per slot */

static void probe_cell(unsigned x, unsigned y, unsigned slot)
{
    if (x > 0xbf || y > 0xc4) {
        img_setw(slot, 0);
        return;
    }
    unsigned row = y & 0xf8;
    unsigned di = row + (row >> 1) + (x >> 4) + LEVEL_CELLS + 8;
    unsigned cell = g_image[di];
    if (cell == 0x0c || cell == 0) {
        img_setw(slot, 0);
        return;
    }
    img_setw(slot, di);
    g_image[HIT_COUNT]++;
    g_image[slot + 2] = (unsigned char)((x & 0xf0) + 8);   /* the brick's */
    g_image[slot + 3] = (unsigned char)((y & 0xf8) + 6);   /* centre */
}

/* 1ac2:27b7  drop_duplicate_hits
 *
 * Two corners of the ball can land in the same brick. Later slots naming a
 * centre an earlier one already has are cleared, so the brick is only hit
 * once.
 */
static void drop_duplicate_hits(void)
{
    for (int i = 0; i < 3; i++) {
        unsigned si = HIT_SLOTS + i * 4;
        if (!img_w(si))
            continue;
        unsigned centre = img_w(si + 2);
        for (int j = i + 1; j < 4; j++) {
            unsigned di = HIT_SLOTS + j * 4;
            if (img_w(di) && img_w(di + 2) == centre)
                img_setw(di, 0);
        }
    }
}

/* Reverse one axis, anchoring one pixel back the way the ball came. */
static void bounce_x(unsigned char *b)
{
    if (b[B_DIR_X] == 0) {
        b[B_DIR_X] = 1;
        b[B_ANCHOR_X] = (unsigned char)(b[B_X] - 1);
    } else {
        b[B_DIR_X] = 0;
        b[B_ANCHOR_X] = (unsigned char)(b[B_X] + 1);
    }
}

static void bounce_y(unsigned char *b)
{
    if (b[B_DIR_Y] == 0) {
        b[B_DIR_Y] = 1;
        b[B_ANCHOR_Y] = (unsigned char)(b[B_Y] - 1);
    } else {
        b[B_DIR_Y] = 0;
        b[B_ANCHOR_Y] = (unsigned char)(b[B_Y] + 1);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:254d  ball_bricks
 *
 * Probe the ball's four corners against the brick grid and work out which way
 * it should leave. One corner in a brick means a corner hit and the direction
 * comes straight out of the table at 0x2e99; two adjacent corners mean a flat
 * face and only that axis reverses; three or four mean it is wedged and both
 * reverse.
 *
 * The corners are a 4x4 box offset by (-8, -6) from the ball's position -
 * the ball sprite is 16 by 4 but only its middle counts for collision, which
 * is why it can graze a brick without taking it.
 */
void ball_bricks(unsigned ball)
{
    unsigned char *b = g_image + ball;
    g_image[HIT_COUNT] = 0;

    unsigned x = (b[B_X] - 8) & 0xff, y = (b[B_Y] - 6) & 0xff;
    probe_cell(x, y, HIT_SLOTS + 0);
    probe_cell((x + 3) & 0xff, y, HIT_SLOTS + 4);
    probe_cell((x + 3) & 0xff, (y + 3) & 0xff, HIT_SLOTS + 8);
    probe_cell(x, (y + 3) & 0xff, HIT_SLOTS + 12);

    unsigned n = g_image[HIT_COUNT];
    if (n == 0)
        return;

    unsigned s0 = img_w(HIT_SLOTS + 0), s1 = img_w(HIT_SLOTS + 4);
    unsigned s2 = img_w(HIT_SLOTS + 8), s3 = img_w(HIT_SLOTS + 12);

    if (n == 3 || (n == 2 && ((s0 && s2) || (!s0 && s1 && s3)))) {
        bounce_x(b);                    /* wedged, or hit on the diagonal */
        bounce_y(b);
    } else if (n == 2) {
        /* A flat face: the pair tells which axis it was. */
        if (s0 && s1)
            bounce_y(b), b[B_ANCHOR_X] = b[B_X];
        else if (s2 && s3)
            bounce_y(b), b[B_ANCHOR_X] = b[B_X];
        else
            bounce_x(b), b[B_ANCHOR_Y] = b[B_Y];
    } else {
        /* One corner, or all four: leave in the direction its slot names. */
        int i = 0;
        while (i < 4 && !img_w(HIT_SLOTS + i * 4))
            i++;
        if (i < 4) {
            unsigned d = img_w(HIT_DIRS + i * 2);
            b[B_DIR_X] = (unsigned char)(d & 0xff);
            b[B_DIR_Y] = (unsigned char)(d >> 8);
            b[B_ANCHOR_Y] = (unsigned char)(b[B_Y] + (b[B_DIR_Y] ? -1 : 1));
            b[B_ANCHOR_X] = (unsigned char)(b[B_X] + (b[B_DIR_X] ? -1 : 1));
        }
    }

    /* Three hits means one of them is a corner that should not count. */
    if (n == 3) {
        if (s0) {
            if (s2) {
                img_setw(HIT_SLOTS + 4, 0);
                img_setw(HIT_SLOTS + 12, 0);
            } else {
                img_setw(HIT_SLOTS + 0, 0);
            }
        } else {
            img_setw(HIT_SLOTS + 8, 0);
        }
    }

    b[B_ACC_X] = b[B_ACC_Y] = 1;
    drop_duplicate_hits();

    for (int i = 0; i < 4; i++) {
        unsigned cell = img_w(HIT_SLOTS + i * 4);
        if (cell)
            brick_hit(HIT_SLOTS + i * 4, cell, ball);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:28cb and the table at 0x3044
 *
 * Each cell value has its own handler; the table maps value to routine, and a
 * value of 0 or above 12 has none. Only the ordinary brick - value 1, routine
 * 0x28cb - is transcribed, and every other type falls back to it, so the
 * unusual bricks currently behave as ordinary ones rather than not at all.
 *
 * The real one has two paths and both clear the cell and take one off the
 * count: usually the brick vanishes at once, but one time in three (while
 * [0x3384] is under 3) it hands itself to an entity running 0x3b2a, which is
 * the crumbling animation.
 */
#define SCORE_ADD_LO  0x1415
#define SCORE_ADD_MID 0x1417
#define SCORE_ADD_HI  0x1419
#define SOUND_BRICK      3

void brick_hit(unsigned slot, unsigned cell, unsigned ball)
{
    img_setw(SCORE_ADD_LO, 0);
    img_setw(SCORE_ADD_MID, 0);
    img_setw(SCORE_ADD_HI, 2);
    score_add();                        /* 1ac2:413d */
    g_image[SOUND_REQUEST] = SOUND_BRICK;
    if (ball)
        g_image[ball + B_BOUNCES] = 0;

    /* The immediate-removal path at 0x2929: clear the cell, take one off the
     * count, and XOR the brick's own picture off the screen. The other path
     * hands it to an entity running 0x3b2a that crumbles it over several
     * frames; that entity is not transcribed, so every brick goes at once. */
    g_image[LEVEL_CELLS]--;
    g_image[cell] = 0;
    xor_sprite_16x7(g_image[slot + 2], g_image[slot + 3],
                    img_w(CELL_TABLE + 1 * 2));
}

/* ------------------------------------------------------------------------
 * 1ac2:3b64  xor_sprite_16x7
 *
 * Seven rows of four bytes, XORed in. A brick's cell is eight scan lines
 * apart but only seven of them are drawn - the eighth is the gap between
 * rows - so this both draws a brick and, run again, rubs it out.
 */
void xor_sprite_16x7(unsigned x, unsigned y, unsigned src)
{
    unsigned di = cga_at(x, y);
    for (int r = 0; r < 7; r++) {
        for (int b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= g_image[src + r * 4 + b];
        di = cga_next_row(di);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:413d  score_add
 *
 * Add the six-digit figure at [0x1415] to the score at [0x13cd], in decimal,
 * and redraw it. Both are held as ASCII digits, so the addition masks off the
 * 0x30 before working and puts it back after.
 *
 * The carry is the classic decimal adjust: add six to the sum and see whether
 * it carries out of the low nibble. If it does, the digit really was ten or
 * more and the adjusted value is the right one; if not, the unadjusted value
 * is kept. The original keeps the carry in `bl` rather than in the flags,
 * because the masking in between would destroy it.
 */
void score_add(void)
{
    unsigned si = 0x13d2, di = 0x141a;
    unsigned carry = 0;
    for (int i = 0; i < 6; i++, si--, di--) {
        unsigned sum = (g_image[si] & 0x0f) + g_image[di] + (carry ? 1 : 0);
        unsigned adjusted = (sum + 6) & 0xff;
        carry = (adjusted & 0xf0) || (sum & 0xf0);
        g_image[si] = (unsigned char)(0x30 | ((carry ? adjusted : sum) & 0x0f));
    }
    /* Redraw the six digits into the panel. */
    si = 0x13cd;
    di = 0x15d2;
    for (int i = 0; i < 6; i++, si++, di += 2)
        draw_char(g_image[si], di);
}

/* ------------------------------------------------------------------------
 * 1ac2:1fc1  field_backdrop
 *
 * One band of the playfield's background: 48 bytes - the full 192-pixel width
 * - by eight scan lines, at `y`. The pattern comes from a table of eight at
 * 0x6d95 chosen by `[0x2efb] >> 3`, and that counter advances every call and
 * wraps at 0x27, so the backdrop drifts while a level is being revealed.
 *
 * This is what clears the previous screen. The level sweep calls it one line
 * ahead of the brick row it is drawing, so the backdrop arrives just before
 * the bricks land on it - which is why skipping it left the menu showing
 * through the playfield.
 */
#define BACKDROP_TABLE 0x6d95
#define BACKDROP_PHASE 0x2efb
#define BACKDROP_BYTES     48

void field_backdrop(unsigned y)
{
    unsigned di = cga_at(0, y) + BRICK_LEFT;
    unsigned si = img_w(BACKDROP_TABLE + ((g_image[BACKDROP_PHASE] >> 3) & 7) * 2);
    for (int r = 0; r < 8; r++) {
        for (int b = 0; b < BACKDROP_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[si + b];
        si += BACKDROP_BYTES;
        di = cga_next_row(di);
    }
    /* `shr al,1` three times looking for a set bit, then `cmp al,4`: the
     * counter resets only when its low three bits are clear and it has
     * reached the last of the eight patterns. */
    unsigned p = g_image[BACKDROP_PHASE];
    if ((p & 7) == 0 && (p >> 3) == 4)
        p = 0xff;
    g_image[BACKDROP_PHASE] = (unsigned char)(p + 1);
}
