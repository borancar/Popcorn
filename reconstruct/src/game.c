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
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

uint8_t *g_image;
/* Empty, and it stays empty: POPCORN.EXE, the .PPC sets and popcorn.hsc are
 * all read and written relative to the **current directory**, which is what
 * the original does - in DOS the game and its files were the current
 * directory. Copy popcorn.exe and the .ppc files in beside the binary and run
 * it from there.
 *
 * It mattered that this is one value. load_high_scores was given the
 * directory POPCORN.EXE was found in and hsc_save was given g_dir, which
 * nothing assigned - so the port read its scores from one file and wrote them
 * to another, and no score ever came back. */
const char *g_dir = "";

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
void ball_step(ball_t *b)
{
    uint32_t dy = b->dy, dx = b->dx;
    uint32_t off_x, off_y;

    if (dx >= dy) {                     /* x is the major axis */
        off_x = b->acc_x;
        off_y = dy ? (uint32_t)(off_x * dy) / dx : 0;
        b->acc_x++;
    } else {                            /* y is the major axis */
        off_y = b->acc_y;
        off_x = dx ? (uint32_t)(off_y * dx) / dy : 0;
        b->acc_y++;
    }
    /* The direction flags negate each axis independently. The original does
     * this in 8-bit registers and lets the add wrap, which is what keeps a
     * ball at the left wall from running off into high coordinates. */
    int32_t sx = b->dir_x ? -(int32_t)off_x : (int32_t)off_x;
    int32_t sy = b->dir_y ? -(int32_t)off_y : (int32_t)off_y;
    b->x = (uint8_t)(b->anchor_x + sx);
    b->y = (uint8_t)(b->anchor_y + sy);
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
#define REPEAT_RESET  5

void input_keyboard(void)
{
    if (--gv.repeat_count != 0)
        return;
    if (gv.repeat_div != 1)
        gv.repeat_div--;
    gv.repeat_count = gv.repeat_div;

    int32_t go_left = gv.key_left != 0;
    if (gv.key_left == gv.key_right) {
        if (!gv.key_left) {              /* neither key held */
            gv.repeat_count = REPEAT_RESET;
            gv.repeat_div = REPEAT_RESET;
            if (gv.paddle_x > gv.paddle_max)
                gv.paddle_x = gv.paddle_max;
            return;
        }
        go_left = gv.last_dir == 0;      /* both: the most recent wins */
    }

    /* Both limits are compared unsigned, after an 8-bit inc or dec that is
     * allowed to wrap. At x = 0 the decrement gives 0xff, which is `jae` the
     * minimum and so is kept - the original has no guard against that, and it
     * never comes up because x never gets below the minimum in the first
     * place. Transcribed as it is rather than as it should be. */
    if (!go_left) {
        uint8_t x = (uint8_t)(gv.paddle_x + 1);
        if (x > gv.paddle_max)
            x = gv.paddle_max;
        gv.paddle_x = x;
    } else {
        uint8_t x = (uint8_t)(gv.paddle_x - 1);
        if (x < gv.paddle_min)
            x = gv.paddle_min;
        gv.paddle_x = x;
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
void input_mouse(uint32_t mouse_x, uint32_t buttons)
{
    gv.key_action = (buttons & 3) ? 1 : 0;

    uint8_t x = (uint8_t)((mouse_x >> 1) & 0xff);
    if (x > gv.paddle_max)
        x = gv.paddle_max;
    else if (x < gv.paddle_min)
        x = gv.paddle_min;
    gv.paddle_x = x;
}

/* ------------------------------------------------------------------------
 * 1ac2:5099 / 1ac2:50bc  save_screen / restore_screen
 *
 * Both halves of the CGA aperture to and from a 32,000-byte buffer at image
 * 0x10250 (the original reaches it as 0xc46:0x3df0, one of the 35 relocated
 * segment constants).  Used around anything that draws over the menu.
 */
/* Two `rep movsw` of 0xfa0 words each, from 0xb800:0000 and 0xb800:2000. That
 * is 8,000 bytes a half - the 200 visible scan lines at 80 bytes for every
 * other one - not the 8,192 each half of the aperture spans. The 192 bytes of
 * padding at the end of each half are neither saved nor restored, and the two
 * halves land adjacent in the buffer rather than 0x2000 apart, so a save is
 * 16,000 bytes and not a copy of the aperture. */
#define SCREEN_HALF  8000

void save_screen(void)
{
    memcpy(gv.screen_save[0], g_vram, SCREEN_HALF);
    memcpy(gv.screen_save[1], g_vram + CGA_PLANE, SCREEN_HALF);
}

void restore_screen(void)
{
    memcpy(g_vram, gv.screen_save[0], SCREEN_HALF);
    memcpy(g_vram + CGA_PLANE, gv.screen_save[1], SCREEN_HALF);
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

void paddle_row_offsets(uint32_t x, paddle_rows_t *rows)
{
    uint32_t off = (x >> 2) + PADDLE_ROW_BASE;
    for (int32_t r = 0; r < PADDLE_ROWS; r++) {
        rows->at[r] = (uint16_t)off;
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
void blit_xor(const uint8_t *pixels, const paddle_rows_t *rows)
{
    for (int32_t r = 0; r < PADDLE_ROWS; r++) {
        uint32_t di = rows->at[r];
        const uint8_t *row = pixels + r * PADDLE_BYTES;
        for (int32_t b = 0; b < PADDLE_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= row[b];
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

void draw_paddle(const uint8_t *sprite)
{
    if (!gv.paddle_morphing &&
        gv.paddle_x == gv.paddle_prev_x)
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
    gv.frame_delay -= 0x1e0;            /* the uint16_t is the `& 0xffff` */

    /* What is on screen now becomes what has to be erased. */
    gv.paddle_rows[1] = gv.paddle_rows[0];
    memcpy(gv.paddle_pix[1], gv.paddle_pix[0],
           PADDLE_IMAGE + 1);

    uint32_t x = gv.paddle_x;
    gv.paddle_prev_x = (uint8_t)x;
    paddle_row_offsets(x, &gv.paddle_rows[0]);

    /* Pick the copy pre-shifted to this pixel within its byte. */
    memcpy(gv.paddle_pix[0],
           sprite + (x & 3) * PADDLE_IMAGE, PADDLE_IMAGE + 1);

    blit_xor(gv.paddle_pix[1], &gv.paddle_rows[1]);   /* erase where it was */
    blit_xor(gv.paddle_pix[0], &gv.paddle_rows[0]);     /* draw where it is */
}

/* ------------------------------------------------------------------------
 * 1ac2:0c64  draw_char
 *
 * One glyph of the score-panel font at `di`, an offset into the framebuffer.
 * gv.font holds 24 bytes per glyph: twelve rows of one word,
 * which at two bits per pixel is an 8x12 cell.
 *
 * The character-to-glyph map is the original's, verbatim - it has no glyphs
 * for lower case and none for punctuation beyond a dash and a colon, so
 * anything else lands on the space, which is a solid block of colour 2 rather
 * than blank. That is deliberate: it is how the red bars behind the headings
 * are painted.
 */
#define FONT_ROWS      12
#define FONT_GLYPH     24

static uint32_t glyph_of(char c)
{
    /* The cursor is 0xff, which is not a character and is negative in a
     * signed char, so the comparisons are done on the byte. */
    uint8_t u = (uint8_t)c;
    if (u == ':')  return 0x26;
    if (u == 0xff) return 0x27;         /* the text-entry cursor */
    if (u == '-')  return 0x0b;
    if (u >= '0' && u <= '9') return u - 0x2f;
    if (u >= 'A' && u <= 'Z') return u - 0x35;
    return 0;                           /* space, and everything unmapped */
}

void draw_char(char c, uint32_t di)
{
    const uint8_t (*g)[2] = gv.font[glyph_of(c)];
    for (int32_t r = 0; r < FONT_ROWS; r++) {
        g_vram[di & (CGA_SIZE - 1)] = g[r][0];
        g_vram[(di + 1) & (CGA_SIZE - 1)] = g[r][1];
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

uint32_t game_random(uint32_t ticks, uint32_t limit)
{
    io_log_random(limit);               /* for sidebyside.py, no-op otherwise */
    uint32_t ax = ticks & 0xffff;
    /* Ten words out of the **entity pool**, from entity 2's variant on:
     * 0x3164 is entities[2] plus two, so what gets folded in is whatever the
     * entities happen to be holding this frame. */
    const uint8_t *stir = (const uint8_t *)&gv.entities[2].p;
    for (int32_t i = 0; i < 10; i++)
        ax = (ax + stir[i * 2] + (stir[i * 2 + 1] << 8)) & 0xffff;
    ax = (ax + gv.rng_state) & 0xffff;
    gv.rng_state = (uint16_t)((gv.rng_state + 0x5ec5) & 0xffff);

    /* `add al,ah` then `xor ah,ah` then `div dl`: folded to eight bits before
     * the divide, so the result really is only ever 0..255 wide. */
    uint32_t al = ((ax & 0xff) + (ax >> 8)) & 0xff;
    return limit ? al % limit : 0;
}

/* ========================================================================
 * Startup, timing and sound.
 * ===================================================================== */

/* Data the program keeps in its own code segment, reached as `cs:[...]`.
 * There are only a handful, and they are all here: the sound state and the
 * two bytes POPSPEED patches. */
#define CS_BASE        0x1ac20

static int32_t g_speaker_on;

/* 1ac2:0085 and 1ac2:0090  speaker_on / speaker_off
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
    uint8_t req = cv.sound_request;

    if (req) {
        cv.sound_request = 0;
        if (!cv.sound_on)
            return;
        cv.sound_ptr = cv.sound_tunes[req - 1];
        speaker_on();
        /* fall through and play the first note straight away */
    } else if (--cv.sound_timer != 0) {
        return;                         /* still holding the current note */
    }

    /* The tune pointers are offsets **into the code segment** - 0xa, 0x14,
     * 0x1e and so on - because that is where the note data sits and DS is set
     * to the code segment at the top of the routine. Reading them as plain
     * image offsets lands in the sprite data and plays whatever is there. */
    uint32_t si = cv.sound_ptr;
    uint32_t note = img_w(CS_BASE + si);
    if (note == 0) {                    /* end of tune */
        speaker_off();
        return;
    }
    cv.sound_ptr = (uint16_t)(si + 2);
    cv.sound_timer = (uint8_t)(note >> 8);
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
    if (cv.delay_entry == 0xc3)   /* patched to a bare `ret` */
        return;
    io_delay_cycles(cv.delay_count * CYCLES_PER_LOOP);
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
void read_speed_setting(uint32_t speed)
{
    if (speed == 1) {
        cv.delay_entry = 0xc3;
        cv.delay_count = (uint16_t)( 0);
        return;
    }
    if (speed == 0)
        speed = 0x6f;
    cv.delay_count = (uint16_t)( speed - 1);
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
void build_shifted_sprites(void)
{
    for (int32_t set = 0; set < 4; set++) {
        for (int32_t phase = 1; phase < 4; phase++) {
            uint8_t *out = gv.paddle_sprites[set][phase];
            memcpy(out, gv.paddle_sprites[set][phase - 1], PADDLE_IMAGE);
            for (int32_t row = 0; row < PADDLE_ROWS; row++) {
                uint8_t *r = out + row * PADDLE_BYTES;
                /* One pixel is two bits, so the shift is done twice. */
                for (int32_t twice = 0; twice < 2; twice++) {
                    uint32_t carry = 0;
                    for (int32_t b = 0; b < PADDLE_BYTES; b++) {
                        uint32_t v = r[b];
                        r[b] = (uint8_t)((v >> 1) | (carry << 7));
                        carry = v & 1;
                    }
                }
            }
        }
    }
}

/* 1ac2:4d96  load_high_scores
 *
 * Opens gv.hsc_file - "popcorn.hsc", with the drive letter the
 * program is running from patched into [0x141b] - and reads 0xb4 bytes into
 * gv.hsc. A missing file is not an error: the table keeps whatever the
 * image shipped with, which is a full set of default entries.
 */
#define HSC_LEN     0xb4                /* ten entries; the eleventh is not saved */

void load_high_scores(const char *dir)
{
    char path[512];
    const char *name = gv.hsc_file;
    snprintf(path, sizeof path, "%s%s", dir ? dir : "", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fread(gv.hsc, 1, HSC_LEN, f);
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
#define CURTAIN_ROW      0x1b           /* 27 bytes: 108 pixels */

void intro_curtain(void)
{
    uint32_t ah = 0, dl = 0xff, di0 = 0x213f;

    for (int32_t bx = 0x1a; bx > 0; bx--, di0--) {
        ah = 0;
        for (int32_t dh = 4; dh > 0; dh--) {
            for (int32_t i = 0; i < 0x32; i++)
                game_delay();
            ah = ((ah << 2) | 3) & 0xff;        /* two `stc; rcl ah,1` */
            uint32_t al = 0x55 & ah;

            io_wait_retrace();
            uint32_t di = di0;
            for (int32_t cx = 0x31; cx > 0; cx--, di += 0x50)
                g_vram[di & (CGA_SIZE - 1)] = (uint8_t)al;
            di = (di0 - 0x1fb0) & 0xffff;
            for (int32_t cx = 0x31; cx > 0; cx--, di += 0x50)
                g_vram[di & (CGA_SIZE - 1)] = (uint8_t)al;

            if (bx == 1) {
                /* The last column is drawn a second time from `dl`, which is
                 * rotated with carry *clear*, so it empties as `ah` fills. */
                dl = (dl << 2) & 0xff;
                al = 0x55 & dl;
                di = 0x213f;
                for (int32_t cx = 0x31; cx > 0; cx--, di += 0x50)
                    g_vram[di & (CGA_SIZE - 1)] = (uint8_t)al;
                di = (0x213f - 0x1fb0) & 0xffff;
                for (int32_t cx = 0x31; cx > 0; cx--, di += 0x50)
                    g_vram[di & (CGA_SIZE - 1)] = (uint8_t)al;
            }
        }
    }
    for (int32_t i = 0; i < 0x32; i++)
        game_delay();

    for (uint32_t rows = 1; rows != 0x6a; rows++) {
        uint32_t n = (CURTAIN_ROW * (rows & 0xff)) & 0xffff;
        memcpy(gv.scratch2.curtain_work, gv.curtain_image[105 - rows], n);

        for (uint32_t i = 0; i < n && i < 0xbd; i++) {
            uint32_t al = gv.scratch2.curtain_work[i], out = 0;
            for (int32_t k = 0; k < 4; k++) {
                uint32_t hi = (al >> 7) & 1;
                al = (al << 1) & 0xff;
                uint32_t lo = (al >> 7) & 1;
                al = (al << 1) & 0xff;
                out = hi ? (((out << 1) | 1) << 1 | lo) & 0xff
                         : (out << 2) & 0xff;
            }
            gv.scratch2.curtain_work[i] = (uint8_t)out;
        }

        const uint8_t *row = gv.scratch2.curtain_work;
        uint32_t di = 0x34;
        io_wait_retrace();
        for (uint32_t r = 0; r < rows; r++) {
            for (int32_t b = 0; b < CURTAIN_ROW; b++)
                g_vram[(di + b) & (CGA_SIZE - 1)] = row[b];
            row += CURTAIN_ROW;
            di = cga_next_row(di);
        }
        for (int32_t i = 0; i < 0x28; i++)
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
static void logo_pass(const uint8_t *src, uint32_t dest, int32_t rows,
                      int32_t erase, int32_t back)
{
    const uint8_t *si = src;
    uint32_t di = dest;
    for (int32_t n = rows; n > 0; n--) {
        uint32_t bx = di;
        for (int32_t i = 0; i < 12; i++) {          /* 12 words */
            /* `movsw` copies the word **at** si and si+1 and *then* steps
             * both pointers - by +2 with the direction flag clear, by -2 with
             * it set. Writing at si-1 and si-2 for the backward case instead
             * shifts the whole picture by two bytes, which drew the
             * background and none of the lettering. */
            g_vram[di & (CGA_SIZE - 1)] = si[0];
            g_vram[(di + 1) & (CGA_SIZE - 1)] = si[1];
            si = back ? si - 2 : si + 2;
            di = back ? di - 2 : di + 2;
        }
        di = back ? cga_prev_row_ja(bx) : cga_next_row_ja(bx);
        bx = di;

        /* The white bar that trails the slice. Backwards it starts four bytes
         * behind the row and walks down; forwards it starts at the row and
         * walks up. Twenty bytes either way. */
        uint32_t w = back ? di - 4 : di;
        for (int32_t i = 0; i < 10; i++) {
            uint32_t a = w & (CGA_SIZE - 1), b = (w + 1) & (CGA_SIZE - 1);
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
        for (int32_t i = 0; i < 0x32; i++)
            game_delay();
    }
}

void intro_logo(void)
{
    /* Two passes down with the direction flag set, then two up with it clear.
     * Each pair draws the slice then rubs the bar out again, so what is left
     * on screen is the picture and not the bar. */
    uint8_t *top = &c46.logo[sizeof c46.logo - 2];      /* the last word */
    logo_pass(top, 0x3f3f, 0x5b, 0, 1);
    logo_pass(top, 0x3f3f, 0x5a, 1, 1);
    logo_pass(c46.logo, 0x3119, 0x5b, 0, 0);
    logo_pass(c46.logo, 0x1119, 0x5c, 1, 0);
}

/* 1ac2:55e5  intro_reveal
 *
 * Two halves. The first paints a dithered wipe down 0x34 columns in four
 * phases; the second reveals the picture band by band, widening the slice
 * copied from one byte to 0x34 and waiting for retrace on each step.
 */
void intro_reveal(void)
{
    uint32_t bx0 = 0x230;
    for (int32_t dl = 0x34; dl > 0; dl--, bx0++) {
        uint32_t al = 0xc0;
        uint32_t di0 = bx0;
        for (int32_t dh = 4; dh > 0; dh--) {
            al &= 0x55;
            uint32_t di = di0;
            for (int32_t cl = 7; cl > 0; cl--, di += 0x370)
                g_vram[di & (CGA_SIZE - 1)] = (uint8_t)al;
            for (int32_t i = 0; i < 0x19; i++)
                game_delay();
            al = ((al >> 1) | 0x80) & 0xff;     /* stc; rcr al,1 */
            al = ((al >> 1) | 0x80) & 0xff;
        }
    }

    uint32_t bp = 0xa0;
    for (int32_t band = 0; band < 7; band++, bp += 0x370) {
        for (uint32_t bx = 1; bx < 0x35; bx++) {
            /* The slice is anchored 0x33 into the band and widens to the
             * left, a byte a pass, which is why the original holds that far
             * edge rather than the band's start. */
            const uint8_t *si = &c46.reveal[band][0x33 - (bx - 1)];
            uint32_t di = bp;
            io_wait_retrace();
            for (int32_t dl = 0x15; dl > 0; dl--) {
                for (uint32_t i = 0; i < bx; i++)
                    g_vram[(di + i) & (CGA_SIZE - 1)] = si[i];
                si += 0x34;
                di = cga_next_row(di);
            }
            for (int32_t i = 0; i < 0x0a; i++)
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
    const uint8_t *feed = c46.scroll_rows[0];
    for (int32_t bl = 0x1a; bl > 0; bl--) {
        uint32_t di = 0x1b33;
        io_wait_retrace();
        for (int32_t bh = 0x19; bh > 0; bh--) {
            uint32_t src = cga_next_row(di);
            for (int32_t i = 0; i < 0x31; i++)
                g_vram[(di + i) & (CGA_SIZE - 1)] =
                    g_vram[(src + i) & (CGA_SIZE - 1)];
            di = src;
        }
        for (int32_t i = 0; i < 0x31; i++)
            g_vram[(0x3ef3 + i) & (CGA_SIZE - 1)] = feed[i];
        feed += 0x31;
        for (int32_t i = 0; i < 0x19; i++)
            game_delay();
    }
}

/* 1ac2:02c8  the demo
 *
 * F2, and the same three instructions the menu falls into by itself when the
 * banner runs out of text - which is how attract mode comes on. The original
 * is `call 0x85 / call 0x1212 / call 0x1509 / jmp 0x2f5`: the last is a jump,
 * not a call, so the demo *is* a session and it ends the way any session
 * ends. Doing the first three and going back to the menu, which is what this
 * used to do, starts nothing at all.
 *
 * Unlike F1 there is no play_prepare and no INT 09h: demo_start points the
 * screen handler at 0x1785, which replays a recorded script instead of
 * reading a keyboard.
 */
static void start_demo(void)
{
    speaker_on();                       /* 1ac2:0085 */
    play_frame();                       /* 1ac2:1212 */
    demo_start();                       /* 1ac2:1509 */
    if (setjmp(g_back_to_menu) == 0)
        play_session();                 /* 1ac2:02f5, left by longjmp */
}

const char *find_exe(void)
{
    static const char *candidates[] = {
        "popcorn.exe", "POPCORN.EXE",
        "../popcorn/popcorn.exe", "popcorn/popcorn.exe",
        "../popcorn/POPCORN.EXE",
    };
    const char *env = getenv("POPCORN_EXE");
    if (env)
        return env;
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

size_t popcorn_load_image(void)
{
    const char *path = find_exe();
    if (!path) {
        fprintf(stderr,
                "popcorn: cannot find POPCORN.EXE.\n"
                "         Copy your own next to this binary, or set "
                "POPCORN_EXE to its path.\n"
                "         It ships beside this binary; a copy has been "
                "moved or deleted.\n");
        return 0;
    }
    size_t len = 0;
    g_image = exepack_load(path, &len);
    if (!g_image)
        return 0;
    printf("popcorn: %s -> %zu bytes of load image\n", path, len);
    if (len != IMAGE_LEN)
        fprintf(stderr, "popcorn: note: expected %d bytes, got %zu\n",
                IMAGE_LEN, len);
    return len;
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
#define INPUT_KEYBOARD  0x16d2
#define INPUT_DEMO      0x1785          /* demo_start installs this one */
#define INPUT_MOUSE     0x1654


static void menu_redraw(void)
{
    speaker_off();
    flush_keys();                   /* 1ac2:0106 */
    restore_screen();
    if (gv.input_selected != INPUT_KEYBOARD)
        menu_arrow();
    gv.particle_count = (uint16_t)(0x50);
    menu_particles_init(0xb800);
    gv.banner_state = 2;
    gv.banner_ptr = (uint16_t)img_off(gv.banner_text);
}

void game_main(const char *dir, const char *levels)
{
    /* The original reads POPSPEED's value out of the offset half of interrupt
     * vector 0x68 here. Nothing sets that vector under the port, so this is
     * always the "POPSPEED was never run" path - which is what read_speed_
     * setting's 0 means, and it leaves DELAY_COUNT at the readme's 110.
     *
     * There is no way to pass anything else any more, and that is deliberate.
     * The value's whole job was to trim the busy-wait the play loop paced
     * itself on, and the play loop paces on the CGA refresh now (see
     * io_frame_pace), so it no longer reaches the thing it exists to tune. It
     * still reaches game_delay everywhere *outside* the play loop - the intro,
     * the transitions, the ending - and 110 is what those should run at.
     *
     * Offering it would also have been actively wrong: the value 1 patches the
     * delay to a `ret`, and play_setup keys off that byte to open the ball's
     * gate from 3 to 0xfa. Under the old pacing those two moved together;
     * under this one it would have sped the ball up without touching the frame
     * rate, which is not a setting the original has. */
    read_speed_setting(0);
    /* `POPCORN POPTAB` loads POPTAB.PPC over the built-in table. The original
     * builds the name from the PSP command tail at 1ac2:0157 - copy it to
     * gv.level_file, skip a leading dot, and append ".PPC" unless it already has
     * an
     * extension - and calls the loader at 1ac2:0187 before anything is drawn.
     * Reading the tail is the machine's job and is not transcribed, so the
     * name is built here from --cmdline. */
    if (levels && *levels) {
        char *name = gv.level_file;
        size_t n = 0;
        while (levels[n] && n < sizeof gv.level_file - 5) {
            name[n] = levels[n];
            n++;
        }
        name[n] = 0;
        /* 1ac2:0160 - leading dots are stepped over when looking for an
         * extension, and stay in the name that is opened: the loader at
         * 1ac2:08c8 opens gv.level_file itself. So `.LTF` becomes `.LTF.PPC` rather
         * than being taken as already extended. It fails either way; matching
         * it costs two lines and means the name-building is the original's
         * rather than nearly it. */
        size_t k = 0;
        while (name[k] == '.')
            k++;
        if (!strchr(name + k, '.'))
            memcpy(name + n, ".PPC", 5);
        if (!level_load_file(dir))
            return;                     /* the original exits to DOS */
    }
    gv.input_selected = (uint16_t)(INPUT_KEYBOARD);
    gv.cheat_done = 0;
    cv.sound_on = 1;
    load_high_scores(dir);
    build_shifted_sprites();

    /* INT 10h AX=0005 - the window is the mode set. */
    intro_curtain();
    intro_logo();
    intro_reveal();
    intro_scroll();
    intro_paddle();
    save_screen();

    for (;;) {
        menu_redraw();
        int32_t back_to_menu = 0;
        while (!back_to_menu) {
            if (!io_pump())
                return;

            if (!io_key_ready()) {
                /* The idle path: step the decoration, and when the banner
                 * runs out of text start the demo, which is how the attract
                 * mode comes on by itself. */
                if (*img_ptr(gv.banner_ptr) == 0) {
                    start_demo();
                    back_to_menu = 1;
                    continue;
                }
                menu_particles_tick();
                menu_banner_tick();
                io_present();
                io_wait_retrace();
                continue;
            }

            uint32_t key = io_get_key();

            /* 1ac2:0213 is a chain of compares, not a table, and the cheat
             * matcher sits in the middle of it at 1ac2:0240 rather than at
             * the end - so the order of these three is the original's:
             *
             *   F9 toggles the sound and falls through into the matcher;
             *   F8 jumps clean past it at 1ac2:0226, which makes it the one
             *      key that does not disturb a cheat half typed;
             *   F7 clears the matcher and is then fed to it.
             *
             * F1 reaches the matcher too, at 1ac2:0259, *before* the game
             * starts - and the original's return from a game is a stack
             * throw to 1ac2:01d1, well past here. Feeding the matcher after
             * play_session instead, as this did, both reversed that and read
             * a `key` no longer guaranteed to survive the longjmp. */
            if ((key >> 8) == 0x42) {                   /* F8: palette */
                palette_cycle();
                io_present();
                continue;
            }
            if ((key >> 8) == 0x43)                     /* F9: sound */
                cv.sound_on ^= 1;
            if ((key >> 8) == 0x41) {                   /* F7: forget the
                                                         * cheat so far */
                gv.cheat_done = 0;
                gv.cheat_at = (uint16_t)img_off(gv.cheat_text);
            }
            if (gv.cheat_done != 1)                     /* 1ac2:0239 */
                cheat_match((uint8_t)(key & 0xff));

            switch (key >> 8) {
            case 0x44:                                  /* F10 */
                /* The boss key. employee_enter is a no-op here on purpose, so
                 * nothing is stashed and screen_restore is not called either -
                 * it would put back whatever the last stash left. F10 does
                 * nothing in the port, which is the intent. */
                employee_enter();
                while (io_key_ready() && (io_get_key() >> 8) == 0x44)
                    ;
                break;
            case 0x3d:                                  /* F3: mouse */
                if (gv.input_selected != INPUT_MOUSE) {
                    gv.input_selected = (uint16_t)(INPUT_MOUSE);
                    menu_arrow();
                }
                break;
            case 0x3e:                                  /* F4: keyboard */
                if (gv.input_selected != INPUT_KEYBOARD) {
                    gv.input_selected = (uint16_t)(INPUT_KEYBOARD);
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
                start_demo();
                back_to_menu = 1;
                break;
            case 0x3b:                                  /* F1: play */
                gv.input_active = (uint16_t)(gv.input_selected);
                speaker_on();
                if (screen_player_names() == 0xff) {
                    back_to_menu = 1;
                    break;
                }
                play_prepare();
                if (gv.input_active == 0x16d2)
                    install_int09();            /* 1ac2:02ea, keyboard only */
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
            io_present();
        }
    }
}

/* 1ac2:1ab1 and 1ac2:1a4f  game_input
 *
 * `call word ptr [0x2d45]` - whichever input routine is installed. There are
 * **three**, not two: the mouse at 0x1654, the keyboard at 0x16d2, and the
 * demo's own at 0x1785, which demo_start puts there. Dispatching only the
 * first two sends the demo to the keyboard routine, which without a key held
 * never moves the paddle - so the demo loses its ball and ends immediately.
 *
 * The mouse one needs the pointer, which is the platform's; the keyboard one
 * reads only the three state bytes, which the platform maintains; the demo's
 * reads the ball.
 */
/* 1ac2:1654, the prologue to the mouse read at 1ac2:169f - and the port had
 * only the read. Everything the game does with a key *during play* is here,
 * so without it Esc, F9 and F10 all did nothing.
 *
 * Esc does not leave the level. It **pauses**: the screen goes aside, the
 * overlay goes on, and the game waits for a key. `and al,0xdf / cmp al,0x41`
 * - so **A**, either case - clears the entities and abandons the game back to
 * the menu the way the mouse handler does at 1ac2:167e. Any other key puts
 * the screen back and play carries on. */
static void input_keys_mouse(void)
{
    if (!io_key_ready())
        return;                         /* 1ac2:1658 */
    uint32_t ax = io_get_key();

    /* 1ac2:165e is F9, and it is **not** repeated here. The original reaches
     * it only with the mouse, because the keyboard mode installs a handler
     * that does not chain to the BIOS and starves INT 16h; the port's
     * platform layer calls int09_handler for every scan code in both modes,
     * so the 0xc3 toggle in there has already done it. Doing it again would
     * toggle twice and look exactly like F9 not working. */
    if (ax == 0x011b) {                 /* 1ac2:1669, Esc */
        screen_stash();                 /* 1ac2:4ba9 */
        uint32_t k;
        do {
            io_present();
            if (!io_pump())
                return;
        } while (!io_key_ready());
        k = io_get_key() & 0xff;
        if ((k & 0xdf) == 'A') {        /* 1ac2:1677 */
            entities_clear();           /* 1ac2:55e */
            longjmp(g_back_to_menu, 1); /* sp = [0x1405]; jmp 0x1d1 */
        }
        screen_unstash();               /* 1ac2:4c13 */
        return;
    }
    if (ax == 0x4400) {                 /* 1ac2:168b, F10 */
        /* The boss key is a no-op here on purpose - see employee_enter - so
         * nothing was stashed and 1ac2:169c's screen_restore is skipped with
         * it, exactly as the menu's F10 does. */
        employee_enter();               /* 1ac2:4ae0 */
        while (io_key_ready() && io_get_key() == 0x4400)
            ;                           /* 1ac2:1693 */
    }
}

/* 1ac2:16d2, the same pause on the keyboard path. It tests [0x2d49] - the
 * make code the game's own INT 09h handler left - against 1, which is Esc,
 * and takes the BIOS handler back so INT 16h has something to read. */
static void input_keys_keyboard(void)
{
    if (gv.last_make != 1)
        return;
    screen_stash();                     /* 1ac2:4ba9 */
    restore_int09();                    /* 1ac2:03d1 */
    uint32_t k;
    do {
        io_present();
        if (!io_pump())
            return;
    } while (!io_key_ready());
    k = io_get_key() & 0xff;
    if ((k & 0xdf) == 'A') {            /* 1ac2:16e5 */
        entities_clear();
        longjmp(g_back_to_menu, 1);
    }
    screen_unstash();                   /* 1ac2:4c13 */
    install_int09();                    /* 1ac2:03b0 */
    gv.last_make = 0xff;          /* 1ac2:16ef */
}

void game_input(void)
{
    uint32_t which = gv.input_active;
    if (which == INPUT_MOUSE) {
        input_keys_mouse();
        input_mouse(io_mouse_x(), io_mouse_buttons());
    } else if (which == INPUT_DEMO) {
        input_demo();
    } else {
        input_keys_keyboard();
        input_keyboard();
    }
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

/* One step of the safety net's crawl: blank the two bytes it occupies, move a
 * row down, and reload the counter.
 *
 * It was `erase_shot`, and took the position and timer as *offsets* so it
 * could serve a laser shot too. It never did - it has one caller, the net's -
 * and naming its variables correctly left the parameters with nothing to
 * generalise over. */
static void net_step(uint32_t reload)
{
    uint32_t di = gv.net_pos;
    g_vram[di & (CGA_SIZE - 1)] = 0;
    g_vram[(di + 1) & (CGA_SIZE - 1)] = 0;
    gv.net_pos = (uint16_t)cga_next_row(di);
    gv.net_timer = (uint8_t)reload;
}

/* Set by the lockstep harness when it is resuming from a snapshot taken at a
 * frame boundary rather than at this routine's entry. Everything above the
 * frame loop - the panel, the level draw, the serve wait - has already
 * happened in the state being restored, so running it again would replay half
 * a second of a level that is already under way. */
int32_t g_resume_at_frame_top;

int32_t play_loop(void)
{
    if (g_resume_at_frame_top) {
        g_resume_at_frame_top = 0;
        goto frames;
    }
    /* The level number, drawn into the header bar as two digits. */
    uint32_t n = (gv.level_number + 1) & 0xff;
    gv.level_num_text = (uint16_t)(((n % 10) << 8 | (n / 10)) + 0x3030);

    uint32_t di = 0x177e;
    for (int32_t i = 0; i < 12; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] = 0xaa;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = 0xaa;
    }
    draw_text(gv.level_text, 0xc, 0x377e);
    di = 0x377e + 0x1e0;
    for (int32_t i = 0; i < 12; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] = 0xaa;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = 0xaa;
    }

    level_draw();                       /* 1ac2:1c4f */

    /* And wipe it off again - fourteen scan lines of nothing over the bar
     * and the level name. The `sub di, 0x18` in the original only puts back
     * what `rep stosw` advanced; taking it literally walks the wipe left a
     * band a row and leaves the banner on screen under everything else. */
    di = 0x177e;
    for (int32_t dl = 0xe; dl > 0; dl--) {
        for (int32_t i = 0; i < 24; i++)
            g_vram[(di + i) & (CGA_SIZE - 1)] = 0;
        di = cga_next_row(di);
    }

    gv.paddle_x = gv.paddle_prev_x = 0x64;
    gv.paddle_kind = 0;
    /* `mov ax, [di+2] / ... / mov [0x2d3a], al` - the width is the **low**
     * byte of the word at 0x2d0f, not the high one at 0x2d10. With the high
     * byte the width came out zero, and a zero-width paddle clamps to the
     * left wall the moment the mouse is read. */
    gv.paddle_width = gv.paddle_sets[0].width;
    gv.paddle_max = 0xac;
    gv.paddle_min = 0x08;
    gv.repeat_count = 5;
    gv.repeat_div = 5;
    io_mouse_warp(0x64 * 2, 0xb8);

    paddle_row_offsets(gv.paddle_x, &gv.paddle_rows[0]);
    memcpy(gv.paddle_pix[0], gv.paddle_sprites[0][0], 0x27 * 2);
    gv.ball_alive = 1;
    memcpy(gv.balls[0].sprite, gv.ball_start_sprite, sizeof gv.balls[0].sprite);
    gv.frame_delay_set = 0x1f4;
    gv.key_right = gv.key_left = 0;
    gv.repeat_div = 0;
    gv.key_action = 0;
    gv.speed_step = gv.speed_limit = 0xfa;
    if (cv.delay_entry != 0xc3) {
        gv.speed_step = 3;
        gv.speed_limit = 3;
    }
    gv.speed_timer = 0x4e20;
    gv.entity_remove = 0;
    gv.bonus_pending = gv.bonus_live = gv.bonus_cap = 0;
    gv.paddle_morphing = 0;
    gv.net_on = gv.caught = 0;
    gv.game_over = gv.extra_on = gv.laser_on = 0;

    /* The serve: ball 0 on the paddle, the other two idle. */
    ball_t *b = &gv.balls[0];
    b->x = b->prev_x = 0x70;
    b->y = b->prev_y = 0xb5;
    b->dy = 1;
    b->dx = 2;
    b->dir_x = 0;
    b->dir_y = 1;
    b->anchor_x = b->x;
    b->anchor_y = b->y;
    b->acc_x = b->acc_y = 0;
    b->state = 1;
    b->bounces = 0;
    gv.balls[1].state = 0;
    gv.balls[2].state = 0;
    ball_draw(b->sprite, b->x, b->y);

    flush_keys();                   /* 1ac2:0106 */

    /* Wait for the action key, or two thousand ticks, before serving. */
    if (gv.input_active != 0x1785) {
        gv.serve_timeout = (uint16_t)(0x7d0);
        for (;;) {
            gv.serve_timeout = (uint16_t)(gv.serve_timeout - 1);
            if (gv.serve_timeout == 0)
                break;
            for (int32_t i = 0; i < 0xf; i++)
                game_delay();
            game_input();
            if (gv.key_action == 1)
                break;
            draw_paddle(gv.paddle_sprites[0][0]);
            io_present();
            if (!io_pump())
                return 1;
        }
    }

frames:
    for (;;) {                          /* one iteration is one frame */
        gv.frame_delay = gv.frame_delay_set;
        demo_input_step();

        if (gv.ball_alive == 0) {
            io_log_random(0x9000);      /* tagged for sidebyside */
            play_teardown();
            gv.game_over = 1;
            return 1;                   /* the original's `stc` */
        }
        if (gv.level.bricks == 0) {
            io_log_random(0x9001);
            play_teardown();
            return 0;                   /* `clc`: the bricks are gone */
        }

        game_input();
        if (gv.paddle_morphing == 0)
            draw_paddle(img_ptr(gv.paddle_sets[gv.paddle_kind].sprites));
        if (gv.laser_on)
            laser_fire();

        /* Every 0x4e20 frames the ball is allowed to move one step more
         * often, up to the limit - the level speeds up the longer it runs. */
        gv.speed_timer--;
        if (gv.speed_timer == 0) {
            gv.speed_timer = 0x61a8;
            if (gv.speed_limit != 0xff)
                gv.speed_limit++;
            gv.speed_step = gv.speed_limit;
        }

        if (--gv.speed_step != 0) {
            for (int32_t i = 0; i < 3; i++) {
                ball_t *ball = &gv.balls[i];
                uint8_t st = ball->state;
                if (st == 0 || st >= 3)
                    continue;
                if (gv.caught == 1 && !ball_on_paddle(ball))
                    continue;
                ball_step(ball);
                if (!ball_redraw(ball)) {
                    io_log_random(0x9002);
                    play_teardown();
                    gv.game_over = 1;
                    return 1;
                }
                if (gv.level.bricks == 0) {
                    io_log_random(0x9003);
                    play_teardown();
                    return 0;
                }
                ball_after(ball);
            }
        } else {
            gv.speed_step = gv.speed_limit;
        }

        /* The entity list. A handler asks to be taken out by setting
         * gv.entity_remove - twelve of them do - and the walk does the
         * unlinking, which is what makes the four details below matter.
         *
         * entity_prev trails one node behind, so the predecessor is to hand
         * and the list needs no second walk to find it. It starts at the head
         * *node*, so removing the first entity is not a special case.
         *
         * It is deliberately **not** advanced on the removal branch: after an
         * unlink the predecessor is still the predecessor of whatever comes
         * next, which is what lets two entities in a row be removed.
         *
         * `next` is read **before** entity_unlink, because unlink overwrites
         * that same field with the free-list head - the node's link is reused
         * to push it onto the free list. Reading it afterwards would walk the
         * free list.
         *
         * And the flag is a single global, consumed by entity_unlink, so it is
         * one request from one handler and cannot outlive the node it was set
         * for.
         *
         * One consequence worth knowing: entity_alloc appends to the **tail**,
         * and this walk runs to the tail, so an entity created by a handler
         * during the walk is called in the same frame it was born. */
        gv.entity_prev = img_off(&gv.entity_head);
        uint32_t bx = gv.entity_head.next;
        while (bx != 0xffff) {
            entity_call(entity_at(bx));
            if (gv.entity_remove == 0) {
                gv.entity_prev = (uint16_t)(bx);
                bx = entity_at(bx)->next;
            } else {
                uint32_t next = entity_at(bx)->next;
                entity_unlink(bx);
                bx = next;
            }
        }

        if (gv.net_on) {
            if (--gv.net_timer == 0)
                net_step(0xc8);
            gv.net_life = (uint16_t)(gv.net_life - 1);
            if (gv.net_life == 0) {
                gv.net_on = 0;
                flash_bar(0x1554);
            }
        }
        if (gv.extra_on) {
            gv.extra_timer = (uint16_t)(gv.extra_timer - 1);
            if (gv.extra_timer == 0) {
                /* One cell off the bar. `stosw` leaves di two on, and the
                 * two `dec di` put it back, so what follows is a plain step
                 * to the next scan line - the bar drains downwards. */
                uint32_t di = gv.extra_pos;
                img_vram_setw(di, 0);
                gv.extra_pos = (uint16_t)(cga_next_row(di));
                gv.extra_timer = (uint16_t)(0x190);
            }
            gv.serve_timeout = (uint16_t)(gv.serve_timeout - 1);
            if (gv.serve_timeout == 0)
                gv.extra_on = 0;
        }

        /* A pause that gets shorter as [0x33d6] rises: three passes of 0xb4
         * empty loops, minus one per point of it. Kept for its shape - the
         * frame is paced on the refresh below, so this no longer sets a
         * speed, and the [0x33d6] taper rides on io_frame_pace instead. */
        for (int32_t i = 3 - gv.bonus_live; i > 0; i--)
            io_delay_cycles(0xb4 * CYCLES_PER_LOOP);

        if (gv.extra_on != 1 && gv.bonus_pending != 3 &&
            game_random(io_ticks(), 0x86) == 0)
            bonus_spawn();

        sound_tick();

        /* Where the original spent FRAME_DELAY empty loops and then POPSPEED's
         * own. The frame is paced against the CGA refresh instead - see
         * io_frame_pace. */
        io_frame_pace();

        /* 1ac2:1c3f, the `jmp 0x1a62` that closes the frame. The top of the
         * loop is the wrong place to sync a frame against the emulator: the
         * serve wait reaches 0x1a62 too, at 0x1a58, every time the action
         * button is held - and a bot holds it permanently. This is the one
         * instruction both paths through the frame converge on. */
        io_frame_sync();

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
#define LEVEL_TABLE   0x000c            /* within the 0xc46 block */
#define LEVEL_BYTES     0xb0
#define LEVEL_COUNT     0x32

jmp_buf g_back_to_menu;

/* Where the end-level bonus lands. 1ac2:2da0 throws four words off the stack
 * before jumping into the
 * end-level bonus: the frames of bonus_effect, morph_finish, entity_paddle_fx
 * and the entity walk that reached them. The bonus never returns to any of
 * those - its own `ret`, after the ending has run, lands back in play_session
 * as if play_loop had returned. Returning normally instead, which is what the
 * port did, left the play loop running the level the bonus had just finished. */
jmp_buf g_bonus_done;

/* Set with g_resume_at_frame_top when the lockstep harness resumes from a
 * snapshot: everything above the retry loop has happened already in the state
 * being restored, and the first play_loop resumes mid-frame. Without this a
 * resumed run cannot follow a lost life into life_lost and level_intro, so
 * any divergence that lives there is unreachable from a snapshot. */
int32_t g_resume_in_session;
/* Resume *inside* the end-of-level bonus. The bonus is the one screen that
 * cannot be played into reliably - it needs a + capsule, and + is 2 chances in
 * 255 of a brick-2 capsule and never comes out of a hatch - so it is captured
 * at 1ac2:4210 and injected. Running the body alone is not enough: the bonus
 * *returns into play_session*, and a port that stops when it ends is somewhere
 * the emulator never is, which reads as a divergence at exactly the moment the
 * screen is most interesting. This rejoins where the longjmp would have. */
int32_t g_resume_in_bonus;

/* popcorn-dev --level N: which level a game starts on, or -1 for the first,
 * which is what the game does. Watching what goes wrong on level 34 should
 * not mean playing thirty-three levels to reach it. */
int32_t g_start_level = -1;

void play_session(void)
{
    if (g_resume_in_session) {
        g_resume_in_session = 0;
        goto retry;
    }

    memcpy(gv.player_name, gv.players[0].name, sizeof gv.players[0].name);
    memset(gv.score_text, '0', sizeof gv.score_text);
    gv.extra_at = 0x3032;               /* the first one at "20" */
    gv.cur_player = 0;
    gv.live_count = gv.player_count;
    gv.lives = 5;

    /* A demo starts on a random level; a game always starts on the first. */
    uint32_t lv = game_random(io_ticks(), 0x1e);
    if (gv.input_active != 0x1785)
        lv = 0;
    /* popcorn-dev --level N. The draw above still happens: it is one of the
     * PRNG's callers and skipping it would shift every number the rest of the
     * game takes, so a level started this way would not be the level that is
     * played normally. Overriding the result afterwards leaves the sequence
     * exactly where the game put it. */
    if (g_start_level >= 0)
        lv = (uint32_t)g_start_level;
    gv.level_number = (uint8_t)lv;
    gv.level_src = (uint16_t)(lv * LEVEL_BYTES + LEVEL_TABLE);
    panel_draw();

    for (;;) {
        level_colours();                        /* 1ac2:044b */
        /* level_src and level_number are written together everywhere the
         * game touches either - set, stepped, wrapped at LEVEL_COUNT, saved
         * into a player's record and restored from it - so level_src is
         * always LEVEL_TABLE + level_number * LEVEL_BYTES, which is this
         * record. The game keeps both because the results screen walks the
         * offset. */
        gv.level = c46.levels[gv.level_number];

        for (;;) {                              /* one level, retried on death */
            level_intro();                      /* 1ac2:1eb9 */
retry:
            for (;;) {
                /* The bonus lands here rather than unwinding through the
                 * entity walk - see g_bonus_done - and it arrives as a level
                 * **completed**, carry clear. Watching [0x2f10] settles it:
                 * the emulator advances the level without the brick count
                 * ever reaching zero, which only the `jae 0x376` at 1ac2:0357
                 * does. Arriving as a lost life instead cost a life and
                 * replayed the level. */
                int32_t jumped = setjmp(g_bonus_done);
                int32_t lost;
                if (!jumped && g_resume_in_bonus) {
                    g_resume_in_bonus = 0;
                    lost = bonus_end_level_body() == 2;
                } else {
                    lost = jumped ? (jumped == 2) : play_loop();
                }
                speaker_off();
                if (!lost)
                    goto level_done;
                life_lost();                    /* 1ac2:0735 */
                if (gv.cheat_done != 1)
                    gv.lives--;
                if (gv.game_over == 1)
                    break;
            }
            screen_game_over();                 /* 1ac2:0473 */
            if (next_player(g_dir))             /* 1ac2:0d2e */
                goto retry;                     /* 0d7a: jmp 0x34f, no intro */
        }

    level_done:
        screen_level_done();                    /* 1ac2:0521 */
        gv.level_src = (uint16_t)(gv.level_src + LEVEL_BYTES);
        gv.level_number++;
        if (gv.level_number == LEVEL_COUNT) {
            gv.level_number = 0;
            gv.level_src = (uint16_t)(LEVEL_TABLE);
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
static uint32_t cga_at(uint32_t x, uint32_t y)
{
    uint32_t di = x >> 2;
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
 * in cell_bitmap.
 */
#define BRICK_TOP        6              /* first scan line of the field */
#define BRICK_LEFT       2              /* bytes, so eight pixels */
#define BRICK_COLS      12
#define BRICK_HEIGHT     8              /* scan lines */
#define BRICK_BYTES      4              /* 16 pixels */

/* Returns the DI it leaves, which 1ac2:46dc depends on: the curtain lays its
 * second cap at [di] without saving DI across this call, and 1ac2:2034 opens
 * `xor di, di` - so the cap goes at the end of the brick row, not where the
 * caller had DI. The `inc di / inc di` at 1ac2:46cc is dead for the same
 * reason. */
uint32_t draw_brick_row(uint32_t y)
{
    uint32_t di = cga_at(0, y) + BRICK_LEFT;
    uint32_t row = (y - BRICK_TOP) & 0xff;
    uint32_t sub = (row & 7) * 4;
    const uint8_t *cells = &gv.level.cells[(row >> 3) * BRICK_COLS];

    for (int32_t c = 0; c < BRICK_COLS; c++, di += BRICK_BYTES) {
        uint32_t cell = cells[c];
        if (cell == 0x0c) {
            cell_special(row & 0xff, c, di);
            continue;
        }
        uint32_t base = (cell >= 24 ? SEG_14A1 : 0) + gv.cell_bitmap[cell];
        const uint8_t *src = g_image + base + sub;
        for (int32_t b = 0; b < BRICK_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = src[b];
    }
    return di;
}

/* 1ac2:20b9  draw_sprite_20x6
 *
 * Six rows of five bytes at a pixel position - the popcorn kernels the level
 * intro sweeps down the screen.
 */
void draw_sprite_20x6(uint32_t x, uint32_t y, const uint8_t *src)
{
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 6; r++) {
        for (int32_t b = 0; b < 5; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = src[r * 5 + b];
        di = cga_next_row(di);
    }
}

/* ========================================================================
 * 1ac2:1eb9  level_intro
 *
 * The level arriving. Three parts, and the port used to have an invention in
 * place of the first and third.
 *
 * First the panel scrolls up and a new one feeds in underneath: eight rows of
 * 0x30 bytes from 0x6d9f, then nineteen of five from 0x6d36, each preceded by
 * one call to scroll_up_band. The source pointer walks on across the calls -
 * the `push si` / `pop si` are there so the call cannot disturb it, and the
 * `rep movsw` that follows is what advances it.
 *
 * Then the brick field, swept by **four popcorn kernels**. The loop has no
 * counter of its own: [0x2f0c] *is* kernel zero's y, and it moves only when
 * that kernel's timer at [0x2efc] runs out, so the kernels pace the reveal.
 * Phase one counts it down from 0xb3 to 0x0c laying the backdrop over the
 * previous level; phase two counts it back up to 0xb3 drawing the bricks. The
 * four records at 0x2efc are (timer, period, sprite pointer).
 *
 * Finally the panel scrolls back down 0x1b times.
 *
 * Driving the reveal directly, with no kernels, is what the port did while
 * 0x2109 was still a stub. It left the right picture on screen by the end and
 * the wrong one at every frame before it - which is what put a TABLEAU banner
 * in the port that the emulator had already cleared.
 * ===================================================================== */
#define SWEEP_X     0x60

static void intro_pause(int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        game_delay();                   /* 1ac2:164c */
}

void level_intro(void)
{
    /* The panel scrolls up, a fresh row feeding in at the bottom. */
    const uint8_t *feed = gv.backdrop[0];
    for (int32_t bl = 8; bl > 0; bl--) {
        /* 1ac2:1ec4. The intro runs *before* the play loop, so io_frame_sync
         * has not started and none of this is compared by anything - the same
         * gap the ending and the results screen were in. */
        io_frame_sync_extra(SYNC_INTRO);
        scroll_up_band();               /* 1ac2:2109 */
        for (int32_t i = 0; i < 0x18 * 2; i++)
            g_vram[(0x3ef2 + i) & (CGA_SIZE - 1)] = feed[i];
        feed += 0x18 * 2;
        io_delay_cycles(0x7d0 * CYCLES_PER_LOOP);
        io_present();
        if (!io_pump())
            return;
    }
    feed = gv.intro_feed[0];
    for (int32_t bl = 0x13; bl > 0; bl--) {
        io_frame_sync_extra(SYNC_INTRO);        /* 1ac2:1ee0 */
        scroll_up_band();
        for (int32_t i = 0; i < 5; i++)
            g_vram[(0x3f08 + i) & (CGA_SIZE - 1)] = feed[i];
        feed += 5;
        io_delay_cycles(0x8fc * CYCLES_PER_LOOP);
        io_present();
        if (!io_pump())
            return;
    }

    gv.sweep_y[3] = 0xc2;
    gv.sweep_y[2] = 0xbd;
    gv.sweep_y[1] = 0xb8;
    gv.sweep_y[0] = 0xb3;

    /* Up the screen, laying the backdrop over what was there. */
    while (gv.sweep_y[0] != 0x0c) {
        io_frame_sync_extra(SYNC_INTRO);        /* 1ac2:1f13 */
        field_backdrop((gv.sweep_y[0] - 7) & 0xff);
        for (uint32_t k = 0; k < 4; k++) {
            sweep_t *st = &gv.sweep[k];
            st->timer++;
            if (st->period != st->timer)
                continue;
            st->timer = 0;
            gv.sweep_y[k]--;
            draw_sprite_20x6(SWEEP_X, gv.sweep_y[k], img_ptr(st->sprite));
        }
        intro_pause(0xa);
        io_present();
        if (!io_pump())
            return;
    }

    /* And back down, drawing the bricks. The kernels are walked backwards
     * here - `mov cx,3` and down to -1 - which is not the same order as the
     * sweep up, and shows: they trail the reveal instead of leading it. */
    while (gv.sweep_y[0] != 0xb3) {
        uint32_t y = (gv.sweep_y[0] - 6) & 0xff;
        draw_brick_row(y);              /* 1ac2:2034 */
        field_backdrop((y + 1) & 0xff); /* 1ac2:1fc1 */
        for (int32_t k = 3; k >= 0; k--) {
            sweep_t *st = &gv.sweep[k];
            if (st->timer == 0) {
                st->timer = st->period;
                gv.sweep_y[k]++;
                draw_sprite_20x6(SWEEP_X, gv.sweep_y[k], img_ptr(st->sprite));
            }
            st->timer--;                /* both ways round, after the draw */
        }
        intro_pause(0xa);
        io_present();
        if (!io_pump())
            return;
    }

    /* The panel back down. */
    for (int32_t n = 0x1b; n > 0; n--) {
        scroll_down_band();             /* 1ac2:2148 */
        intro_pause(0xc);
        io_present();
        if (!io_pump())
            return;
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:2881  ball_draw
 *
 * Four rows of one word - 16 pixels by 4 - XORed in at a pixel position, so
 * drawing the same sprite twice erases it. Everything that moves in this game
 * is drawn this way.
 */
/* `rows` is void, not uint16_t *, on purpose. The four words live inside a
 * packed record at an odd address - gv.balls[i] is at 0x2ea1 + i * 0x1e - so a
 * typed pointer to them claims an alignment they do not have, and GCC is right
 * to warn about it. Bytes have no alignment to get wrong, and bytes are what
 * goes into video memory anyway. */
void ball_draw(const void *rows, uint32_t x, uint32_t y)
{
    const uint8_t *p = rows;
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 4; r++) {
        g_vram[di & (CGA_SIZE - 1)] ^= p[r * 2];
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= p[r * 2 + 1];
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

int32_t ball_redraw(ball_t *b)
{

    memcpy(b->prev_spr, b->sprite, sizeof b->sprite);
    memcpy(b->sprite, gv.ball_start_sprite, sizeof b->sprite);

    uint32_t shift = (b->x & 3) * 2;
    if (shift) {
        for (int32_t r = 0; r < 4; r++) {
            uint32_t w = b->sprite[r];
            b->sprite[r] = (uint16_t)((w >> shift) | (w << (16 - shift)));
        }
    }

    ball_draw(b->prev_spr, b->prev_x, b->prev_y);
    b->prev_x = b->x;
    b->prev_y = b->y;
    ball_draw(b->sprite, b->x, b->y);
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

void ball_after(ball_t *b)
{

    if (b->bounces >= 0x23) {
        b->bounces = 0;
        b->anchor_x = b->x;
        b->anchor_y = b->y;
        b->acc_x = b->acc_y = 0;
        b->dy = (uint8_t)(game_random(io_ticks(), 5) + 1);
        b->dx = (uint8_t)(game_random(io_ticks(), 5) + 1);
    }

    uint32_t x = b->x, y = b->y;
    if (x <= WALL_LEFT || x >= WALL_RIGHT) {
        cv.sound_request = SOUND_BOUNCE;
        b->dir_x = (x <= WALL_LEFT) ? 0 : 1;
        b->acc_x = 1;
        b->acc_y = 0;
        b->anchor_x = (uint8_t)(x <= WALL_LEFT ? 9 : 0xc3);
        b->anchor_y = (uint8_t)y;
        b->bounces++;
    }
    if (y <= WALL_TOP) {
        cv.sound_request = SOUND_BOUNCE;
        b->dir_y = 0;                 /* downwards */
        b->bounces++;
        b->acc_x = 0;
        b->acc_y = 1;
        b->anchor_x = (uint8_t)x;
        b->anchor_y = (uint8_t)(y + 1);
    }

    ball_bricks(b);                     /* 1ac2:254d */

    if (b->y != FLOOR) {
        ball_paddle(b);                 /* 1ac2:2316 */
        return;
    }
    if (gv.net_on == 1) {     /* the net catches it */
        cv.sound_request = SOUND_BOUNCE;
        b->anchor_x = b->x;
        b->anchor_y = 0xc3;
        b->dir_y = 1;                 /* upwards */
        b->acc_x = 1;
        b->acc_y = 1;
        return;
    }
    /* Lost. Erase it, mark it idle, and take one off the live count - the
     * play loop notices [0x2e73] reaching zero at the top of the next frame. */
    b->state = 0;
    ball_draw(b->sprite, b->x, b->y);
    gv.ball_alive--;
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
#define SLOPE_TOP     img_off(gv.slope_top)
#define SLOPE_SIDE    img_off(gv.slope_side)
#define SOUND_PADDLE     1

/* The common tail of every top-of-paddle bounce: reverse vertically, anchor
 * one pixel clear of the paddle, and restart the accumulators. */
static void paddle_bounce_up(ball_t *b)
{
    uint32_t ah = b->y;
    if (b->y == PADDLE_BOTTOM) {
        b->dir_y = 0;                 /* it came from below: send it down */
        ah++;
    } else {
        b->dir_y = 1;
        ah--;
    }
    b->anchor_x = b->x;
    b->anchor_y = (uint8_t)ah;
    b->acc_x = b->acc_y = 1;
    b->bounces = 0;
    cv.sound_request = SOUND_PADDLE;
}

static void paddle_slope(ball_t *b, uint32_t table, uint32_t index)
{
    uint32_t w = img_w(table + index * 2);
    b->dy = (uint8_t)w;
    b->dx = (uint8_t)(w >> 8);
}

void ball_paddle(ball_t *b)
{
    uint32_t y = b->y;

    if (y < PADDLE_TOP || y > PADDLE_BOTTOM)
        return;

    if (y > PADDLE_TOP && b->dir_y != 1) {
        /* The sides. Only the two single columns just outside the paddle
         * count, which is why this is an equality and not a range. */
        uint32_t left = (gv.paddle_x - 3) & 0xff;
        uint32_t bx = b->x;
        int32_t from_left = 1;
        if (bx != left) {
            uint32_t off = (bx - left) & 0xff;
            if (off != ((gv.paddle_width + 3) & 0xff))
                return;
            from_left = 0;
        }
        uint32_t depth = (y - 0xb6) & 0xff;
        b->dir_y = (depth <= 5) ? 1 : 0;
        b->dir_x = (uint8_t)from_left;
        b->anchor_x = from_left
            ? (uint8_t)(gv.paddle_x - 4)
            : (uint8_t)(gv.paddle_x + gv.paddle_width + 1);
        b->anchor_y = (uint8_t)y;
        b->acc_x = b->acc_y = 1;
        b->bounces = 0;
        paddle_slope(b, SLOPE_SIDE, depth);
        cv.sound_request = SOUND_PADDLE;
        return;
    }

    /* The top. */
    uint32_t left = (gv.paddle_x - 3) & 0xff;
    if (b->x < left)
        return;
    uint32_t off = (b->x - left) & 0xff;

    if (off <= 0x0a) {                          /* the left end */
        paddle_slope(b, SLOPE_TOP, off);
        b->dir_x = 1;                         /* away to the left */
        paddle_bounce_up(b);
        return;
    }
    uint32_t span = (gv.paddle_width + 3) & 0xff;
    if (off > span)
        return;
    uint32_t from_right = (span - off) & 0xff;
    if (from_right <= 0x0a) {                   /* the right end */
        paddle_slope(b, SLOPE_TOP, from_right);
        b->dir_x = 0;                         /* away to the right */
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
void probe_cell_at(uint32_t x, uint32_t y, hit_t *hit)
{
    if (x > 0xbf || y > 0xc4) {
        hit->cell = 0;
        return;
    }
    /* The original spells the row stride as `row + row / 2` with row = y & 0xf8,
     * which is (y >> 3) * 12 - twelve columns of sixteen pixels, fourteen rows
     * of eight. It is an array index. */
    uint8_t *cell = &gv.level.cells[(y >> 3) * 12 + (x >> 4)];
    if (*cell == 0x0c || *cell == 0) {
        hit->cell = 0;
        return;
    }
    /* The slot keeps the cell's *address*, not its index: the brick handlers
     * are handed it and write through it, and 1ac2:27b7 compares slots by it.
     * That 16-bit value is the game's own, so it stays an image offset. */
    hit->cell = (uint16_t)img_off(cell);
    gv.hit_count++;
    hit->x = (uint8_t)((x & 0xf0) + 8);              /* the brick's */
    hit->y = (uint8_t)((y & 0xf8) + 6);              /* centre */
}

/* 1ac2:27b7  drop_duplicate_hits
 *
 * Two corners of the ball can land in the same brick. Later slots naming a
 * centre an earlier one already has are cleared, so the brick is only hit
 * once.
 */
void drop_duplicate_hits(void)
{
    for (int32_t i = 0; i < 3; i++) {
        if (!gv.hits[i].cell)
            continue;
        uint32_t centre = gv.hits[i].centre;
        for (int32_t j = i + 1; j < 4; j++)
            if (gv.hits[j].cell && gv.hits[j].centre == centre)
                gv.hits[j].cell = 0;
    }
}

/* Reverse one axis, anchoring one pixel back the way the ball came. */
static void bounce_x(ball_t *b)
{
    if (b->dir_x == 0) {
        b->dir_x = 1;
        b->anchor_x = (uint8_t)(b->x - 1);
    } else {
        b->dir_x = 0;
        b->anchor_x = (uint8_t)(b->x + 1);
    }
}

static void bounce_y(ball_t *b)
{
    if (b->dir_y == 0) {
        b->dir_y = 1;
        b->anchor_y = (uint8_t)(b->y - 1);
    } else {
        b->dir_y = 0;
        b->anchor_y = (uint8_t)(b->y + 1);
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
void ball_bricks(ball_t *b)
{
    gv.hit_count = 0;

    uint32_t x = (b->x - 8) & 0xff, y = (b->y - 6) & 0xff;
    probe_cell_at(x, y, &gv.hits[0]);
    probe_cell_at((x + 3) & 0xff, y, &gv.hits[1]);
    probe_cell_at((x + 3) & 0xff, (y + 3) & 0xff, &gv.hits[2]);
    probe_cell_at(x, (y + 3) & 0xff, &gv.hits[3]);

    uint32_t n = gv.hit_count;
    if (n == 0)
        return;

    uint32_t s0 = gv.hits[0].cell, s1 = gv.hits[1].cell;
    uint32_t s2 = gv.hits[2].cell, s3 = gv.hits[3].cell;

    /* The second half of that condition reads **[0x2e99]**, which is not slot
     * 3 at [0x2e95] but the first word of the direction table - and that word
     * is a constant zero. So `!s0 && s1` never reaches the both-axes bounce;
     * it always falls through to the x-only one at 1ac2:26b3. Almost
     * certainly a slip in the original - the offset is four too far and the
     * three neighbouring tests do read slots - but it is the original's slip
     * and the ball's path depends on it. Reading s3 here instead sends a ball
     * that clips two corners off in the wrong direction, which took eleven
     * thousand frames of a level 6 game to show up. */
    if (n == 3 || (n == 2 && ((s0 && s2) || (!s0 && s1 && gv.hit_dirs[0])))) {
        bounce_x(b);                    /* wedged, or hit on the diagonal */
        bounce_y(b);
    } else if (n == 2) {
        /* A flat face: the pair tells which axis it was. */
        if (s0 && s1)
            bounce_y(b), b->anchor_x = b->x;
        else if (s2 && s3)
            bounce_y(b), b->anchor_x = b->x;
        else
            bounce_x(b), b->anchor_y = b->y;
    } else {
        /* One corner, or all four: leave in the direction its slot names. */
        int32_t i = 0;
        while (i < 4 && !gv.hits[i].cell)
            i++;
        if (i < 4) {
            uint32_t d = gv.hit_dirs[i];
            b->dir_x = (uint8_t)(d & 0xff);
            b->dir_y = (uint8_t)(d >> 8);
            b->anchor_y = (uint8_t)(b->y + (b->dir_y ? -1 : 1));
            b->anchor_x = (uint8_t)(b->x + (b->dir_x ? -1 : 1));
        }
    }

    /* Three hits means one of them is a corner that should not count. */
    if (n == 3) {
        if (s0) {
            if (s2) {
                gv.hits[1].cell = 0;
                gv.hits[3].cell = 0;
            } else {
                gv.hits[0].cell = 0;
            }
        } else {
            gv.hits[2].cell = 0;
        }
    }

    b->acc_x = b->acc_y = 1;
    drop_duplicate_hits();

    for (int32_t i = 0; i < 4; i++) {
        uint32_t cell = gv.hits[i].cell;
        if (cell)
            brick_hit(&gv.hits[i], cell, b);
    }
}

/* ========================================================================
 * Brick behaviour: the table at 0x3044, indexed by cell value.
 *
 * Value 0 and 13 have no handler, 4 and 12 are indestructible, 5 through 8 are
 * a chain that degrades one step per hit, and 1, 2, 3 and 9 through 11 each do
 * something of their own. They share a preamble - add to the score, ask for a
 * sound, and reset the ball's bounce counter, so that a brick hit does not
 * count towards the every-0x23-bounces slope shuffle.
 * ===================================================================== */
#define SOUND_BRICK      3

static void brick_score(uint32_t a, uint32_t b, uint32_t c)
{
    img_setw(img_off(gv.score_add) + 0, a);
    img_setw(img_off(gv.score_add) + 2, b);
    img_setw(img_off(gv.score_add) + 4, c);
    score_add();                        /* 1ac2:413d */
}

/* The common opening: score, sound, and clear the ball's bounce counter. */
static void brick_common(ball_t *ball, uint32_t sound,
                         uint32_t a, uint32_t b, uint32_t c)
{
    brick_score(a, b, c);
    cv.sound_request = (uint8_t)sound;
    if (ball)
        ball->bounces = 0;
}

/* Attach a fresh entity to the brick that was just hit. `slot` is the hit
 * record: its word is the cell address, the two bytes after it the centre. */
/* The node a broken brick leaves behind. `[si+2]` is deliberately **not**
 * written here: every handler but brick 8 stores the slot pointer there with
 * its own `mov word ptr [si+2], bx`, and brick 8 stores a byte 4 in [si+2]
 * and leaves [si+3] holding whatever the recycled slot had. Writing the word
 * for it put the slot pointer's high byte, 0x2f, where the original had the
 * previous occupant's value - one byte, 62,536 frames in. */
static entity_t *brick_entity(hit_t *hit, uint32_t handler,
                             uint32_t frames, uint32_t rate)
{
    entity_t *e = entity_alloc();
    e->handler = (uint16_t)handler;
    ent_sprite_t *s = &e->p.anim.sprite;
    /* The original stores x and y with one word write from the hit slot; two
     * byte writes are the same thing and say which is which. */
    s->x = hit->x;
    s->y = hit->y;
    s->frame = (uint16_t)frames;
    s->timer = (uint8_t)rate;
    s->period = (uint8_t)rate;
    return e;
}

/* Degrade a brick one step: the cell becomes `next`, the old picture comes off
 * and the new one goes on. Cells 5, 6 and 7 all do exactly this. */
static void brick_degrade(hit_t *hit, uint32_t next,
                          uint32_t old_pic, uint32_t new_pic)
{
    g_image[hit->cell] = (uint8_t)next;
    uint32_t x = hit->x, y = hit->y;
    xor_sprite_16x7(x, y, img_ptr(old_pic));
    xor_sprite_16x7(x, y, img_ptr(new_pic));
}

/* Pick one of the bonus kinds by the cumulative weights at 0x33b1: walk the
 * table until an entry is at least random(0xff) and take that index. */
static uint32_t bonus_kind(void)
{
    uint32_t r = game_random(io_ticks(), 0xff);
    uint32_t i = 0;
    while (gv.bonus_odds[i] < r)
        i++;
    return i;
}

/* 1ac2:28cb  brick 1, and 1ac2:2985  brick 2
 *
 * Both score 20 and both usually hand the brick to a crumbling entity. The
 * other path - taken while fewer than three capsules are out, and then one
 * time in three - removes the brick at once and leaves something behind:
 * brick 1 a score popup (0x3561), brick 2 a falling capsule (0x3273).
 */
static void brick_1_or_2(hit_t *hit, ball_t *ball, int32_t is_two)
{
    brick_common(ball, SOUND_BRICK, 0, 0, 2);

    if (gv.bonus_cap >= 3 || game_random(io_ticks(), 3) != 0) {
        /* crumble, and it keeps the cell it is standing on */
        brick_entity(hit, 0x3b2a, is_two ? 0x6508 : 0x65fe, 7)
            ->p.anim.arg.cell = (uint16_t)hit->cell;
        g_image[hit->cell] = 0;
        gv.level.bricks--;
        return;
    }

    gv.level.bricks--;
    uint32_t cell = hit->cell;
    g_image[cell] = 0;
    uint32_t x = hit->x, y = hit->y;
    xor_sprite_16x7(x, y, img_ptr(is_two ? 0x63a6 : gv.cell_bitmap[1]));

    entity_t *e = entity_alloc();
    e->handler = is_two ? 0x3273 : 0x3561;
    ent_fall_t *f = &e->p.fall;
    /* The original writes x and y with one word and `inc bh` to put it a row
     * lower; the fall arm has them as the two bytes they are. */
    uint32_t centre = hit->centre;
    f->x = (uint8_t)centre;
    f->y = (uint8_t)((centre >> 8) + 1);
    f->kind = (uint8_t)bonus_kind();
    f->tick = 0;
    f->frame = 0;
    f->cycle = 1;
    /* The sprite goes at the brick's centre, one scan line down - `inc bh`
     * before the store at [si+2], and BL untouched. Passing [si+4], the kind
     * that was just picked, as the x instead put it wherever the random
     * number landed. */
    xor_sprite_16xn(centre & 0xff, ((centre >> 8) + 1) & 0xff,
                    is_two ? 0x4e13 : 0x5863, 6);
    gv.bonus_cap++;
}

void brick_1(hit_t *hit, ball_t *ball) { brick_1_or_2(hit, ball, 0); }
void brick_2(hit_t *hit, ball_t *ball) { brick_1_or_2(hit, ball, 1); }

/* 1ac2:2a3f  brick 3 - hardens into a 4, which nothing can break */
void brick_3(hit_t *hit, ball_t *ball)
{
    cv.sound_request = 4;
    if (ball)
        ball->bounces++;
    brick_entity(hit, 0x365e, 0x66f4, 8)->p.anim.arg.cell =
        (uint16_t)hit->cell;
    g_image[hit->cell] = 4;
}

/* 1ac2:3221  bricks 4 and 12 - indestructible; the ball only bounces */
void brick_solid(hit_t *hit, ball_t *ball)
{
    (void)hit;
    cv.sound_request = SOUND_BOUNCE;
    if (ball)
        ball->bounces++;
}

/* 1ac2:2a73, 1ac2:2ab4, 1ac2:2af5  bricks 5, 6, 7 - one step down the
 * chain per hit */
void brick_5(hit_t *hit, ball_t *ball)
{
    brick_common(ball, SOUND_BRICK, 0, 0, 2);
    brick_degrade(hit, 6, 0x6466, 0x6486);
}

void brick_6(hit_t *hit, ball_t *ball)
{
    brick_common(ball, SOUND_BRICK, 0, 0, 3);
    brick_degrade(hit, 7, 0x6486, 0x64a6);
}

void brick_7(hit_t *hit, ball_t *ball)
{
    brick_common(ball, SOUND_BRICK, 0, 0, 5);
    brick_degrade(hit, 8, 0x64a6, 0x64c6);
}

/* 1ac2:2b36  brick 8 - the end of that chain. A hundred points, and it leaves
 * an entity running 0x366f where it was. */
void brick_8(hit_t *hit, ball_t *ball)
{
    brick_common(ball, 4, 0, 0x100, 0);
    g_image[hit->cell] = 0;
    uint32_t x = hit->x, y = hit->y;
    xor_sprite_16x7(x, y, img_ptr(0x64c6));
    xor_sprite_16x7(x, y, img_ptr(0x681c));
    /* four times round the animation - a **byte**, see ent_anim_t's arg */
    brick_entity(hit, 0x366f, 0x67ea, 7)->p.anim.arg.count = 4;
    gv.level.bricks--;
}

/* The dispatch ball_bricks does through the table at 0x3044. */
/* The table at 0x3044 is thirty words long but names only twelve distinct
 * handlers, and sidebyside.py tags the emulator's dispatch by the routine it
 * reaches. So several cell values have to come out as the one value that
 * names their routine, or the two sides disagree over a brick they both got
 * right - which is exactly what frame 206,783 was. */
static uint32_t brick_tag(uint32_t v)
{
    if (v == 12 || (v >= 24 && v <= 29)) return 4;    /* all 0x3221 */
    if (v >= 17 && v <= 21) return 16;                /* all 0x2ccd */
    return v;
}

void brick_hit(hit_t *hit, uint32_t cell, ball_t *ball)
{
    io_log_random(0x8000 | brick_tag(g_image[cell]));  /* for sidebyside */
    switch (g_image[cell]) {
    case 1:  brick_1(hit, ball); break;
    case 2:  brick_2(hit, ball); break;
    case 3:  brick_3(hit, ball); break;
    case 4:
    case 12: brick_solid(hit, ball); break;
    case 5:  brick_5(hit, ball); break;
    case 6:  brick_6(hit, ball); break;
    case 7:  brick_7(hit, ball); break;
    case 8:  brick_8(hit, ball); break;
    case 9:  brick_9(hit, ball); break;
    case 10: brick_10(hit, ball); break;
    case 11: brick_11(hit, ball); break;
    case 16: case 17: case 18:
    case 19: case 20: case 21:
        brick_animated(hit, ball); break;   /* 1ac2:2ccd */
    /* An animated brick that has already been hit carries its cell value
     * plus eight, and the table sends all six of those back to the solid
     * handler: it bounces the ball and nothing more. */
    case 24: case 25: case 26:
    case 27: case 28: case 29:
        brick_solid(hit, ball); break;      /* 1ac2:3221 */
    default: break;                     /* 0, 13-15 and 22-23 have none */
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:3b64  xor_sprite_16x7
 *
 * Seven rows of four bytes, XORed in. A brick's cell is eight scan lines
 * apart but only seven of them are drawn - the eighth is the gap between
 * rows - so this both draws a brick and, run again, rubs it out.
 */
void xor_sprite_16x7(uint32_t x, uint32_t y, const uint8_t *src)
{
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 7; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= src[r * 4 + b];
        di = cga_next_row(di);
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:413d  score_add
 *
 * Add the six-digit figure in score_add to the score in score_text, in decimal,
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
    uint32_t carry = 0;
    for (int32_t i = 5; i >= 0; i--) {          /* least significant first */
        uint32_t sum = (gv.score_text[i] & 0x0f) + gv.score_add[i]
                     + (carry ? 1 : 0);
        uint32_t adjusted = (sum + 6) & 0xff;
        carry = (adjusted & 0xf0) || (sum & 0xf0);
        gv.score_text[i] = (uint8_t)(0x30 | ((carry ? adjusted : sum) & 0x0f));
    }
    /* Redraw the six digits into the panel. */
    uint32_t di = 0x15d2;
    for (int32_t i = 0; i < 6; i++, di += 2)
        draw_char((char)gv.score_text[i], di);

    /* An extra life every time the score reaches gv.extra_at, which then
     * advances by two.
     *
     * Both are ASCII, and the two digits are the score's top two - of six, so
     * they count in ten-thousands. Adding two to the second of them is
     * **twenty thousand** points, and the thresholds run 02, 04, 06, 08, 10,
     * 12 and on: an extra life every 20,000, forever. `inc ax` twice can
     * carry out of '9', so the fix-up puts the digit back to '0' and carries
     * into the one above by hand. */
    uint32_t thresh = gv.extra_at;
    /* The original loads the score's top word and `xchg bl,bh` to put it the
     * way round the threshold is stored. Reading the two digits by name in
     * that order is the same thing without the swap. */
    uint32_t top = (uint32_t)(gv.score_text[0] << 8) | gv.score_text[1];
    if (top >= thresh) {
        thresh += 2;
        if ((thresh & 0xff) >= 0x3a)
            thresh = (thresh & 0xff00) + 0x100 + 0x30;
        gv.extra_at = (uint16_t)thresh;
        extra_life();
    }
}

/* 1ac2:318b  extra_life
 *
 * One more life, up to twelve, and its marker drawn on the panel. The markers
 * are four to a row: `al & 0xfc` steps along and `(al & 3) * 0xf0` steps down.
 */
void extra_life(void)
{
    if (gv.lives == 0x0c)
        return;
    uint32_t n = (gv.lives - 1) & 0xff;
    uint32_t di = 0x3a7c + (n & 0xfc) + (n & 3) * 0xf0;
    for (int32_t r = 0; r < 5; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = gv.life_sprite[r][b];
        /* `sub di, 4` only puts back what `rep movsw` advanced, and what
         * follows is the ordinary step to the next scan line. */
        di = cga_next_row(di);
    }
    gv.lives++;
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
#define BACKDROP_BYTES     48

void field_backdrop(uint32_t y)
{
    io_log_random(0x1fc1);              /* tagged, for sidebyside's per-frame list */
    uint32_t di = cga_at(0, y) + BRICK_LEFT;
    const uint8_t *src = img_ptr(gv.backdrop_table[(gv.backdrop_phase >> 3) & 7]);
    for (int32_t r = 0; r < 8; r++) {
        for (int32_t b = 0; b < BACKDROP_BYTES; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = src[b];
        src += BACKDROP_BYTES;
        di = cga_next_row(di);
    }
    /* `shr al,1` three times looking for a set bit, then `cmp al,4`: the
     * counter resets only when its low three bits are clear and it has
     * reached the last of the eight patterns. */
    uint32_t p = gv.backdrop_phase;
    if ((p & 7) == 0 && (p >> 3) == 4)
        p = 0xff;
    gv.backdrop_phase = (uint8_t)(p + 1);
}

/* ========================================================================
 * The level's opening animation: a creature walks the paddle row carrying
 * the ball on. 1ac2:1c4f drives it, 1ac2:1e23 steps it, 1ac2:1e50 draws one
 * frame.
 * ===================================================================== */
#define WALKER_ROW    0x1cc0            /* the paddle row */
#define WALKER_FIRST  0x7521            /* where the frame list restarts */

/* 1ac2:1e50  walker_draw
 *
 * One frame of the creature, 12 pixels by 7, XORed onto the paddle row. The
 * frame is copied to a work buffer and shifted right `(x & 3) * 2` bits -
 * a pixel per two - across each row of three bytes, since at this depth a
 * byte holds four pixels and there is no pre-shifted copy for this one.
 */
void walker_draw(uint32_t x)
{
    /* walker_anim is a cursor into a list of frame addresses, ended by
     * 0xffff; the word it names is the frame. */
    const uint8_t *frame = img_ptr(img_w(gv.walker_anim));
    memcpy(gv.walker_work, frame, sizeof gv.walker_work);

    for (uint32_t n = (x & 3) * 2; n > 0; n--) {
        for (int32_t r = 0; r < 7; r++) {
            uint8_t *row = gv.walker_work[r];
            uint32_t carry = 0;
            for (int32_t b = 0; b < 3; b++) {
                uint32_t v = row[b];
                row[b] = (uint8_t)((v >> 1) | (carry << 7));
                carry = v & 1;
            }
        }
    }

    uint32_t di = (x >> 2) + WALKER_ROW;
    for (int32_t r = 0; r < 7; r++) {
        g_vram[di & (CGA_SIZE - 1)] ^= gv.walker_work[r][0];
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= gv.walker_work[r][1];
        g_vram[(di + 2) & (CGA_SIZE - 1)] ^= gv.walker_work[r][2];
        di = cga_next_row(di);
    }
}

/* 1ac2:1e23  walker_step
 *
 * Erase the creature where it was - two pixels to the right, with the frame
 * before this one, which is what XOR needs to cancel exactly - then draw it
 * where it is, then advance the animation, wrapping at the 0xffff that ends
 * the frame list.
 */
void walker_step(uint32_t x)
{
    gv.walker_anim = (uint16_t)(gv.walker_anim - 2);
    walker_draw(x + 2);
    gv.walker_anim = (uint16_t)(gv.walker_anim + 2);
    walker_draw(x);
    gv.walker_anim = (uint16_t)(gv.walker_anim + 2);
    if (img_w(gv.walker_anim) == 0xffff)
        gv.walker_anim = (uint16_t)(WALKER_FIRST);
}

/* One strip of the hatch the creature comes out of: 19 rows of one word at a
 * fixed position, from a list of frames. */
static void hatch_frame(const uint8_t *src, uint32_t x, uint32_t y)
{
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 0x13; r++) {
        g_vram[di & (CGA_SIZE - 1)] = src[r * 2];
        g_vram[(di + 1) & (CGA_SIZE - 1)] = src[r * 2 + 1];
        di = cga_next_row(di);
    }
}

/* 1ac2:1c4f  level_draw
 *
 * The hatch opens, the creature walks from x=0xc8 to x=0x6d laying the paddle
 * row down behind it, the hatch closes, and six frames of something play at
 * 0x1cd9. Cosmetic, but it is also what puts the bottom band of the playfield
 * on screen - the backdrop sweep only reaches y=179.
 */
#define LIVES_MARK  0x3a7c

void level_draw(void)
{
    /* The paddle's own hatch is the last of the eight field marks. */
    uint32_t hx = gv.field_marks[7].x, hy = (gv.field_marks[7].y - 1) & 0xff;

    gv.paddle_x = 0xc8;
    for (int32_t f = 0; f < 5; f++) {
        hatch_frame(img_ptr(gv.hatch_open[f]), hx, hy);
        for (int32_t i = 0; i < 0x12c; i++)
            game_delay();
    }

    /* Rub out one life marker: the lives are four to a row, 0xf0 apart -
     * the same layout extra_life draws them in, and the same trap. The
     * `sub di, 4` only puts back what `rep stosw` advanced, and the step
     * that follows is forwards. */
    uint32_t n = (gv.lives - 1) & 0xff;
    uint32_t di = LIVES_MARK + (n & 0xfc) + (n & 3) * 0xf0;
    for (int32_t r = 0; r < 5; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = 0;
        di = cga_next_row(di);
    }

    gv.walker_anim = (uint16_t)(0x7525);
    gv.paddle_x = 0xc6;
    walker_draw(0xc8);
    gv.walker_anim = (uint16_t)(gv.walker_anim + 2);
    for (int32_t i = 0; i < 9; i++) {
        for (int32_t d = 0; d < 0x4b; d++)
            game_delay();
        walker_step(gv.paddle_x);
        gv.paddle_x -= 2;
        io_present();
        if (!io_pump())
            return;
    }

    /* Closing the hatch, one frame every fourth step of the walk. */
    for (int32_t f = 0; f < 0x14; f++) {
        uint32_t ch = (uint32_t)(0x14 - f);
        if (!(ch & 3))
            hatch_frame(img_ptr(gv.hatch_shut[f >> 2]), hx, hy);
        for (int32_t d = 0; d < 0x4b; d++)
            game_delay();
        walker_step(gv.paddle_x);
        gv.paddle_x -= 2;
        io_present();
        if (!io_pump())
            return;
    }
    while (gv.paddle_x >= 0x6d) {
        walker_step(gv.paddle_x);
        gv.paddle_x -= 2;
        for (int32_t d = 0; d < 0x4b; d++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }

    for (int32_t f = 0; f < 6; f++) {
        uint32_t src = gv.walker_drop[f];
        uint32_t d = 0x1cd9;
        for (int32_t r = 0; r < 7; r++) {
            for (int32_t b = 0; b < 7; b++)
                g_vram[(d + b) & (CGA_SIZE - 1)] = g_image[src + r * 7 + b];
            d = cga_next_row(d);
        }
        for (int32_t i = 0; i < 0x147; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }
}

/* ========================================================================
 * 1ac2:0b0b  panel_draw
 *
 * The score panel down the right-hand side: the player's name, the score, and
 * a row of life markers. It is composed in the image first - a picture 28
 * bytes (112 pixels) wide starting at 0x85f0 - and then revealed on screen a
 * row at a time from the bottom up, with a retrace wait for each.
 *
 * The glyph copy is written out again here rather than calling draw_char,
 * because the original does the same: draw_char writes to the framebuffer with
 * the CGA interlace between rows, and this writes to a flat buffer 28 bytes to
 * a row. The character-to-glyph mapping is the same one, minus the cursor.
 * ===================================================================== */
#define PANEL_STRIDE      28
#define PANEL_ON_SCREEN 0x3f24          /* bottom-right, and it grows upwards */
/* `cmp bx, 0x5e` - the reveal runs for bx = 1..0x5d, ninety-three passes.
 * Ninety-two leaves the whole panel one scan line lower than it belongs, and
 * the only place that shows is the life markers: level_draw clears five rows
 * at the absolute address 0x3a7c, the marker is sitting one row below it, and
 * its bottom row of caps survives every clear. */
#define PANEL_ROWS     0x5e

static void panel_char(uint8_t c, uint8_t *dest)
{
    uint32_t g;
    if (c == '-')                       g = 0x0b;
    else if (c <= ' ')                  g = 0;
    else if (c <= '9')                  g = c - 0x2f;
    else if (c >= 'A')                  g = c - 0x35;
    else                                g = 0x0b;
    const uint8_t (*src)[2] = gv.font[g];
    for (int32_t r = 0; r < FONT_ROWS; r++, dest += PANEL_STRIDE) {
        dest[0] = src[r][0];
        dest[1] = src[r][1];
    }
}

void panel_draw(void)
{
    uint8_t *dest = &gv.panel[9][2];
    for (int32_t i = 0; i < 0x0c; i++, dest += 2)
        panel_char(gv.player_name[i], dest);

    dest = &gv.panel[31][14];
    for (int32_t i = 0; i < 6; i++, dest += 2)
        panel_char(gv.score_text[i], dest);

    /* Twelve life markers, four to a row: `al & 0xfc` steps along the row and
     * `(al & 3) * 0xa8` steps down. Ones past the lives left are blanked
     * rather than skipped, so a lost life is rubbed out. */
    for (uint32_t n = 1; n <= 0x0c; n++) {
        uint32_t k = n - 1;
        uint8_t *d = &gv.panel[62][8] + (k & 0xfc) + (k & 3) * 0xa8;
        int32_t lit = n <= gv.lives;
        for (int32_t r = 0; r < 5; r++, d += PANEL_STRIDE) {
            for (int32_t b = 0; b < 4; b++)
                d[b] = lit ? gv.life_sprite[r][b] : 0;
        }
    }

    /* Reveal it. Each pass redraws one more row than the last, from the
     * bottom of the panel upwards, so it wipes on rather than appearing. */
    uint32_t bottom = PANEL_ON_SCREEN;
    for (uint32_t rows = 1; rows != PANEL_ROWS; rows++) {
        uint32_t d = bottom;
        io_wait_retrace();
        for (uint32_t r = 0; r < rows; r++) {
            for (int32_t b = 0; b < PANEL_STRIDE; b++)
                g_vram[(d + b) & (CGA_SIZE - 1)] = gv.panel[r][b];
            d = cga_next_row(d);
        }
        for (int32_t i = 0; i < 0x32; i++)
            game_delay();
        bottom = cga_prev_row(bottom);
        io_present();
        if (!io_pump())
            return;
    }
}

/* ========================================================================
 * Small routines, in address order.
 * ===================================================================== */

/* 1ac2:044b  level_colours
 *
 * Pick up the level's animation script. Each level has four bytes at the start
 * of the block reached as segment 0x14a1: a word that is a pointer into the
 * script, and a byte that is both the rate and the countdown to the next step.
 * The play loop walks it with the same `0xffff means restart` idiom the entity
 * list uses.
 */

void level_colours(void)
{
    const level_anim_t *a = &s14a1.level[gv.level_number];
    gv.anim_ptr = a->script;
    gv.anim_rate = a->rate;
    gv.anim_count = a->rate;
}

/* 1ac2:10c5  draw_run - the same character `count` times.
 *
 * Returns where DI ended up. The original leaves it there for the caller and
 * the screens rely on it: the high-score table spells its heading out as a
 * run of spaces, ten separate `call 0xc64`, and another run, each picking up
 * exactly where the last left off. */
uint32_t draw_run(uint8_t c, uint32_t count, uint32_t di)
{
    for (uint32_t i = 0; i < count; i++, di = (di + 2) & 0xffff)
        draw_char(c, di);
    return di;
}

/* 1ac2:10d1  draw_text - `count` characters from `src`, DI handed back for
 * the same reason as draw_run. `lodsb` advances SI too, so a caller that
 * wants it preserved pushes it - which is what the high-score table does
 * around every run of spaces between its columns. */
uint32_t draw_text(const char *src, uint32_t count, uint32_t di)
{
    for (uint32_t i = 0; i < count; i++, di = (di + 2) & 0xffff)
        draw_char(src[i], di);
    return di;
}

/* 1ac2:14a7  draw_cursor
 *
 * Glyph 0xff, the text-entry cursor, one cell to the right of `di` - and `di`
 * is left where it was, because the caller is still pointing at the character
 * being typed.
 */
void draw_cursor(uint32_t di)
{
    draw_char((char)0xff, di + 2);   /* the cursor, not a character */
}

/* 1ac2:1642  define_keys_prompt
 *
 * Copy a zero-terminated string into the **text-mode** framebuffer: `stosb`
 * then `inc di` steps two bytes, leaving the attribute byte between characters
 * alone. Only the key-definition screen uses it, and only because that screen
 * switches to mode 01h rather than drawing in graphics.
 */
/* 1ac2:1642  define_keys_prompt
 *
 * One line of the redefine-keys screen: the prompt that asks for left, then
 * right, then the one that launches the ball off the paddle, written as text
 * characters with the attribute already in the byte after each. It was called
 * copy_string_text here, which described the instructions and not the job.
 *
 * The screen it belongs to is **deliberately not transcribed** - see
 * screen_define_keys. This is kept because it is complete and it checks out.
 */
void define_keys_prompt(uint32_t src, uint32_t dst)
{
    /* Part of the redefine-keys screen, which is not transcribed. It wrote
     * one prompt - left, right, or the key that launches the ball off the
     * paddle - as text characters with the attribute in the byte after each.
     * It was called copy_string_text, which described the instructions rather
     * than the job it did. */
    (void)src;
    (void)dst;
}

/* 1ac2:3146  flash_bar
 *
 * XOR a pattern across 24 words at 0x3ef2 - the bar along the bottom of the
 * playfield. Called with the pattern in DX, so the same routine both draws it
 * and rubs it out.
 */
void flash_bar(uint32_t pattern)
{
    uint32_t di = 0x3ef2;
    for (int32_t i = 0; i < 0x18; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)pattern;
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= (uint8_t)(pattern >> 8);
    }
}

/* 1ac2:3232  entity_alloc
 *
 * Two lists share one header block: `[0x3138]` is the head of the **free**
 * list and `[0x3144]` - which is `0x3138 + 0x0c`, the header's own link field
 * - is the head of the **active** one. That is why walking from 0x3138 by
 * `+0x0c` lands on the active list: the header is a node whose link is the
 * active head.
 *
 * So this pops the first free node and appends it to the end of the active
 * list. Appending rather than pushing keeps entities in the order they were
 * created, which is the order they are drawn in.
 */

entity_t *entity_alloc(void)
{
    /* Take the first node off the free list. */
    uint32_t si = gv.entity_free;
    gv.entity_free = (uint16_t)(entity_at(si)->next);

    /* Then append it to the end of the **active** list - which is what this
     * walk is over, though it starts at the free list's own head variable.
     * See the note on entity_free: 0x3138 read as a node has its `next` at
     * 0x3144, which *is* entity_head, so the sentinel's link is the list. */
    uint32_t bx = img_off(&gv.entity_head);
    while (entity_at(bx)->next != 0xffff)
        bx = entity_at(bx)->next;
    entity_at(si)->next = (uint16_t)(0xffff);
    entity_at(bx)->next = (uint16_t)(si);
    return entity_at(si);
}

/* 1ac2:3257  entity_unlink
 *
 * Take a node out of the active list and push it back on the free one.
 * [0x3142] is the node before it, which the play loop keeps up to date as it
 * walks - a singly linked list cannot find it otherwise.
 */
void entity_unlink(uint32_t node)
{
    entity_at(gv.entity_prev)->next = entity_at(node)->next;
    entity_at(node)->next = (uint16_t)(gv.entity_free);
    gv.entity_free = (uint16_t)(node);
    gv.entity_remove = 0;
}

/* 1ac2:3668  cell_set_three - the cell an entity is sitting on becomes a 3 */
void cell_set_three(uint32_t node)
{
    g_image[entity_at(node)->p.anim.arg.cell] = 3;
}

/* 1ac2:36fb  cells_restore
 *
 * Put back the [0x2f11] cells listed at 0x2f12, as value 9, and ask to be
 * unlinked. This is how a bonus that hid part of the field gives it back.
 */
void cells_restore(void)
{
    uint32_t n = gv.level.teleports;
    for (uint32_t i = 0; i < n; i++)
        gv.level.cells[gv.level.teleport[i]] = 9;
    gv.entity_remove = 1;
}

/* ========================================================================
 * Entities: the things that are not the ball or the paddle.
 *
 * Every node in the list is the same fourteen bytes, and a handler reads them
 * however it likes, but the shape is consistent enough to name:
 *
 *   +0x00  the handler, rewritten in place to change state
 *   +0x02  a pointer, usually to the cell or ball the entity belongs to
 *   +0x04  x, +0x05  y
 *   +0x06  a cursor into a list of frame pointers, 0xffff at the end
 *   +0x08  ticks until the next frame, +0x09  what to reload it with
 *   +0x0a  a second frame cursor, for handlers that run two animations
 *   +0x0c  the next node
 * ===================================================================== */

/* 1ac2:406a  xor_sprite_20x16 - sixteen rows of five bytes, XORed in */
void xor_sprite_20x16(uint32_t x, uint32_t y, uint32_t src)
{
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 0x10; r++) {
        for (int32_t b = 0; b < 5; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= g_image[src + r * 5 + b];
        di = cga_next_row(di);
    }
}

/* 1ac2:3f4f  sprite_shift_draw
 *
 * The same sprite at any pixel x. The 80 bytes are copied to a work buffer and
 * shifted right `(x & 3) * 2` bits, one pixel at a time, in groups of five -
 * one row - so a pixel leaving one byte enters the next through the carry and
 * nothing crosses a row boundary. The original unrolls all sixteen rows; the
 * loop here is the same operation.
 */

void sprite_shift_draw(uint32_t x, uint32_t y, uint32_t src)
{
    memcpy(gv.sprite_work, g_image + src, sizeof gv.sprite_work);
    for (uint32_t n = (x & 3) * 2; n > 0; n--) {
        for (int32_t r = 0; r < 0x10; r++) {
            uint8_t *row = gv.sprite_work[r];
            uint32_t carry = 0;
            for (int32_t b = 0; b < 5; b++) {
                uint32_t v = row[b];
                row[b] = (uint8_t)((v >> 1) | (carry << 7));
                carry = v & 1;
            }
        }
    }
    xor_sprite_20x16(x, y, img_off(gv.sprite_work));
}

/* Step a two-frame XOR animation: erase the frame before this one, draw this
 * one, advance. Shared by the handlers below, which differ only in which
 * drawing routine they use and what they do when the list ends. */
static int32_t entity_anim(ent_anim_t *a, void (*draw)(uint32_t, uint32_t, uint32_t))
{
    if (--a->sprite.timer != 0)
        return 0;                       /* not time for the next frame yet */
    a->sprite.timer = a->sprite.period;

    /* [bx+6] points *into* a list of frame pointers, so one dereference gets
     * the frame: `[si]` where si is the cursor. Dereferencing twice reads the
     * first word of the frame's pixels as if it were an address. */
    uint32_t cur = a->sprite.frame;
    uint32_t x = a->sprite.x, y = a->sprite.y;
    draw(x, y, img_w(cur - 2));         /* the previous frame, to erase */
    uint32_t next = img_w(cur);
    if (next == 0xffff)
        return -1;                      /* the animation is over */
    draw(x, y, next);
    a->sprite.frame = (uint16_t)(cur + 2);
    return 1;
}

/* 1ac2:3aee  entity_sparkle
 *
 * The flash left where something was hit. When its frames run out it takes one
 * off bonus_pending - the count of deliveries under way, which caps them - and
 * asks to be unlinked.
 */
void entity_sparkle(ent_anim_t *a)
{
    if (entity_anim(a, sprite_shift_draw) < 0) {
        gv.bonus_pending--;
        gv.entity_remove = 1;
    }
}

/* 1ac2:3b2a  entity_crumble
 *
 * A brick coming apart. Same animation, drawn with the 16x7 XOR that bricks
 * use, and it checks for the end of the list *after* advancing rather than
 * before - so it plays its last frame and then goes, where the sparkle stops
 * one frame earlier.
 */
void entity_crumble(ent_anim_t *a)
{
    if (--a->sprite.timer != 0)
        return;
    a->sprite.timer = a->sprite.period;

    uint32_t cur = a->sprite.frame;
    uint32_t x = a->sprite.x, y = a->sprite.y;
    xor_sprite_16x7(x, y, img_ptr(img_w(cur - 2)));
    xor_sprite_16x7(x, y, img_ptr(img_w(cur)));
    a->sprite.frame = (uint16_t)(cur + 2);
    if (img_w(a->sprite.frame) == 0xffff)
        gv.entity_remove = 1;
}

/* 1ac2:39a1  bonus_release
 *
 * Let a capsule go from the hatch: allocate an entity running 0x39fa, give it
 * a random fall speed (`random(0x3c) + 9`) and one of eight kinds from the
 * table at 0xac60, and put it where the hatch is. A kind of 0 means it starts
 * eight pixels left and is marked type 2.
 */

void bonus_release(const ent_hatch_t *h)
{
    gv.bonus_live++;
    entity_t *e = entity_alloc();
    e->handler = 0x39fa;
    ent_anim_t *b = &e->p.anim;
    b->arg.move.mode = 0;
    b->arg.move.steps = (uint8_t)(game_random(io_ticks(), 0x3c) + 9);

    const bonus_kind_t *kind = &gv.bonus_kinds[game_random(io_ticks(), 8)];
    b->sprite.frame = kind->frame;
    b->sprite.timer = kind->timer;       /* one word in the original */
    b->sprite.period = kind->period;

    uint32_t al = h->x;
    if (al) {
        al = (al - 8) & 0xff;
        b->arg.move.mode = 2;
    }
    b->sprite.x = (uint8_t)al;
    b->sprite.y = h->y;
    xor_sprite_20x16(b->sprite.x, b->sprite.y,
                     img_w(b->sprite.frame));
}

/* 1ac2:390d  entity_hatch
 *
 * The hatch at the top that lets capsules out. It only runs while no extra
 * ball is in play, waits out [bx+6], and then steps its animation every 0x23
 * ticks - `div cl` and a test of the remainder, which is a modulo written the
 * only way an 8086 has. Frame 0x635c is the one where the capsule appears, and
 * that is where it calls bonus_release.
 */
void entity_hatch(ent_hatch_t *h)
{
    /* `jne 0x390c` is the **ret**, not the `dec word [bx+6]` at 0x3909 that
     * sits just above it: while an extra ball is in play the hatch does
     * nothing at all. Decrementing here walks a counter that is already 0
     * round to 0xffff, which is what diverged 5,872 frames into level 4. */
    if (gv.extra_on != 0)
        return;
    if (h->wait != 0) {
        h->wait--;
        return;
    }
    h->phase--;
    if (h->phase % 0x23 != 0)
        return;

    uint32_t si = img_w(h->script);
    uint32_t di = cga_at(h->x, (h->y - 0x0a) & 0xff);
    for (int32_t r = 0; r < 0x25; r++) {
        g_vram[di & (CGA_SIZE - 1)] = g_image[si + r * 2];
        g_vram[(di + 1) & (CGA_SIZE - 1)] = g_image[si + r * 2 + 1];
        di = cga_next_row(di);
    }
    if (si == 0x635c) {
        h->wait = 0x12c;
        bonus_release(h);
    }
    h->script += 2;
    if (img_w(h->script) == 0xffff) {
        g_image[h->cell + 3] = 0;
        gv.entity_remove = 1;
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:3c66, 1ac2:3cf3, 1ac2:3caf, 1ac2:3d3c  bonus_move_*
 *
 * A falling capsule tries to step one pixel; each direction has its own
 * routine and the table at 0x3447 picks one by [bx+2]. They all do the same
 * thing: work out which cell the capsule would move into, refuse if anything
 * is there, and otherwise take the step. Returning "blocked" is the original's
 * carry, and bonus_steer picks a new direction when it comes back set.
 *
 * The cell index is the same arithmetic as everywhere else - `(y >> 3) * 12 +
 * (x >> 4)` - written as `(al & 0xf8) + ((al & 0xf8) >> 1)`, and the extra
 * cells each one checks (`+0x0c` one row down, `+1` one column right) are the
 * rest of the capsule's footprint.
 */
static uint32_t cell_index(uint32_t y, uint32_t x)
{
    uint32_t row = y & 0xf8;
    return img_off(gv.level.cells) + row + (row >> 1) + ((x >> 4) & 0x0f);
}

/* 1ac2:3c66  bonus_move_right */
int32_t bonus_move_right(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    (void)b;
    uint32_t y = *py, x = *px;
    if (x >= 0xb8)
        return 0;
    uint32_t di = cell_index((y - 6) & 0xff, (x + 8) & 0xff);
    if (g_image[di] || g_image[di + 0x0c])
        return 0;
    if ((((y - 6) & 7) != 0) && g_image[di + 0x18])
        return 0;
    (*px)++;
    return 1;
}

/* 1ac2:3cf3  bonus_move_left */
int32_t bonus_move_left(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    (void)b;
    uint32_t y = *py, x = *px;
    if (x <= 8)
        return 0;
    uint32_t di = cell_index((y - 6) & 0xff, (x - 9) & 0xff);
    if (g_image[di] || g_image[di + 0x0c])
        return 0;
    if ((((y - 6) & 7) != 0) && g_image[di + 0x18])
        return 0;
    (*px)--;
    return 1;
}

/* 1ac2:3caf  bonus_move_up */
int32_t bonus_move_up(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    (void)b;
    uint32_t y = *py, x = *px;
    if (y <= 6)
        return 0;
    uint32_t di = cell_index((y - 7) & 0xff, (x - 8) & 0xff);
    if (g_image[di])
        return 0;
    if ((((x - 8) & 0x0f) != 0) && g_image[di + 1])
        return 0;
    (*py)--;
    return 1;
}

/* Down is the odd one of the four. It looks **ten** pixels below the capsule,
 * not two - `add al, 0xa` - so the cell it tests is the row under its whole
 * sixteen-pixel body rather than the one it is standing in. And it has an
 * ending the others do not: at y 0x78 the capsule has reached the paddle row,
 * and instead of being blocked it is handed to movement kind 4, the script at
 * 0x8320, with its current x left in [bx+3] for the script to steer from.
 *
 * Missing both is what diverged frame 1310 of sidebyside.py: with `+2` the
 * cell tested is a whole row out, so a capsule at the left wall stepped down
 * in the port where the original found a brick and picked a new direction -
 * two random() draws that never happened, and every draw after that offset.
 */
/* 1ac2:3d3c  bonus_move_down */
int32_t bonus_move_down(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    uint32_t y = *py, x = *px;

    if (y >= 0x78) {                    /* 1ac2:3d80 */
        b->arg.move.mode = 4;            /* follow a script from here on */
        b->script = 0x8320;
        b->arg.move.steps = (uint8_t)x;
        (*py)++;
        return 1;
    }

    uint32_t di = cell_index((y + 0x0a) & 0xff, (x - 8) & 0xff);
    if (g_image[di])
        return 0;
    if ((((x - 8) & 0x0f) != 0) && g_image[di + 1])
        return 0;
    (*py)++;
    return 1;
}

/* 1ac2:3bf7  bonus_steer
 *
 * Keep going the way it was going until it is blocked or its timer runs out,
 * then pick a new direction with `random(4)` and a new duration with
 * `random(0x3d)`. Direction 1 gets 0xff instead - it runs until something
 * stops it. Direction 4 is not random at all: it follows a script of steps at
 * [bx+0x0a], which is how a capsule homes in.
 */

int32_t bonus_steer(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    if (b->arg.move.mode == 4)
        return bonus_script(b, px, py);

    if (--b->arg.move.steps != 0) {
        int32_t moved;
        switch (b->arg.move.mode) {
        case 0:  moved = bonus_move_right(b, px, py); break;
        case 1:  moved = bonus_move_down(b, px, py);  break;
        case 2:  moved = bonus_move_left(b, px, py);  break;
        case 3:  moved = bonus_move_up(b, px, py);    break;
        default: moved = 0;                        break;
        }
        if (moved)
            return 1;
    }
    b->arg.move.mode = (uint8_t)game_random(io_ticks(), 4);
    if (b->arg.move.mode == 1) {
        b->arg.move.steps = 0xff;
        return 1;
    }
    b->arg.move.steps = (uint8_t)game_random(io_ticks(), 0x3d);
    return 1;
}

/* 1ac2:40f2  xor_sprite_16xn - like 0x3b64 but the caller says how many rows */
void xor_sprite_16xn(uint32_t x, uint32_t y, uint32_t src, uint32_t rows)
{
    uint32_t di = cga_at(x, y);
    for (uint32_t r = 0; r < rows; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= g_image[src + r * 4 + b];
        di = cga_next_row(di);
    }
}

/* ========================================================================
 * The entities bricks leave behind.
 *
 * Most are entity_crumble with a tail: run the animation, and when it asks to
 * be unlinked do something instead of, or as well as, going away.
 * ===================================================================== */

/* 1ac2:365e  from brick 3 - when the animation ends the cell becomes a 3
 * again, so a hardened brick softens back. */
void entity_soften(ent_anim_t *a)
{
    entity_crumble(a);
    if (gv.entity_remove == 1)
        g_image[a->arg.cell] = 3;  /* the cell it sat on */
}

/* 1ac2:366f  from brick 8 - plays its animation [bx+2] times over, cancelling
 * its own removal each time round, and rubs the last frame out at the end. */
void entity_repeat(ent_anim_t *a)
{
    entity_crumble(a);
    if (gv.entity_remove != 1)
        return;
    if (--a->arg.count != 0) {   /* a **byte**: 1ac2:3679 */
        gv.entity_remove = 0;
        a->sprite.frame = 0x67ea;       /* and round the animation again */
        return;
    }
    xor_sprite_16x7(a->sprite.x, a->sprite.y, img_ptr(0x681c));
}

/* 1ac2:3696  from brick 9 - the animation and nothing else */
void entity_plain(ent_anim_t *a)
{
    entity_crumble(a);
}

/* Put a ball down at a point and set it going upwards: position, anchor and
 * both accumulators, a fresh sprite, and draw it. Three handlers do exactly
 * this and only the offsets they add differ. */
void ball_place(ball_t *ball, uint32_t x, uint32_t y)
{
    ball_t *b = ball;
    b->x = b->prev_x = b->anchor_x = (uint8_t)x;
    b->y = b->prev_y = b->anchor_y = (uint8_t)y;
    b->acc_x = b->acc_y = 0;
    b->state = 1;
    b->dir_y = 1;                     /* set off upwards */
    memcpy(b->sprite, gv.ball_start_sprite, sizeof b->sprite);
    ball_draw(b->sprite, b->x, b->y);
}

/* 1ac2:36a1  from brick 9 - where the ball comes back
 *
 * When the arrival animation finishes it puts the ball down at this entity's
 * position, eight pixels right and four up, gives it a fresh sprite, and draws
 * it. [bx+2] is the ball, not a cell, for this one.
 */
void entity_ball_arrive(ent_anim_t *a)
{
    entity_crumble(a);
    if (gv.entity_remove != 1)
        return;

    ball_place(ball_at(a->arg.ball), (a->sprite.x + 8) & 0xff,
               (a->sprite.y - 4) & 0xff);
}

/* 1ac2:36f6  from brick 9 - counts [bx+4] down and then puts the cells back */
void entity_cells_timer(ent_cells_t *c)
{
    if (--c->left == 0)
        cells_restore();
}

/* ------------------------------------------------------------------------
 * 1ac2:2b9d  brick 9 - takes the ball away and puts it back somewhere else
 *
 * Twenty-five points. The cells listed at 0x2f12 all become 4s so nothing can
 * be broken while the ball is gone, the ball is erased and parked in state 3,
 * and three entities are set going: the animation where it left, a timer that
 * restores the cells, and the arrival animation at a cell picked at random -
 * any but this one.
 *
 * The pixel position of that cell comes out of `div cl` with cl = 12: the
 * quotient is the row and the remainder the column, so x = column * 16 + 8 and
 * y = row * 8 + 6, which is the same grid as everywhere else read backwards.
 */
void brick_9(hit_t *hit, ball_t *ball)
{
    if (!ball)
        return;
    brick_score(0, 0, 0x0502);

    uint32_t n = gv.level.teleports;
    for (uint32_t i = 0; i < n; i++)
        gv.level.cells[gv.level.teleport[i]] = 4;

    ball_t *b = ball;
    b->state = 3;
    b->bounces = 0;
    ball_draw(b->sprite, b->x, b->y);

    brick_entity(hit, 0x3696, 0x6abe, 0x32)->p.anim.arg.cell =
        (uint16_t)hit->cell;

    /* A cell that is not this one. */
    uint32_t cell, idx;
    do {
        idx = gv.level.teleport[game_random(io_ticks(), n)];
        cell = img_off(gv.level.cells) + idx;
    } while (cell == hit->cell);

    entity_t *timer = entity_alloc();
    timer->handler = 0x36f6;
    timer->p.cells.left = 0x514;

    entity_t *e = entity_alloc();
    e->handler = 0x36a1;
    ent_anim_t *a = &e->p.anim;
    a->arg.ball = img_off(ball);        /* the slot holds its address */
    a->sprite.frame = 0x6ad0;
    a->sprite.timer = a->sprite.period = 0x32;
    a->sprite.x = (uint8_t)((idx % 12) * 16 + 8);
    a->sprite.y = (uint8_t)((idx / 12) * 8 + 6);
}

/* 1ac2:2c59  brick 10 - fifty points, and the ball goes into state 4 while an
 * entity runs at where the brick was. */
void brick_10(hit_t *hit, ball_t *ball)
{
    brick_common(ball, SOUND_BRICK, 0, 0, 5);
    g_image[hit->cell] = 0;
    gv.level.bricks--;
    uint32_t x = hit->x, y = hit->y;
    xor_sprite_16x7(x, y, img_ptr(0x63e6));
    if (!ball)
        return;

    entity_t *e = entity_alloc();
    e->handler = 0x37e0;
    ent_anim_t *a = &e->p.anim;
    a->arg.ball = img_off(ball);        /* likewise */
    a->sprite.frame = 0x6b88;
    a->sprite.x = (uint8_t)x;
    a->sprite.y = (uint8_t)y;
    a->sprite.timer = a->sprite.period = 0x69;
    sprite_shift_draw(x, y, 0x6b9c);

    ball_t *b = ball;
    b->state = 4;
    ball_draw(b->sprite, b->x, b->y);
}

/* ------------------------------------------------------------------------
 * The `call word ptr [bx]` at 1ac2:1b5e.
 *
 * An entity's kind *is* its handler, and handlers install each other, so this
 * is the whole type system. Anything not transcribed yet is dropped rather
 * than run, which leaves it stuck in the list - so it says so once.
 */
/* The handler word chooses the arm as well as the routine, so this is the one
 * place that knows which is which - each handler is handed the arm it owns and
 * cannot reach the others. The four that rewrite `handler` to become a
 * different kind of entity, or that pass the node to a helper, take the node
 * instead; they are the ones that legitimately need more than their arm. */
void entity_call(entity_t *e)
{
    switch (e->handler) {
    case 0x3273: entity_capsule(&e->p.fall); break;
    case 0x3386: entity_paddle_fx(&e->p.morph); break;
    case 0x3561: entity_popup(&e->p.fall); break;
    case 0x365E: entity_soften(&e->p.anim); break;
    case 0x366F: entity_repeat(&e->p.anim); break;
    case 0x3696: entity_plain(&e->p.anim); break;
    case 0x36A1: entity_ball_arrive(&e->p.anim); break;
    case 0x36F6: entity_cells_timer(&e->p.cells); break;
    case 0x37E0: entity_ball_hold(&e->p.anim); break;
    case 0x390D: entity_hatch(&e->p.hatch); break;
    case 0x39FA: entity_bonus(&e->p.anim); break;
    case 0x3ABF: entity_anim_brick(&e->p.brick); break;
    case 0x3AEE: entity_sparkle(&e->p.anim); break;
    case 0x3717: entity_multiball(); break;
    case 0x3B2A: entity_crumble(&e->p.anim); break;
    default:     entity_unknown(img_off(e)); break;
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:37e0  entity_ball_hold
 *
 * Brick 10 catches the ball. This carries it down the screen a pixel at a time
 * until y reaches 0xb8, then lets it go - upwards if the safety net is up,
 * and otherwise the ball is simply lost. If something hits the carrier on the
 * way down ([0x33d4] non-zero) it releases early, and scores 33 or 50
 * depending on what hit it.
 */
void entity_ball_hold(ent_anim_t *a)
{
    uint32_t y = a->sprite.y, x = a->sprite.x;

    /* `inc al` is inside the kind-1 branch, and the fall-through at 0x37f7
     * carries whatever AL holds - so a carrier of any other kind is updated
     * at the y it already had, not one lower. */
    uint32_t ny = y;
    if ((a->sprite.timer & 0x0f) == 1)
        ny = (y + 1) & 0xff;

    if ((a->sprite.timer & 0x0f) == 1 && ny == 0xb8) {
        /* It has arrived at the bottom. */
        sprite_shift_draw(x, y, img_w(a->sprite.frame));
        if (gv.net_on == 1) {
            gv.entity_remove = 1;
            ball_place(ball_at(a->arg.ball), (x + 8) & 0xff, (y + 0x0b) & 0xff);
            return;
        }
        gv.ball_alive--;
        ball_at(a->arg.ball)->state = 0;
        gv.entity_remove = 1;
        return;
    }

    bonus_update(&a->sprite, x, ny);            /* 1ac2:3df1 */
    if (gv.hit_kind == 0)
        return;
    if (gv.hit_kind == 2)
        return;                         /* bounced: nothing more to do */

    /* Hit: let the ball go, and score for it unless the hit was type 1. */
    gv.entity_remove = 1;
    uint32_t ry = a->sprite.y;      /* 1ac2:3897 reloads it */
    if (gv.hit_kind != 1) {
        brick_score(0, 0, 0x0303);
        ry = (ry + 4) & 0xff;
    }
    ball_place(ball_at(a->arg.ball), (a->sprite.x + 8) & 0xff, (ry + 0x0c) & 0xff);
    if (gv.hit_kind != 3)
        brick_score(0, 0, 5);
}

/* ========================================================================
 * The paddle's laser, and what a falling capsule collides with.
 * ===================================================================== */

/* 1ac2:30dd  pixel_xor
 *
 * One two-pixel dot. The mask is 0xc0 - the leftmost pixel of a byte - shifted
 * right `(x & 3) * 2` bits to the pixel wanted. Returns the framebuffer offset
 * it used, because 1ac2:306b carries on from there down the next two rows.
 */
uint32_t pixel_xor(uint32_t x, uint32_t y)
{
    uint32_t di = cga_at(x, y);
    uint32_t mask = 0xc0 >> ((x & 3) * 2);
    g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)mask;
    return di;
}

/* 1ac2:306b  shot_xor
 *
 * The laser: two dots three scan lines tall, 0x13 pixels apart - one under
 * each end of the paddle. Drawing it twice rubs it out, and it leaves
 * [0x2e7e] at 1 to say a shot is on its way.
 */
void shot_xor(uint32_t x, uint32_t y)
{
    for (int32_t side = 0; side < 2; side++) {
        uint32_t sx = side ? (x + 0x13) & 0xff : x;
        uint32_t mask = 0xc0 >> ((sx & 3) * 2);
        uint32_t di = pixel_xor(sx, y);
        for (int32_t r = 0; r < 2; r++) {
            di = cga_next_row(di);
            g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)mask;
        }
    }
    gv.laser_on = 1;
}

/* 1ac2:3f20  bonus_hits_ball
 *
 * Do a capsule's sixteen-pixel box and a ball's four overlap? Sets [0x33d4] to
 * 2 if so, which is the answer bonus_update passes back up.
 */
void bonus_hits_ball(const ent_sprite_t *s, const ball_t *ball)
{
    uint32_t by = s->y, ballY = ball->y;
    if (by > ((ballY + 3) & 0xff))
        return;
    if (((by + 0x0f) & 0xff) < ballY)
        return;
    uint32_t bxx = s->x, ballX = ball->x;
    if (((bxx + 0x0f) & 0xff) < ballX)
        return;
    if (bxx > ((ballX + 3) & 0xff))
        return;
    gv.hit_kind = 2;
}

/* ------------------------------------------------------------------------
 * 1ac2:3df1  bonus_update
 *
 * Animate a falling capsule and see what it has run into, leaving the answer
 * in [0x33d4]: 0 nothing, 1 the paddle caught it, 2 a ball hit it, 3 the laser
 * shot it.
 *
 * The animation timer lives in one byte with two counters in it: the low
 * nibble paces the movement and the high nibble the frame, which is why it is
 * masked apart rather than simply decremented.
 */

/* Which ball the collision found - the original's DI across 3df1/3f20. */
static int32_t g_hit_ball;              /* an index: 0 was the original's default */

void bonus_update(ent_sprite_t *s, uint32_t nx, uint32_t ny)
{
    gv.hit_kind = 0;

    if ((--s->timer & 0x0f) == 0) {
        s->timer--;
        s->timer = (uint8_t)((s->timer & 0xf0) |
                                     (s->period & 0x0f));
        /* Erase where the node still says it is - the move so far has only
         * happened in registers - then commit the new position and draw
         * there. Moving the node first and erasing after leaves the old
         * sprite on screen, which is what it did before this was read
         * properly. */
        sprite_shift_draw(s->x, s->y,
                          img_w(s->frame));
        s->x = (uint8_t)nx;
        s->y = (uint8_t)ny;
        uint32_t x = nx, y = ny;
        if ((s->timer >> 4) == 0) {
            s->timer = s->period;
            s->frame += 2;
            if (img_w(s->frame) == 0xffff)
                s->frame = (uint16_t)img_w(s->frame + 2);
        }
        sprite_shift_draw(x, y, img_w(s->frame));       /* draw */
    }

    /* The laser shot, if one is in flight. */
    if (gv.laser_on == 2) {
        uint32_t sy = (gv.laser_y + 2) & 0xff;
        uint32_t by = s->y;
        if (((sy + 1) & 0xff) >= by && sy <= ((by + 0x0f) & 0xff)) {
            uint32_t sx = gv.laser_x, bxx = s->x;
            int32_t hit = (sx >= bxx && sx <= ((bxx + 0x0f) & 0xff)) ||
                      (((sx + 0x13) & 0xff) >= bxx &&
                       ((sx + 0x13) & 0xff) <= ((bxx + 0x0f) & 0xff));
            if (hit) {
                gv.hit_kind = 3;
                sprite_shift_draw(s->x, s->y, img_w(s->frame));
                shot_xor(gv.laser_x, (gv.laser_y + 2) & 0xff);
                gv.laser_y = 0xb3;
                return;
            }
        }
    }

    /* The paddle. */
    uint32_t y = s->y;
    if (y <= 0xbe && ((y + 0x0f) & 0xff) >= 0xb8) {
        uint32_t bxx = s->x, px = gv.paddle_x;
        if (((bxx + 0x0f) & 0xff) >= px &&
            bxx <= ((px + gv.paddle_width) & 0xff)) {
            gv.hit_kind = 1;
            sprite_shift_draw(bxx, y, img_w(s->frame));
            return;
        }
    }

    /* Any ball in play. Which one matched matters: the original leaves it in
     * DI and entity_bonus bounces *that* ball, not the first one it can find
     * afterwards. */
    for (int32_t i = 0; i < 3; i++) {
        ball_t *ball = &gv.balls[i];
        if (ball->state != 1)
            continue;
        bonus_hits_ball(s, ball);
        if (gv.hit_kind == 2) {
            g_hit_ball = i;
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * 1ac2:39fa  entity_bonus
 *
 * A capsule on its way down. Steer it, see what it hit, and react.
 *
 * The control flow is the part to get right, and three things about it are
 * easy to get wrong:
 *
 *   - `cmp ah,0xff / je 0x3a52` on a steer that did not move jumps to the
 *     **draw**, not past the collision test. It skips bonus_update, so the
 *     [0x33d4] tested afterwards is whatever the last call left there.
 *   - the ball that bounces is the one bonus_update found, which the original
 *     carries in DI. Picking the first ball in play instead bounces the wrong
 *     one whenever more than one is out.
 *   - bouncing a ball does **not** end the routine. It falls through the draw
 *     into 0x3a60, where [0x33d4] is 2 and so not zero, and the capsule is
 *     consumed exactly as if the paddle had caught it: sound, sparkle, score.
 *
 * That last one is why the sound request diverged. cs:[0xf4] = 6 is raised
 * here, and a port that returned early after the bounce never raised it.
 */
void entity_bonus(ent_anim_t *b)
{
    uint32_t x = b->sprite.x, y = b->sprite.y;
    int32_t draw = 1;

    /* The timer byte carries two counters: the low nibble paces the movement
     * and the high nibble the frame. */
    if (gv.extra_on != 1 && (b->sprite.timer & 0x0f) == 1) {
        if (!bonus_steer(b, &x, &y))
            goto sprite;                /* 1ac2:3a52, the draw */
    }
    bonus_update(&b->sprite, x, y);             /* 1ac2:3df1 */

    if (gv.hit_kind == 0)
        return;                         /* 1ac2:3a24 */
    if (gv.hit_kind != 2) {
        draw = 0;                       /* 1ac2:3a60, no draw on the way */
        goto settle;
    }

    {   /* A ball: fresh slope, re-anchor, and reverse both ways. */
        ball_t *b = &gv.balls[g_hit_ball];
        b->dy = (uint8_t)(game_random(io_ticks(), 7) + 1);
        b->dx = (uint8_t)(game_random(io_ticks(), 7) + 1);
        b->anchor_x = b->x;
        b->anchor_y = b->y;
        b->acc_x = b->acc_y = 0;
        b->dir_x ^= 1;
        b->dir_y ^= 1;
    }

sprite:
    if (draw)
        sprite_shift_draw(b->sprite.x, b->sprite.y,
                          img_w(b->sprite.frame));

settle:
    if (gv.hit_kind == 0) {       /* 1ac2:3aaa - it reached the bottom */
        if (gv.net_on != 1) {
            gv.entity_remove = 1;
            gv.bonus_pending--;
            gv.bonus_live--;
            return;
        }
        /* the net catches it, so it counts as collected */
    }

    cv.sound_request = 6;         /* 1ac2:3a67 */
    /* Collected: the node becomes a sparkle where it stands. The arm is the
     * sparkle's too, unchanged - only `handler` is the node's, so this is the
     * one line here that has to look outside the capsule. */
    entity_of(b)->handler = 0x3aee;
    b->sprite.frame = 0xb7a4;
    b->sprite.timer = b->sprite.period = 0x0f;
    gv.bonus_live--;
    sprite_shift_draw(b->sprite.x, b->sprite.y,
                      img_w(b->sprite.frame - 2));   /* now a sparkle */
    brick_score(0, 0, 0x0703);
}

/* ========================================================================
 * Scrolling and the paddle's other draw path.
 * ===================================================================== */

/* 1ac2:2109  scroll_up_band and 1ac2:2148  scroll_down_band
 *
 * Move a 48-byte by 27-row band one scan line, in place. Used by the level
 * intro to slide the field about. Scrolling up reads from the row below and
 * writes upwards; scrolling down does the reverse, and both walk the interlace
 * rather than a linear buffer.
 */
void scroll_up_band(void)
{
    uint32_t di = 0x1ae2;
    for (int32_t r = 0x1b; r > 0; r--) {
        uint32_t si = cga_next_row(di);
        for (int32_t b = 0; b < 48; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] =
                g_vram[(si + b) & (CGA_SIZE - 1)];
        di = si;
    }
}

void scroll_down_band(void)
{
    uint32_t si = 0x1ef2;
    for (int32_t r = 0x1b; r > 0; r--) {
        uint32_t di = cga_next_row(si);
        for (int32_t b = 0; b < 48; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] =
                g_vram[(si + b) & (CGA_SIZE - 1)];
        si = cga_prev_row(si);
    }
}

/* 1ac2:22a9  draw_paddle_raw
 *
 * Sixteen rows of seven bytes straight onto the paddle row, no XOR and no
 * shift - it overwrites. The game-over sequence uses it to put the paddle's
 * remains down.
 */
void draw_paddle_raw(const uint8_t *src)
{
    uint32_t di = (gv.paddle_x >> 2) + PADDLE_ROW_BASE;
    for (int32_t r = 0; r < 0x10; r++) {
        for (int32_t b = 0; b < 7; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = src[r * 7 + b];
        di = cga_next_row(di);
    }
}

/* 1ac2:2187  draw_paddle_shifted
 *
 * draw_paddle for a sprite that has no pre-shifted copies: the bytes are
 * shifted here instead, `(x & 3) * 2` bits across each row of eleven. Same
 * erase-then-draw and the same early-out, but it charges the frame delay
 * 0x1f3 rather than 0x1e0 - a shift costs more than picking a copy.
 */
void draw_paddle_shifted(const uint8_t *sprite)
{
    if (!gv.paddle_morphing &&
        gv.paddle_x == gv.paddle_prev_x)
        return;
    gv.frame_delay -= 0x1f3;          /* the uint16_t is the `& 0xffff` */

    gv.paddle_rows[1] = gv.paddle_rows[0];
    memcpy(gv.paddle_pix[1], gv.paddle_pix[0],
           PADDLE_IMAGE + 1);

    uint32_t x = gv.paddle_x;
    gv.paddle_prev_x = (uint8_t)x;
    paddle_row_offsets(x, &gv.paddle_rows[0]);
    memcpy(gv.paddle_pix[0], sprite, PADDLE_IMAGE + 1);

    for (uint32_t n = (x & 3) * 2; n > 0; n--) {
        for (int32_t r = 0; r < PADDLE_ROWS; r++) {
            uint8_t *row = &gv.paddle_pix[0][r * PADDLE_BYTES];
            uint32_t carry = 0;
            for (int32_t b = 0; b < PADDLE_BYTES; b++) {
                uint32_t v = row[b];
                row[b] = (uint8_t)((v >> 1) | (carry << 7));
                carry = v & 1;
            }
        }
    }

    blit_xor(gv.paddle_pix[1], &gv.paddle_rows[1]);
    blit_xor(gv.paddle_pix[0], &gv.paddle_rows[0]);
}

/* ========================================================================
 * Clearing the entity list, at the end of a level or a life.
 *
 * Two entities have to be told before they go: 0x36f6 puts the cells it hid
 * back, and 0x365e softens the brick it hardened. Everything else is simply
 * moved to the free list.
 * ===================================================================== */

/* Tell a node that is about to be discarded, if it is one of the two that
 * care. Shared by all three purges below. */
static void entity_farewell(uint32_t bx)
{
    uint32_t handler = img_w(bx);
    if (handler == 0x36f6)
        cells_restore();
    else if (handler == 0x365e)
        cell_set_three(bx);
}

/* 1ac2:055e  entities_clear - empty the active list onto the free one */
void entities_clear(void)
{
    uint32_t bx = gv.entity_head.next;
    while (bx != 0xffff) {
        entity_farewell(bx);
        uint32_t next = entity_at(bx)->next;
        entity_at(bx)->next = (uint16_t)(gv.entity_free);
        gv.entity_free = (uint16_t)(bx);
        bx = next;
    }
    gv.entity_head.next = (uint16_t)(0xffff);
}

/* 1ac2:0521  screen_level_done - the same, and then the between-levels
 * sequence at 0x5f8 */
void screen_level_done(void)
{
    entities_clear();
    level_between();                    /* 1ac2:05f8 */
}

/* 1ac2:0735  life_lost
 *
 * Reload the level's animation script and clear the list - but **keep** any
 * entity running 0x3abf, which is why this one tracks the previous node in
 * [0x3142] where the other two do not: skipping a node means unlinking around
 * it.
 */
void life_lost(void)
{
    level_colours();
    gv.entity_prev = img_off(&gv.entity_head);
    uint32_t bx = gv.entity_head.next;
    while (bx != 0xffff) {
        if (img_w(bx) == 0x3abf) {      /* this one stays */
            gv.entity_prev = (uint16_t)(bx);
            bx = entity_at(bx)->next;
            continue;
        }
        entity_farewell(bx);
        uint32_t next = entity_at(bx)->next;
        entity_at(bx)->next = (uint16_t)(gv.entity_free);
        gv.entity_free = (uint16_t)(bx);
        entity_at(gv.entity_prev)->next = (uint16_t)next;
        bx = next;
    }
    level_between();
}

/* ========================================================================
 * 1ac2:2e1e  ball_on_paddle
 *
 * The catch bonus: the ball sticks to the paddle instead of bouncing, rides
 * along with it, and goes when the action key is pressed or the timer at
 * [0x2e76] runs out. Returns 1 to mean "carry on and step this ball" - the
 * original's carry - and 0 to mean the ball is held and the play loop should
 * leave it alone this frame.
 *
 * [0x2e56] is the offset along the paddle where it landed, so it stays at the
 * same point as the paddle moves rather than snapping to the middle.
 * ===================================================================== */
#define HOLD_RESET   0x230
#define SOUND_CATCH      7

int32_t ball_on_paddle(ball_t *b)
{
    if (gv.paddle_morphing != 0)
        return 1;

    if (gv.hold_timer == HOLD_RESET) {
        /* Not holding one yet: is this ball landing on the paddle? */
        uint32_t y = b->y;
        uint32_t left = (gv.paddle_x - 3) & 0xff;
        uint32_t off = (b->x - left) & 0xff;
        if (y < PADDLE_TOP || y > PADDLE_BOTTOM || b->x < left ||
            off > ((gv.paddle_width + 3) & 0xff)) {
            gv.hold_timer = (uint16_t)(HOLD_RESET);
            return 1;
        }
        b->y = PADDLE_TOP;
        b->state = 2;                 /* held */
        gv.hold_timer = (uint16_t)((gv.hold_timer - gv.speed_limit) & 0xffff);
        gv.hold_offset = (uint8_t)(b->x - gv.paddle_x);
        ball_redraw(b);
        cv.sound_request = SOUND_CATCH;
        return 0;
    }

    if (b->state != 2)
        return 1;                       /* a different ball; not held */

    int32_t release = gv.key_action == 1;
    if (!release) {
        gv.hold_timer = (uint16_t)(gv.hold_timer - 1);
        if (gv.hold_timer == 0) {
            release = 1;
        } else if (((gv.speed_limit - 1) & 0xff) == gv.speed_step) {
            /* On the frame the ball would have moved, the timer runs down
             * twice, so a held ball is let go after the same amount of play
             * however fast the level has become. */
            gv.hold_timer = (uint16_t)(gv.hold_timer - 1);
            if (gv.hold_timer == 0)
                release = 1;
        }
    }

    if (!release) {
        b->x = (uint8_t)(gv.paddle_x + gv.hold_offset);
        ball_redraw(b);
        return 0;
    }

    gv.hold_timer = (uint16_t)(HOLD_RESET);
    ball_after(b);
    b->dir_y = 1;                     /* away, upwards */
    b->y = 0xb4;
    b->anchor_x = b->x;
    b->anchor_y = 0xb4;
    b->acc_x = b->acc_y = 0;
    b->state = 1;
    ball_redraw(b);
    return 1;
}

/* 1ac2:1614  read_new_key
 *
 * The key-definition screen: wait for a scan code that is not already one of
 * the `bl` keys defined so far, and not one of the four the game keeps for
 * itself at 0x2d52. Then store it as key number `bl`.
 *
 * Not transcribed - see screen_define_keys.
 */
void read_new_key(uint32_t which)
{
    /* Part of the redefine-keys screen, which is not transcribed. It waited
     * on [0x2d49], the scan code the game's own INT 09h handler leaves. */
    (void)which;
}

/* 1ac2:108c  score_before
 *
 * Is the six-digit score at `b` lower than the one at `a`? The
 * hall-of-fame sort walks the table with this. `scasb` compares and steps, so
 * the first digit that differs decides.
 */
int32_t score_before(const uint8_t *a, const uint8_t *b)
{
    for (int32_t i = 0; i < 6; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 0;
}

/* ========================================================================
 * 1ac2:2ee3  laser_fire
 *
 * The paddle's laser. With no shot in flight the action key fires one from
 * just above the paddle; with one in flight it moves two pixels up a frame,
 * and when it reaches a brick the same table at 0x3044 that the ball uses
 * decides what happens.
 *
 * The dots are drawn and erased by hand here rather than through shot_xor,
 * and the row stepping is not the same on the two paths: firing touches three
 * consecutive scan lines, moving touches y, y+1, y+3 and y+4 - the `add
 * di,0x50` in the middle steps a row *within* the half it is already in,
 * which is two scan lines rather than one. Transcribed as it is.
 * ===================================================================== */
#define SHOT_SOUND 5

static void laser_dot_rows(uint32_t x, uint32_t y, int32_t moving)
{
    uint32_t mask = 0xc0 >> ((x & 3) * 2);
    uint32_t di = pixel_xor(x, y);
    di = cga_next_row(di);
    g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)mask;
    if (moving) {
        di = (di + 0x50) & 0xffff;
        g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)mask;
    }
    di = cga_next_row(di);
    g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)mask;
}

void laser_fire(void)
{
    if (gv.paddle_morphing == 0 && gv.laser_on != 2) {
        if (gv.key_action != 1)
            return;
        uint32_t x = (gv.paddle_x + 4) & 0xff;
        cv.sound_request = SHOT_SOUND;
        gv.laser_x = (uint8_t)x;
        uint32_t y = gv.laser_y;
        laser_dot_rows(x, y, 0);
        laser_dot_rows((x + 0x13) & 0xff, y, 0);
        gv.laser_y = 0xb1;
        gv.laser_on = 2;
        return;
    }
    if (gv.laser_on != 2)
        return;

    uint32_t x = gv.laser_x, y = gv.laser_y;
    laser_dot_rows(x, y, 1);
    laser_dot_rows((x + 0x13) & 0xff, y, 1);
    gv.laser_y -= 2;

    if (y < 4) {                        /* off the top of the playfield */
        shot_xor(x, y);
        gv.laser_y = 0xb3;
        return;
    }

    /* Probe the two cells the shot covers. */
    gv.hit_count = 0;
    uint32_t py = (x - 8) & 0xff, px = (y - 6) & 0xff;
    probe_cell_at(py, px, &gv.hits[0]);
    probe_cell_at((py + 0x13) & 0xff, px, &gv.hits[1]);
    if (gv.hit_count == 0)
        return;

    for (int32_t i = 0; i < 2; i++) {
        uint32_t cell = gv.hits[i].cell;
        if (cell)
            brick_hit(&gv.hits[i], cell, NULL); /* no ball: BP is zero */
    }
    shot_xor(gv.laser_x, (gv.laser_y + 2) & 0xff);
    gv.laser_y = 0xb3;
}

/* ========================================================================
 * 1ac2:3273  entity_capsule
 *
 * A bonus capsule falling towards the paddle. This one keeps its position in
 * [bx+2] and [bx+3] rather than [bx+4] and [bx+5] - [bx+4] is its **kind**,
 * which indexes the frame tables at 0x3385, and [bx+6]/[bx+7] hold the
 * paddle kind it will install and the frame it is showing.
 *
 * It steps once every eighth tick: three `shr dl,1 / jb` in a row is a test
 * that the low three bits of the counter are clear, written the way an 8086
 * writes it.
 *
 * Caught, it turns into the paddle-morph animation at 0x3386 and scores 23;
 * missed, it is simply dropped. Either way [0x3384] - how many capsules are
 * out - comes back down.
 * ===================================================================== */

/* 1ac2:3561  entity_popup is the same routine with a different set of frames -
 * table 0x339b rather than 0x3385 - so the two share a body. */
void entity_popup(ent_fall_t *f) { entity_capsule_frames(f, img_off(gv.popup_frames)); }
void entity_capsule(ent_fall_t *f) { entity_capsule_frames(f, img_off(gv.capsule_frames)); }

void entity_capsule_frames(ent_fall_t *f, uint32_t table)
{
    if ((--f->tick & 7) != 0)
        return;                         /* not this tick */

    uint32_t base = img_w(table + f->kind * 2);
    uint32_t di = base + f->frame * 4;
    uint32_t src = img_w(di), rows = img_w(di + 2);

    uint32_t y = f->y;
    f->y++;
    xor_sprite_16xn(f->x, y, src, rows & 0xff);

    y = f->y;
    if (y == 0xc5) {                    /* fallen past the paddle */
        gv.bonus_cap--;
        gv.entity_remove = 1;
        return;
    }

    if (y >= 0xb6 && y <= 0xbe) {
        /* Level with the paddle: does it overlap? The comparison is done in
         * sixteen bits with an `adc ch,0`, so a paddle at the right-hand edge
         * does not wrap. */
        uint32_t right = (f->x + 0x0e) & 0xffff;
        uint32_t px = gv.paddle_x;
        if (right >= px &&
            (right - 0x0f) <= (px + gv.paddle_width)) {
            /* Caught. The node stays where it is and becomes a different
             * kind of entity: the handler is rewritten and the same ten
             * bytes are read as the morph arm from here on - fall.frame and
             * morph.from are the same byte, fall.cycle and morph.to the
             * next. */
            uint8_t kind = f->kind;
            entity_t *e = entity_of(f);
            ent_morph_t *m = &e->p.morph;
            m->from = gv.paddle_kind;
            m->to = gv.paddle_next[kind];
            m->bonus = kind;
            m->pending = 1;
            m->step = 6;
            e->handler = 0x3386;        /* becomes the paddle morph */
            gv.bonus_cap--;
            brick_score(0, 0, 0x0302);
            return;
        }
    }

    /* Still falling: step the animation. Kind 2 cycles its frame 0..0x0f. */
    f->frame = f->cycle;
    if ((y & 3) == 2) {
        if (f->cycle == 0x0f)
            f->cycle = 0;
        else
            f->cycle++;
    }
    di = base + f->frame * 4;
    xor_sprite_16xn(f->x, y, img_w(di), img_w(di + 2) & 0xff);
}

/* ========================================================================
 * What the bonuses do: the table at 0x33bc, indexed by a capsule's kind.
 * ===================================================================== */

/* 1ac2:41b1  fill_column - 0x19 words down one column, stepping the interlace.
 * `stosw` then `dec di` twice leaves the offset where it started, so the
 * column stays put while the rows advance. */
void fill_column(uint32_t di, uint32_t value)
{
    for (int32_t i = 0; i < 0x19; i++) {
        g_vram[di & (CGA_SIZE - 1)] = (uint8_t)value;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = (uint8_t)(value >> 8);
        di = cga_next_row(di);
    }
}

/* 1ac2:2daa  bonus 0 - a hundred points, and it cancels the net and the
 * extra ball if either is running */
void bonus_points(void)
{
    brick_score(0, 0x100, 0);
    if (gv.net_on == 1) {
        flash_bar(0x1554);
        gv.net_on = 0;
        fill_column(0x1a77, 0);
    }
    if (gv.extra_on != 1)
        return;
    gv.extra_on = 0;
    fill_column(0x1a8b, 0);
}

/* 1ac2:2def  bonus 1 - the paddle catches the ball */
void bonus_catch(void)
{
    if (gv.caught != 0)
        return;
    gv.caught = 1;
    gv.hold_timer = (uint16_t)(HOLD_RESET);
}

/* 1ac2:2e03  bonus 3 - the laser */
void bonus_laser(void)
{
    if (gv.laser_on != 0)
        return;
    gv.laser_on = 1;
    gv.laser_y = 0xb3;
}

/* 1ac2:2e16  bonus 4 - more balls, run by an entity of its own */
void bonus_multiball(void)
{
    entity_alloc()->handler = 0x3717;
}

/* 1ac2:3231  bonus 2, the E capsule - the wider paddle.
 *
 * The routine really is empty, and that is not the whole story: the widening
 * is done by the morph, not by the effect. The table at 0x2d2d maps a
 * capsule's kind to a paddle kind, and E maps to paddle 1, which is 39 pixels
 * wide against the default 27 - the pairs are at 0x2d0d. So the capsule that
 * looks like it does nothing is one of the more useful ones. */
void bonus_wider_paddle(void) { }

/* 1ac2:3119  bonus 5 - the safety net across the bottom */
void bonus_net(void)
{
    if (gv.net_on != 1) {
        gv.net_on = 1;
        flash_bar(0x1554);
    }
    gv.net_life = (uint16_t)(0x1388);
    gv.net_timer = 0xc8;
    fill_column(0x1a77, 0xaaaa);
    gv.net_pos = (uint16_t)(0x1a77);
}

/* 1ac2:315b  bonus 6 - every ball in play reverses vertically and re-anchors
 * where it is */
void bonus_reverse(void)
{
    for (int32_t i = 0; i < 3; i++) {
        ball_t *b = &gv.balls[i];
        if (b->state == 0)
            continue;
        b->dir_y = (uint8_t)(b->dir_y == 1 ? 0 : 1);
        b->anchor_x = b->x;
        b->anchor_y = b->y;
        b->acc_x = b->acc_y = 0;
    }
}

/* 1ac2:31e8  bonus 9, the S capsule - the ball moves **less** often.
 *
 * [0x1486] is the gate's limit and it counts *down* to two, so the ball goes
 * from stepping on two frames in three to stepping on one in two. This
 * comment said "more often" and had it backwards: a smaller limit is a slower
 * ball, which is what the letter means. */
void bonus_slower_ball(void)
{
    if (gv.speed_limit != 2) {
        gv.speed_limit--;
        gv.speed_step = gv.speed_limit;
    }
    gv.speed_timer = 0x4e20;
}

/* The dispatch at 1ac2:337d. Kind 8 ends the level and is not here: it throws
 * four words off the stack and jumps into 0x4210, which no C call can do, so
 * it is handled where the morph animation calls this. */
void bonus_effect(uint32_t kind)
{
    switch (kind) {
    case 0: bonus_points(); break;
    case 1: bonus_catch(); break;
    case 2: bonus_wider_paddle(); break;
    case 3: bonus_laser(); break;
    case 4: bonus_multiball(); break;
    case 5: bonus_net(); break;
    case 6: bonus_reverse(); break;
    case 7: extra_life(); break;
    case 8:
        /* 1 if the level is done, 2 if a life went with it - see
         * ball_after_endgame's two endings. */
        longjmp(g_bonus_done, bonus_end_level());
    case 9: bonus_slower_ball(); break;
    case 10: bonus_stop_monsters(); break;
    /* The table has a twelfth word, 0x2e55, but it points into the middle of
     * ball_on_paddle and no capsule reaches it: the kind comes from [bx+4],
     * which also indexes the eleven-byte paddle table at 0x2d2d. */
    default: break;
    }
}

/* ========================================================================
 * 1ac2:3717  entity_multiball
 *
 * Fill every idle ball slot with a copy of one that is in play, each with its
 * vertical component two larger so they diverge instead of travelling as one.
 * Sets [0x2e73] to 3 - three balls alive - and unlinks itself; it exists only
 * to run once.
 * ===================================================================== */
void entity_multiball(void)
{
    if (gv.ball_alive == 3) {
        gv.entity_remove = 1;
        return;
    }

    /* Find one that is in play to copy. */
    ball_t *src = NULL;
    for (int32_t i = 0; i < 3; i++) {
        ball_t *b = &gv.balls[i];
        if (b->state != 0) {
            src = b;
            break;
        }
    }
    if (!src)
        return;                         /* none: nothing to multiply */

    gv.ball_alive = 3;
    uint32_t dy = src->dy, dx = src->dx;
    uint32_t x = src->x, y = src->y;

    for (int32_t i = 0; i < 3; i++) {
        ball_t *si = &gv.balls[i];
        if (si->state != 0)
            continue;
        ball_t *b = si;
        b->x = b->prev_x = b->anchor_x = (uint8_t)x;
        b->y = b->prev_y = b->anchor_y = (uint8_t)y;
        dx = (dx + 2) & 0xff;           /* each copy a little steeper */
        b->dy = (uint8_t)dy;
        b->dx = (uint8_t)dx;
        b->dir_x = src->dir_x;
        b->dir_y = src->dir_y;
        b->acc_x = b->acc_y = 1;
        b->state = 1;
        b->bounces = 0;

        memcpy(b->sprite, gv.ball_start_sprite, sizeof b->sprite);
        uint32_t shift = (b->x & 3) * 2;
        if (shift) {
            for (int32_t r = 0; r < 4; r++) {
                uint32_t w = b->sprite[r];
                b->sprite[r] = (uint16_t)((w >> shift) | (w << (16 - shift)));
            }
        }
        ball_draw(si->sprite, b->x, b->y);
    }
    gv.entity_remove = 1;
}

/* ========================================================================
 * 1ac2:3386  entity_paddle_fx
 *
 * The paddle changing shape when a capsule is collected. It shrinks the paddle
 * it has through six frames, swaps in the new one, grows that, and finally
 * applies the bonus effect and unlinks itself.
 *
 * It owns [0x2d3b] while it runs: that byte suppresses the ordinary paddle
 * draw, and it is set to 0xff and then counted down, with the animation
 * stepping only when the count is a multiple of 0x23 (a `div cl` and a test of
 * the remainder). [0x2d3c] holds which entity owns it, so a second capsule
 * collected mid-morph does not fight the first.
 *
 * [0x2d38] is how much the paddle's width changes per frame: the width at
 * [0x2d3a] goes up by it and the right-hand limit at [0x2d3f] down by the
 * same, which keeps the paddle inside the playfield as it grows.
 * ===================================================================== */
/* Three sprite tables, and they are not interchangeable. 0x2d0d is the one
 * the play loop draws the paddle from and the one whose `+2` byte gives the
 * width; 0x2d25 is the shrink animation and 0x2d1d the grow. Using 0x2d0d for
 * the grow draws a full-size paddle at every frame of it. */

static void morph_finish(ent_morph_t *m)
{
    bonus_effect(m->bonus);
    gv.entity_remove = 1;
}

void entity_paddle_fx(ent_morph_t *m)
{
    /* The morph is the node's, not the arm's: morph_owner is what stops a
     * second capsule from fighting the first, and what it holds is the
     * original's BX. */
    const uint16_t self = (uint16_t)img_off(entity_of(m));

    if (gv.paddle_morphing == 0) {
        /* Nothing is morphing. If the paddle is already the kind this capsule
         * gives, there is nothing to animate - just apply the effect. */
        if (gv.paddle_kind == m->to) {
            morph_finish(m);
            return;
        }
        m->from = gv.paddle_kind;
        gv.paddle_morphing = 0xff;
        gv.morph_owner = self;

        if (m->to != 2) {
            /* Losing the laser: take any shot in flight off the screen. */
            if (gv.laser_on == 2)
                shot_xor(gv.laser_x, (gv.laser_y + 2) & 0xff);
            gv.laser_on = 0;
        }
        if (m->to != 3) {
            /* Losing the catch: release anything held. */
            gv.caught = 0;
            gv.hold_timer = (uint16_t)(0x460);
            for (int32_t i = 0; i < 3; i++) {
                ball_t *ball = &gv.balls[i];
                ball_t *b = ball;
                if (b->state != 2)
                    continue;
                ball_after(ball);
                b->dir_y = 1;
                b->y = 0xb4;
                b->anchor_x = b->x;
                b->anchor_y = b->y;
                b->acc_x = b->acc_y = 0;
                b->state = 1;
                b->bounces = 0;
                ball_redraw(ball);
            }
        }
    } else if (gv.morph_owner != self) {
        return;                         /* somebody else's morph */
    }

    if (--gv.paddle_morphing % 0x23 != 0) {
        /* Between animation steps: redraw the current frame if the paddle has
         * moved, so it still follows the player. */
        if (gv.paddle_x == gv.paddle_prev_x)
            return;
        if (m->step == 6) {
            draw_paddle(img_ptr(gv.paddle_sets[gv.paddle_kind].sprites));
            return;
        }
        uint32_t si = m->sprites + m->step * 2;
        draw_paddle_shifted(img_ptr(img_w(si)));
        return;
    }

    if (m->step != 6) {
        morph_step(m);
        return;
    }

    /* A frame boundary with step == 6: pick the sprite list for this stage.
     * `pending` is 1 while shrinking the old paddle and 0 while growing the
     * new one. */
    uint32_t si, kind;
    if (m->pending != 0) {
        si = img_off(gv.paddle_shrink);
        gv.paddle_step = 0;
        kind = m->from;
        if (kind == 1)
            gv.paddle_step = 0xfe;    /* -2: this one shrinks */
        if (kind != 0) {
            morph_begin(m, si, kind);
            return;
        }
        m->pending = 0;
    }
    si = img_off(gv.paddle_grow);
    gv.paddle_step = 0;
    kind = m->to;
    if (kind == 1)
        gv.paddle_step = 2;
    if (kind != 0) {
        morph_begin(m, si, kind);
        return;
    }
    /* Both ends are the plain paddle: nothing to animate. */
    gv.paddle_kind = 0;
    gv.paddle_width = 0x1b;
    gv.paddle_morphing = 0;
    morph_finish(m);
}

/* 1ac2:34c5  morph_begin - start a stage: remember its sprite list and run
 * the first frame. */
void morph_begin(ent_morph_t *m, uint32_t table, uint32_t kind)
{
    m->sprites = (uint16_t)img_w(table + kind * 2);
    m->step = 6;
    gv.paddle_morphing = 0xff;
    morph_step(m);
}

/* 1ac2:34d7  morph_step - one frame of the shrink or grow */
void morph_step(ent_morph_t *m)
{
    m->step--;
    uint32_t si = m->sprites + m->step * 2;
    draw_paddle_shifted(img_ptr(img_w(si)));

    gv.paddle_width += gv.paddle_step;
    gv.paddle_max -= gv.paddle_step;

    if (m->step != 0)
        return;
    m->step = 6;
    if (m->pending == 1) {      /* done shrinking; grow next */
        m->pending = 0;
        gv.paddle_kind = 0;
        return;
    }
    /* Done growing: install the new paddle and apply the effect. */
    uint32_t kind = m->to;
    gv.paddle_kind = (uint8_t)kind;
    gv.paddle_width = gv.paddle_sets[kind].width;
    gv.paddle_morphing = 0;
    morph_finish(m);
}

/* ========================================================================
 * 1ac2:05f8  level_between
 *
 * Put the playfield back the way the cells say it is: four sprites from the
 * table at 0x33d7, then every brick redrawn from 0x2f18, then the band below
 * it cleared. Called when a level ends and when a life is lost - anything the
 * play loop left half-drawn is undone by simply drawing the truth again.
 *
 * A cell of 4 is empty and skipped; 0x0c has its own routine at 0x4cc1; and a
 * cell of 0x18 or more takes its bitmap from the block reached as segment
 * 0x14a1 instead of from the image, which is what the `cmp dl,0x30 / push ds`
 * around the copy is for - 0x30 because the index has already been doubled.
 * ===================================================================== */

void level_between(void)
{
    uint32_t si = img_off(gv.field_marks);
    for (int32_t i = 0; i < 4; i++, si += 4) {
        uint32_t x = g_image[si], y = (g_image[si + 1] - 0x0a) & 0xff;
        g_image[si + 3] = 0;
        uint32_t di = cga_at(x, y);
        for (int32_t r = 0; r < 0x25; r++) {
            g_vram[di & (CGA_SIZE - 1)] = gv.mark_sprite[r][0];
            g_vram[(di + 1) & (CGA_SIZE - 1)] = gv.mark_sprite[r][1];
            di = cga_next_row(di);
        }
    }

    for (int32_t i = 0; i < 0x18; i++) {
        g_vram[(0xa2 + i * 2) & (CGA_SIZE - 1)] = 0;
        g_vram[(0xa3 + i * 2) & (CGA_SIZE - 1)] = 0;
        g_vram[(0x20a2 + i * 2) & (CGA_SIZE - 1)] = 0;
        g_vram[(0x20a3 + i * 2) & (CGA_SIZE - 1)] = 0;
    }

    uint32_t di_cell = img_off(gv.level.cells);
    uint32_t y = 6;
    for (int32_t row = 0; row < 0x0e; row++, y += 8) {
        uint32_t x = 8;
        for (int32_t col = 0; col < 0x0c; col++, di_cell++, x += 0x10) {
            uint32_t cell = g_image[di_cell];
            if (cell == 0x0c) {
                cell_hole_draw(x, y);
                continue;
            }
            if (cell == 4)
                continue;               /* empty */
            uint32_t src = (cell >= 24 ? SEG_14A1 : 0) + gv.cell_bitmap[cell];
            uint32_t di = cga_at(x, y);
            for (int32_t r = 0; r < 8; r++) {
                for (int32_t b = 0; b < 4; b++)
                    g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[src + r * 4 + b];
                di = cga_next_row(di);
            }
        }
    }

    /* And the empty band under the bricks, 0x29 rows of 48 bytes. */
    uint32_t bp = 0x1272;
    for (int32_t r = 0; r < 0x29; r++, bp += 0x50) {
        for (int32_t i = 0; i < 48; i++) {
            g_vram[(bp + i) & (CGA_SIZE - 1)] = 0;
            g_vram[(bp + CGA_PLANE + i) & (CGA_SIZE - 1)] = 0;
        }
    }
}

/* ========================================================================
 * 1ac2:13b8  name_field
 *
 * One player-name box, read through INT 21h AH=07h - a blocking key with no
 * echo. Twelve characters, upper case only, digits, space and dash; anything
 * else is ignored rather than beeped at.
 *
 * The return says what to do next, and it is the original's carry: 1 means
 * start the game, 0 means take another name - and 0 with 0xff means the
 * player pressed Escape and the whole thing is off.
 *
 * Enter on an **empty** box is the "that's everyone" signal, but only once at
 * least one name has been entered; on the first box it just waits again.
 * ===================================================================== */

int32_t name_field(uint32_t di, uint8_t *abort)
{
    uint32_t si = img_off(gv.players[gv.player_digit - '1'].name);
    uint32_t len = 0;
    *abort = 0;

    di -= 2;
    draw_cursor(di);
    di += 2;

    for (;;) {
        uint32_t c = io_get_key() & 0xff;
        if (!c) {
            if (!io_pump())
                return 0;
            io_present();
            io_wait_retrace();
            continue;
        }

        if (c == 0x1b) {                /* Escape */
            *abort = 0xff;
            return 0;
        }
        if (c == 8) {                   /* Backspace, 1ac2:1380 */
            if (len == 0)
                continue;
            /* What gets painted over is the **cursor's** cell, at the
             * current di - not the character being removed. A full field
             * has its cursor one past the twelfth cell, where the right
             * thing is a space; anywhere else it sits on an empty cell of
             * the field, where the right thing is the dash placeholder.
             *
             * The character itself goes when the cursor moves back onto it:
             * di drops two cells, draw_cursor puts glyph 0xff one past that,
             * which is exactly where the deleted character was. */
            draw_char(len == 0x0c ? ' ' : '-', di);
            di -= 4;
            len--;
            draw_cursor(di);
            di += 2;
            g_image[--si] = 0;
            continue;
        }
        if (c == 0x0d) {                /* Enter */
            if (len != 0)
                break;                  /* accept this name */
            if (gv.player_count == 0)
                continue;               /* the first box: keep waiting */
            return 1;                   /* that is everyone: start */
        }
        if (c >= 0x60)
            c &= 0xdf;                  /* fold to upper case */
        int32_t ok = (c == ' ' || c == '-' ||
                  (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'));
        if (!ok || len == 0x0c)
            continue;
        g_image[si++] = (uint8_t)c;
        len++;
        draw_char((char)c, di);
        draw_cursor(di);
        di += 2;
    }

    /* Pad the rest of the field with spaces, then shift the name right by
     * half the space left so it sits centred in the box. */
    uint32_t pad = 0x0d - len;
    uint32_t shift = (pad - 1) >> 1;
    for (uint32_t i = 0; i < pad; i++, si++, di += 2) {
        g_image[si] = ' ';
        draw_char(' ', di);
    }
    /* 1ac2:145e - one place right per pass, and the walk is **downwards**:
     * `mov al,[si] / mov [si+1],al / dec si`, eleven times from rec+0x0a.
     * Going up instead copies each byte onto the one just written, so the
     * whole name becomes twelve copies of the byte before it and two passes
     * leave it blank - which is what a five-letter name got, since its
     * shift is three. The space then goes at rec+0, not rec-1: after the
     * eleven steps `si` is rec-1 and the store is `[si+1]`. */
    uint32_t base = img_off(gv.players[gv.player_digit - '1'].name) + 0x0a;
    for (uint32_t n = shift; n > 0; n--) {
        uint32_t si2 = base;
        for (int32_t k = 0x0b; k > 0; k--, si2--)
            g_image[si2 + 1] = g_image[si2];
        g_image[si2 + 1] = ' ';          /* 1ac2:146b */
    }
    return 0;
}

/* ========================================================================
 * 1ac2:10de  screen_player_names
 *
 * Ask each player for a name, one box under the last, up to eight.
 *
 * The box is three things at three places, and the original keeps two of them
 * on the stack rather than recomputing: DI at the top bar is pushed, the
 * label line one scan line below it is pushed as well, and the bar underneath
 * is 0x1e0 - twelve scan lines, a text line - below the label. The field the
 * player types into is the label line again, 0x16 bytes in.
 *
 * Get only the field right and the typing lands correctly on a box drawn in
 * the wrong place, which is exactly what a hardcoded address here used to do.
 *
 * When a name is taken the box turns into an engraved panel: four rows of
 * fixed patterns at 0x280 below the box's top, light down the left and dark
 * along the bottom. The next box starts a scan line and 0x50 past those.
 * ===================================================================== */
#define NAME_WIDTH  0x18                /* characters, and words of bar */

static uint32_t name_bar(uint32_t di, uint32_t word)
{
    for (int32_t i = 0; i < NAME_WIDTH; i++)
        img_vram_setw((di + i * 2) & 0xffff, word);
    return cga_next_row(di);
}

/* One row of the engraved panel: a byte, a middle, and a byte. The middle is
 * given as a byte so both the `rep stosw` rows and the `rep stosb` rows can
 * use it - 0xffff is 0xff twice, 0x5555 is 0x55 twice. */
static uint32_t panel_row(uint32_t di, uint32_t lead, uint32_t mid,
                          uint32_t tail, int32_t has_tail)
{
    uint32_t d = di;
    g_vram[d++ & (CGA_SIZE - 1)] = (uint8_t)lead;
    for (int32_t i = 0; i < (has_tail ? 0x2e : 0x2f); i++)
        g_vram[(d + i) & (CGA_SIZE - 1)] = (uint8_t)mid;
    if (has_tail)
        g_vram[(d + 0x2e) & (CGA_SIZE - 1)] = (uint8_t)tail;
    return cga_next_row(di);
}

uint8_t screen_player_names(void)
{
    gv.player_count = 0;
    play_frame();                       /* 1ac2:1212 - the surround */
    flush_keys();                   /* 1ac2:0106 */

    uint32_t di = 0x142;
    for (;;) {
        uint32_t top = di;                      /* pushed at 1ac2:10f2 */
        uint32_t label = name_bar(top, 0xaaaa); /* pushed at 1ac2:110e */

        draw_text(gv.name_prompt, NAME_WIDTH, label);
        name_bar((label + 0x1e0) & 0xffff, 0xaaaa);

        uint8_t abort = 0;
        int32_t done = name_field((label + 0x16) & 0xffff, &abort);
        if (done) {
            /* Rub the box out and start: fourteen rows of nothing. */
            uint32_t d = top;
            for (int32_t r = 0; r < 0x0e; r++)
                d = name_bar(d, 0);
            gv.player_digit = '1';
            return 0;
        }
        if (abort == 0xff) {
            gv.player_digit = '1';
            return 0xff;
        }
        if (++gv.player_count == 9) {
            gv.player_digit = '1';
            return 0;
        }

        /* The box just filled in becomes an engraved panel. */
        uint32_t d = (top + 0x280) & 0xffff;
        d = panel_row(d, 0x3f, 0xff, 0xfc, 1);
        d = panel_row(d, 0xf5, 0x55, 0,    0);
        d = panel_row(d, 0xd5, 0x15, 0,    0);
        d = panel_row(d, 0x15, 0x55, 0x54, 1);
        gv.player_digit++;
        di = (d + 0x50) & 0xffff;
    }
}

/* ========================================================================
 * 1ac2:1354  frame_band
 *
 * One horizontal band of the playfield surround: one row of
 * frame_corner_left chosen by cs:[0x5c6d], 0x17 words of `ax`, then the same
 * row of frame_corner_right. The
 * counter advances every call, so consecutive bands use different corner
 * pieces and the border does not repeat.
 * ===================================================================== */

uint32_t frame_band(uint32_t di, uint32_t fill)
{
    uint32_t phase = cv.frame_phase;
    for (int32_t i = 0; i < 3; i++)
        g_vram[(di + i) & (CGA_SIZE - 1)] = gv.frame_corner_left[phase][i];
    di += 3;
    for (int32_t i = 0; i < 0x17; i++, di += 2) {
        g_vram[di & (CGA_SIZE - 1)] = (uint8_t)fill;
        g_vram[(di + 1) & (CGA_SIZE - 1)] = (uint8_t)(fill >> 8);
    }
    for (int32_t i = 0; i < 3; i++)
        g_vram[(di + i) & (CGA_SIZE - 1)] = gv.frame_corner_right[phase][i];
    cv.frame_phase++;
    return di + 3;
}

/* ========================================================================
 * 1ac2:1212  play_frame
 *
 * The playfield surround: six bands down the top, then the side walls built
 * by scrolling a 0x1a-word column up 0xc2 times and laying a fresh two-byte
 * cap on each pass, which is what makes them look woven. The cap alternates
 * between 0x50 and 0x10 every fourth row - `and al,3` on the same counter the
 * bands use.
 * ===================================================================== */
void play_frame(void)
{
    cv.frame_phase = 0;

    uint32_t di = 0x1e50;
    static const uint32_t fills[6] = { 0xffff, 0x5555, 0x5454, 0x5555, 0, 0 };
    for (int32_t i = 0; i < 6; i++) {
        frame_band(di, fills[i]);
        if (i < 5)
            di = cga_next_row(di);
    }

    /* The walls. Each pass scrolls the column up six rows and caps it. */
    uint32_t bp = 0x3e00;
    for (int32_t pass = 0xc2; pass > 0; pass--) {
        io_wait_retrace();
        di = bp;
        for (int32_t dh = 6; dh > 0; dh--) {
            /* Copy the row below over this one. `rep movsw` leaves SI
             * 0x34 on and the `sub di, 0x34` puts it back, so what the next
             * iteration works on is the row just read - not 0x34 to the left
             * of it. Taking the subtraction literally walks the column left
             * a third of a line every row, which smears the white band at
             * the top of the frame across the whole screen. */
            uint32_t src = cga_next_row(di);
            for (int32_t i = 0; i < 0x1a * 2; i++)
                g_vram[(di + i) & (CGA_SIZE - 1)] =
                    g_vram[(src + i) & (CGA_SIZE - 1)];
            di = src;
        }
        di = cga_next_row(di);

        uint32_t cap = (cv.frame_phase & 3) ? 0x50 : 0x10;
        g_vram[di++ & (CGA_SIZE - 1)] = 0x0d;
        g_vram[di++ & (CGA_SIZE - 1)] = (uint8_t)cap;
        for (int32_t i = 0; i < 0x18; i++, di += 2) {
            g_vram[di & (CGA_SIZE - 1)] = 0;
            g_vram[(di + 1) & (CGA_SIZE - 1)] = 0;
        }
        g_vram[di++ & (CGA_SIZE - 1)] = 0x0d;
        g_vram[di & (CGA_SIZE - 1)] = (uint8_t)cap;
        cv.frame_phase++;

        bp = cga_prev_row(bp);
        /* `mov cx,0x5dc / loop $` - one busy-wait, not 0x5dc calls to the
         * delay routine. Calling it 1500 times a pass, 0xc2 passes, is a
         * third of a million trips through the platform layer for a wait
         * the original spends entirely inside two instructions. */
        io_delay_cycles(0x5dc * CYCLES_PER_LOOP);
        io_present();
        if (!io_pump())
            return;
    }

    panel_reveal();                     /* 1ac2:0911 */
    for (int32_t b = 5; b > 0; b--)
        for (int32_t i = 0; i < 0x147; i++)
            game_delay();
    panel_finish();                     /* 1ac2:09c5 */
}

/* ========================================================================
 * 1ac2:0911  panel_reveal
 *
 * The rails down both sides of the playfield and the pieces that cap them.
 * `0x500d` and `0x100d` are two-byte patterns written as words at a fixed
 * stride - 0x50 down the even half, 0xa0 down the odd - so the two rails are
 * laid in one pass each rather than a loop per column.
 * ===================================================================== */
void panel_reveal(void)
{
    uint32_t di = 0;
    for (int32_t i = 0; i < 0x64; i++, di += 0x50) {
        img_vram_setw(di, 0x500d);
        img_vram_setw(di + 0x32, 0x500d);
    }
    di = CGA_PLANE;
    for (int32_t i = 0; i < 0x32; i++, di += 0xa0) {
        img_vram_setw(di, 0x500d);
        img_vram_setw(di + 0x32, 0x500d);
        img_vram_setw(di + 0x50, 0x100d);
        img_vram_setw(di + 0x82, 0x100d);
    }

    /* The top edge: a solid row, then a lighter one under it. */
    for (int32_t i = 0; i < 0x19; i++)
        img_vram_setw(i * 2, 0xffff);
    for (int32_t i = 0; i < 0x19; i++)
        img_vram_setw(0x50 + i * 2, 0x1515);
    for (int32_t i = 0; i < 0x32; i++) {
        g_vram[(CGA_PLANE + i) & (CGA_SIZE - 1)] = 0x55;
        g_vram[(CGA_PLANE + 0x50 + i) & (CGA_SIZE - 1)] = 0x55;
    }

    /* And the corner pieces down each side. `si` is set once, before the
     * loop, and `rep movsb` carries it forward - so these are the two 21-byte
     * tables read straight through, three bytes per row, not one row drawn
     * seven times. frame_band takes a single row out of the same two. */
    di = 0;
    for (int32_t r = 0; r < 7; r++) {
        for (int32_t i = 0; i < 3; i++)
            g_vram[(di + i) & (CGA_SIZE - 1)] = gv.frame_corner_left[r][i];
        di = cga_next_row(di);
    }
    di = 0x31;
    for (int32_t r = 0; r < 7; r++) {
        for (int32_t i = 0; i < 3; i++)
            g_vram[(di + i) & (CGA_SIZE - 1)] = gv.frame_corner_right[r][i];
        di = cga_next_row(di);
    }
}

/* 1ac2:0598  field_marks
 *
 * The eight marks along the playfield from the table at 0x33d7, each 0x1f rows
 * of one word from mark_sprite. level_between draws the first four of the same
 * table 0x25 rows tall; this draws all eight, shorter.
 */
void field_marks(void)
{
    uint32_t si = img_off(gv.field_marks);
    for (int32_t i = 0; i < 8; i++, si += 4) {
        uint32_t x = g_image[si], y = (g_image[si + 1] - 0x0a) & 0xff;
        g_image[si + 3] = 0;
        uint32_t di = cga_at(x, y);
        for (int32_t r = 0; r < 0x1f; r++) {
            g_vram[di & (CGA_SIZE - 1)] = gv.mark_sprite[r][0];
            g_vram[(di + 1) & (CGA_SIZE - 1)] = gv.mark_sprite[r][1];
            di = cga_next_row(di);
        }
    }
}

/* 1ac2:09c5  panel_finish
 *
 * Six passes of four bands, each band 0x7d0 or so above the last, and then the
 * marks. The offsets are subtracted rather than stepped because the bands are
 * not evenly spaced.
 */
void panel_finish(void)
{
    uint32_t di = 0x1cc0;
    for (int32_t pass = 0; pass < 6; pass++) {
        /* 1ac2:09d1, the **top** of the pass. After the drawing instead puts
         * this side a pass ahead - the same slip the ending sync had. */
        io_frame_sync_extra(SYNC_CURTAIN);
        uint32_t d = di;
        field_marks_wide(d, pass);
        d = (d - 0x7d0) & 0xffff;
        field_marks_wide(d, pass);
        d = (d - 0x820) & 0xffff;
        field_marks_wide(d, pass);
        d = (d - 0x780) & 0xffff;
        field_marks_wide(d, pass);
        di = di > CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
        for (int32_t i = 0; i < 0x147; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }
    field_marks();
}

/* ========================================================================
 * The menu's live decoration, and two more bricks.
 * ===================================================================== */

/* 1ac2:2d68  brick 11 - 72 points, and the cell becomes 0x0c, which is not a
 * brick at all: the drawing code has a special case for it. */
void brick_11(hit_t *hit, ball_t *ball)
{
    /* **Not** brick_common. 1ac2:2d68 scores and asks for a sound and then
     * never touches the ball: there is no `inc [di+0x1d]` and no bp anywhere
     * in the routine, where every other handler has one. Zeroing the bounce
     * counter here made the every-0x23-bounces slope shuffle fire at a
     * different moment - which matters most on level 49, the last one, since
     * that is 168 of these bricks and nothing else. */
    (void)ball;
    brick_score(0, 0, 0x0207);
    cv.sound_request = SOUND_BRICK;
    g_image[hit->cell] = 0x0c;
    gv.level.bricks--;
    uint32_t x = hit->x, y = hit->y;
    xor_sprite_16x7(x, y, img_ptr(0x6406));
    brick_11_after(x, y);               /* 1ac2:4c4b */
}

/* 1ac2:3d95  bonus_spawn
 *
 * Once in a while the play loop tries to open a hatch. One of the four marks
 * at 0x33d7 is picked at random; it is refused if that mark is already busy
 * ([si+3]) or if either of the two cells behind it still has a brick. The
 * entity it starts runs 0x390d, the hatch animation, and remembers which mark
 * it belongs to so it can free it again.
 */
void bonus_spawn(void)
{
    uint32_t si = img_off(&gv.field_marks[game_random(io_ticks(), 4)]);
    if (g_image[si + 3] != 0)
        return;                         /* that hatch is already open */
    uint32_t di = img_off(gv.level.cells) + g_image[si + 2];
    if (g_image[di] != 0 || g_image[di + 0x0c] != 0)
        return;                         /* still bricked over */

    g_image[si + 3] = 1;
    gv.bonus_pending++;
    entity_t *e = entity_alloc();
    e->handler = 0x390d;
    ent_hatch_t *h = &e->p.hatch;
    h->cell = (uint16_t)si;
    h->x = g_image[si];
    h->y = g_image[si + 1];
    h->wait = 0;
    h->phase = 0x2bc;
    h->script = 0x604e;
}

/* 1ac2:50df  menu_banner_tick
 *
 * The text scrolling across the character's belly. [0x13c4] is a one-bit
 * window that walks right, and when it falls off the end a new character is
 * fetched: `xor al,0xaa` then `sub al,0x20` decodes it - the text is stored
 * obfuscated - and its six-byte cell is copied to 0:0000, which is where the
 * shifter reads from.
 */
void menu_banner_tick(void)
{
    if (gv.banner_state == 2) {
        gv.banner_state = 0x80;
        gv.banner_ptr = (uint16_t)(gv.banner_ptr + 1);
        uint32_t c = *img_ptr(gv.banner_ptr);
        c = ((c ^ 0xaa) - 0x20) & 0xff;
        memcpy(gv.scratch1.banner_cell, gv.banner_font[c], 6);
    }
    banner_shift();                     /* 1ac2:5140 */

    uint32_t di = 0x38a9;
    for (int32_t i = 0; i < 6; i++) {
        if (gv.scratch1.banner_cell[i] & gv.banner_state)
            g_vram[di & (CGA_SIZE - 1)] ^= 3;
        di = cga_next_row(di);
    }
    gv.banner_state >>= 1;
}

/* 1ac2:5448  particle_random
 *
 * The other random: the eighty particle records are summed, then the BIOS tick
 * counter's two words, then a running value at [0x1acd] which is advanced by
 * the quotient. `div cx` leaves the remainder, so the answer is 0..dx-1.
 *
 * It does **not** zero AX first: whatever the caller happened to leave there
 * is part of the seed. That is why `ax` is a parameter and why particle_init
 * threads its answer from one call to the next - starting from zero gives a
 * different sequence and the two runs diverge from the very first kernel.
 */
uint32_t particle_random(uint32_t ax, uint32_t ticks, uint32_t limit)
{
    uint32_t n = gv.particle_count;
    const uint8_t *p = gv.particles[0];
    for (uint32_t i = 0; i < n; i++)
        ax = (ax + p[i * 2] + (p[i * 2 + 1] << 8)) & 0xffff;
    ax = (ax + ticks) & 0xffff;
    ax = (ax + gv.particle_seed) & 0xffff;
    if (!limit)
        return 0;
    gv.particle_seed = (uint16_t)(gv.particle_seed + ax / limit);
    return ax % limit;
}

/* 1ac2:548a  particle_init
 *
 * Set one kernel going from (0x68, 0xa0) with a random speed and a random
 * angle. [si+0x0e] is its horizontal step and [si+0x0c] its direction; the
 * height is a parabola computed as `step * t * t / 100`, which is why the
 * record carries the time in [si+6] rather than a velocity.
 */
uint32_t particle_init(uint32_t si, uint32_t ax_in)
{
    img_setw(si + 0, 0x68);
    img_setw(si + 2, 0xa0);
    uint32_t ax = (particle_random(ax_in, io_ticks(), 6) + 8) & 0xffff;
    img_setw(si + 0x0e, ax);
    ax = (particle_random(ax, io_ticks(), 0x46) - 0x23) & 0xffff;
    if (ax == 0)
        ax = 0x0a;
    img_setw(si + 4, ax);
    img_setw(si + 6, ax);
    img_setw(si + 0x0c, ax >= 0x8000 ? 1 : 0xffff);
    /* `imul word [si+4]` twice then `idiv cx`. The first product is truncated
     * to sixteen bits before the second multiply - only AX carries forward -
     * and only the second keeps its high half, because `idiv` divides DX:AX.
     * Doing the whole thing in 32-bit C gives a different answer as soon as
     * the first product overflows, which it does for most angles. */
    int16_t v = (int16_t)img_w(si + 0x0e);
    int16_t t = (int16_t)ax;
    int16_t first = (int16_t)(v * t);
    int32_t prod = (int32_t)first * (int32_t)t;
    img_setw(si + 0x0a, (int16_t)(prod / 100) & 0xffff);
    img_setw(si + 8, (int16_t)(prod / 100) & 0xffff);
    return (uint32_t)(int16_t)(prod / 100) & 0xffff;   /* what AX is left as */
}

/* 1ac2:53c2  menu_particles_tick
 *
 * The popcorn kernels bouncing under the menu. Each is a sixteen-byte record
 * at 0x148d: origin (+0, +2), the launch angle (+4), the time since launch
 * (+6), the current height (+8) and the last one (+0x0a), the horizontal
 * direction (+0x0c) and the speed (+0x0e).
 *
 * Points are put on the screen with INT 10h AH=0Ch, one BIOS call per pixel -
 * which is where the six hundred thousand INT 10h calls in a minute of menu
 * come from. The port draws them directly. `AX = 0x0c83` is the call: bit 7 of
 * AL means **XOR**, so both the erase and the draw are the same operation and
 * neither needs to know what was underneath.
 *
 * The trajectory is a parabola in integer arithmetic: `height = speed * t * t
 * / 100` with `t` counting up from the angle. A kernel that would leave the
 * bottom of the screen is thrown away and launched again.
 */
void menu_particles_tick(void)
{
    uint32_t si = img_off(gv.particles);
    uint32_t n = gv.particle_count;
    for (uint32_t k = 0; k < n; k++, si += 0x10) {
        /* Rub out where it was. */
        uint32_t x = (img_w(si) + img_w(si + 4) - img_w(si + 6)) & 0xffff;
        uint32_t y = (img_w(si + 8) + img_w(si + 2) - img_w(si + 0x0a)) & 0xffff;
        if (x <= 0x13f && y <= 0xc7)
            plot_pixel_xor(x, y, 3);

        img_setw(si + 6, (img_w(si + 6) + img_w(si + 0x0c)) & 0xffff);
        int16_t t = (int16_t)img_w(si + 6);
        int16_t v = (int16_t)img_w(si + 0x0e);
        int16_t first = (int16_t)(v * t);
        int32_t prod = (int32_t)first * (int32_t)t;
        img_setw(si + 8, (int16_t)(prod / 100) & 0xffff);

        y = (img_w(si + 8) + img_w(si + 2) - img_w(si + 0x0a)) & 0xffff;
        x = (img_w(si) + img_w(si + 4) - img_w(si + 6)) & 0xffff;
        if (y <= 0xc7 && x <= 0x13f)
            plot_pixel_xor(x, y, 3);

        if (y > 0xc7)
            particle_init(si, y);       /* gone: launch another */
        /* One delay per kernel, not one per kernel per kernel: `mov cx,bp`
         * restores the loop counter and the `call` is outside any loop. */
        game_delay();
    }
}

/* 1ac2:5476  menu_particles_init - every kernel launched at once */
/* `ax_in` is the seed the caller happened to leave in AX. It changes only
 * which kernels you get, so at the real call site anything will do - but the
 * verifier has to pass what the original had or the two diverge. */
void menu_particles_init(uint32_t ax_in)
{
    uint32_t n = gv.particle_count;
    uint32_t ax = ax_in;
    for (uint32_t i = 0; i < n; i++)
        ax = particle_init(img_off(gv.particles[i]), ax);
}

/* INT 10h AH=0Ch in mode 05h: one pixel, two bits, in the byte that holds it.
 * The virtual screen the game plots into is 320 wide, so `cx` is used as it
 * comes rather than halved. */
void plot_pixel(uint32_t x, uint32_t y, uint32_t colour)
{
    if (x >= CGA_W || y >= CGA_H)
        return;
    uint32_t di = cga_at(x, y);
    uint32_t shift = 6 - (x & 3) * 2;
    uint8_t *p = &g_vram[di & (CGA_SIZE - 1)];
    *p = (uint8_t)((*p & ~(3u << shift)) | ((colour & 3) << shift));
}

/* The same with bit 7 of AL set: XOR rather than replace. */
void plot_pixel_xor(uint32_t x, uint32_t y, uint32_t colour)
{
    if (x >= CGA_W || y >= CGA_H)
        return;
    uint32_t di = cga_at(x, y);
    uint32_t shift = 6 - (x & 3) * 2;
    g_vram[di & (CGA_SIZE - 1)] ^= (uint8_t)((colour & 3) << shift);
}

/* 1ac2:5140  banner_shift
 *
 * Slide the banner one pixel left. Six rows, and each row is fourteen bytes
 * shifted left by one bit twice - two bits being one pixel at this depth.
 *
 * The bytes are walked from the highest address down: `shl` the first, then
 * `rcl` the rest so each takes the bit that left the one before it. The `stc`
 * ahead of the `shl` does nothing - `shl` shifts a zero in regardless and
 * overwrites the flag - which is why the banner scrolls in blank rather than
 * repeating itself.
 */
#define BANNER_ROW 0x38a9
#define BANNER_LEN     14

void banner_shift(void)
{
    uint32_t di = BANNER_ROW;
    for (int32_t row = 0; row < 6; row++) {
        for (int32_t twice = 0; twice < 2; twice++) {
            uint32_t carry = 0;
            for (int32_t b = 0; b < BANNER_LEN; b++) {
                uint32_t a = (di - b) & (CGA_SIZE - 1);
                uint32_t v = g_vram[a];
                g_vram[a] = (uint8_t)((v << 1) | carry);
                carry = (v >> 7) & 1;
            }
        }
        di = cga_next_row(di);
    }
}

/* ========================================================================
 * 1ac2:0cc5  play_prepare  and  1ac2:1509  demo_start
 *
 * Each player gets a 0x11b-byte record at 0x344f: name, then lives, the offset
 * of the level they are on, the score as ASCII digits, a private copy of the
 * level's 176 cells, and six 0xffff terminators.
 *
 * A record carries its own copy of the cells because with more than one player
 * the game switches between them, and each has to come back to the level as
 * they left it.
 * ===================================================================== */

static void player_record_init(player_t *p)
{
    p->lives = 5;
    p->level_src = LEVEL_TABLE;
    p->level_number = 0;
    memset(p->score, '0', sizeof p->score);
    p->level = c46.levels[0];
    for (int32_t i = 0; i < 6; i++)
        p->state[i] = 0xffff;
}

void play_prepare(void)
{
    for (uint32_t n = 0; n < gv.player_count; n++) {
        player_record_init(&gv.players[n]);
        gv.players[n].ent_count = 0;
    }
}

/* The demo plays itself: [0x2d45] points at 0x1785, a third input routine that
 * reads a recorded script instead of a keyboard or a mouse, and cs:[0x1784] is
 * set to 0xff to say it is running. One player, under demo_name. */


void demo_start(void)
{
    gv.input_active = (uint16_t)(INPUT_DEMO);
    cv.demo_ball = 0xff;
    memcpy(gv.players[0].name, gv.demo_name, sizeof gv.players[0].name);
    player_record_init(&gv.players[0]);
    /* 1ac2:1576 clears **+0xd3**, one past the entity count at +0xd2 that
     * play_prepare clears at 1ac2:0d20 and 1ac2:0dce. So the demo's record
     * never has its count cleared; it blanks the first byte of the first
     * entity slot instead. The original's slip, kept. */
    gv.players[0].ents[0][0] = 0;
    gv.player_count = 1;
}

/* ========================================================================
 * 1ac2:490d  menu_arrow
 *
 * The arrow that says whether the mouse or the keyboard is selected. It is
 * XOR-drawn on both rows every time, so calling it moves the arrow from
 * whichever row it is on to the other - there is no state to keep.
 * ===================================================================== */
#define ARROW_MOUSE 0x0bfe
#define ARROW_KEYS  0x088e

/* 1ac2:492f  arrow_head - the nine rows of arrow_head_sprite */
void arrow_head(uint32_t di)
{
    for (int32_t r = 0; r < 9; r++) {
        for (int32_t b = 0; b < 5; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] ^= gv.arrow_head_sprite[r][b];
        di = cga_next_row(di);
    }
}

/* 1ac2:4957  arrow_tail - four blank rows, one solid, four blank: XOR-ing
 * zero changes nothing, so what this actually draws is the single 0x5555 row
 * in the middle. The blank rows only step the offset. */
void arrow_tail(uint32_t di)
{
    for (int32_t r = 0; r < 4; r++)
        di = cga_next_row(di);
    for (int32_t b = 0; b < 5; b++)
        g_vram[(di + b) & (CGA_SIZE - 1)] ^= 0x55;
    di = cga_next_row(di);
    for (int32_t r = 0; r < 4; r++)
        di = cga_next_row(di);
}

void menu_arrow(void)
{
    arrow_head(ARROW_MOUSE);
    arrow_tail(ARROW_MOUSE);
    arrow_tail(ARROW_KEYS);
    arrow_head(ARROW_KEYS);
}

/* ========================================================================
 * Small routines: the keyboard vector, the cheat matcher, the palette, and
 * the two disk calls.
 * ===================================================================== */

/* 1ac2:0106  flush_keys - drain the BIOS buffer with INT 16h until it is
 * empty. The platform keeps its own queue, so this is that queue. */
void flush_keys(void)
{
    io_flush_keys();
}

/* 1ac2:03b0 and 1ac2:03d1  install_int09 / restore_int09
 *
 * The original hooks INT 09h so it can read make and break codes, which the
 * BIOS buffer does not report, and takes the hook out again for the menus.
 * There is no vector table here: the platform layer maintains the same three
 * bytes at 0x2d4c-0x2d4e from SDL key events, all the time, so both are
 * recorded and do nothing. See the note on 1ac2:03e3 below.
 */
/* These are no-ops in the sense that there is no vector to write, but they
 * are **not** nothing: while the game's own handler is installed it does not
 * chain to the BIOS, so INT 16h's buffer stays empty. That is load-bearing.
 *
 * `input_keys_keyboard` pauses on Esc and then waits for a key through INT
 * 16h - and calls restore_int09 first precisely so there is something to wait
 * for. If the platform layer keeps filling that buffer while the game's
 * handler is installed, the Esc that opened the pause is still sitting in it,
 * satisfies the wait immediately, and the pause opens and closes inside one
 * frame. At 326 Hz that looks exactly like Esc doing nothing. */
void install_int09(void) { io_set_int09_installed(1); }
void restore_int09(void) { io_set_int09_installed(0); }

/* 1ac2:03e3  the INT 09h handler itself
 *
 * Not called: it is the handler the two above install. Written out because it
 * is the whole keyboard interface and the platform layer stands in for it.
 *
 *   in al,0x60                 the scan code
 *   in al,0x61 / or al,0x80    acknowledge, then put the port back
 *   out 0x61,al
 *   ah = (al <= 0x7f)          make or break
 *   if al == [0x2d4f]  [0x2d4a] = 0      the last direction, left
 *   if al == [0x2d50]  [0x2d4a] = 1      or right
 *   if al == 0xc3      cs:[0x84] ^= 1    F9 released: sound on/off
 *   if al <= 0x7f      [0x2d49] = al     the last make code
 *   al &= 0x7f
 *   repne scasb over the three configured keys at 0x2d4f
 *   if found          [0x2d4c + cx] = ah
 *   out 0x20,0x20                        end of interrupt
 *
 * The `repne scasb` leaves CX at 2, 1 or 0 for left, right or action, and
 * `0x2d4c + cx` turns that into 0x2d4e, 0x2d4d or 0x2d4c - so the three state
 * bytes are in the reverse of the order the scan codes are.
 */

/* 1ac2:41d4  play_teardown - blank the two indicator columns */
void play_teardown(void)
{
    fill_column(0x1a77, 0);
    fill_column(0x1a8b, 0);
}

/* 1ac2:41e5  cell_special
 *
 * What a cell of 0x0c draws: four bytes from the block reached as segment
 * 0xc46, at 0x28f0 + row * 0x30 + column * 4. Not a brick - it is the hole
 * brick 11 leaves.
 *
 * The column arrives in `cl`; it is not derivable from `di`, which is where
 * the four bytes go. This read it back out of `di` with the formula for the
 * *cell index* - `(di - 0x2f18) % 12` on a vram offset - and picked the wrong
 * four bytes. `di` steps four a column, so it cycled with period three.
 */
void cell_special(uint32_t row, uint32_t col, uint32_t di)
{
    const uint8_t *src = &c46.hole_picture[row & 0xff][col * 4];
    for (int32_t b = 0; b < 4; b++)
        g_vram[(di + b) & (CGA_SIZE - 1)] = src[b];
}

/* 1ac2:48af  input_and_draw_paddle - read the input and put the paddle where
 * it now is. The between-levels sequences call this so the paddle keeps
 * following the player while nothing else is running. */
void input_and_draw_paddle(void)
{
    game_input();
    draw_paddle(img_ptr(gv.paddle_sets[gv.paddle_kind].sprites));
}

/* 1ac2:5171  cheat_match
 *
 * One character of a typed cheat. cheat_at walks along cheat_text; a wrong
 * character starts it over at the beginning, and reaching the terminating
 * return sets cheat_done, which is what the menu tests.
 */
void cheat_match(uint8_t c)
{
    uint32_t bx = gv.cheat_at;
    if (c != g_image[bx]) {
        gv.cheat_at = (uint16_t)img_off(gv.cheat_text);       /* wrong: back to the beginning */
        return;
    }
    if (g_image[bx] == 0x0d) {
        gv.cheat_done = 1;            /* the whole word */
        return;
    }
    gv.cheat_at = (uint16_t)(bx + 1);
}

/* 1ac2:5196  palette_cycle
 *
 * F8. [0x13c8] is the colour-select register and 0x10 is added to it each
 * press, so it walks the intensity and palette bits; every fourth press it
 * wraps to zero and the colour-burst bit in the mode register at [0x13c7] is
 * flipped as well. Both are then written to their ports.
 */
void palette_cycle(void)
{
    gv.cga_colour += 0x10;
    if (gv.cga_colour == 0) {
        gv.cga_mode ^= 4;
        io_cga_mode(gv.cga_mode);
    }
    io_cga_colour(gv.cga_colour);
}

/* ========================================================================
 * The hall of fame: its table, its file, and the border that runs round it.
 * ===================================================================== */
#define HSC_ENTRY   0x12                /* twelve of name and six of score */
#define HSC_COUNT     10
#define HSC_SCRATCH img_off(gv.scratch2.hsc_scratch)

/* 1ac2:4d5d  hsc_bubble - one pass of the sort, from the bottom up.
 * `scasb` compares a name's six score digits against the entry above it and
 * swaps the whole nine-word record when the lower one is bigger. */
uint32_t hsc_bubble(uint32_t si, uint32_t di)
{
    /* Returns where the new record belongs. hsc_sort's `rep movsw` at
     * 1ac2:4d4c runs **before** its `pop di`, so the destination is whatever
     * DI this leaves - not the 0x3ef6 it started from. */
    si += 0x0c;
    di += 0x0c;
    for (int32_t n = 0x0a; n > 0; n--) {
        int32_t higher = 0;
        for (int32_t i = 0; i < 6; i++) {
            if (g_image[si + i] != g_image[di - 0x12 + i]) {
                higher = g_image[si + i] > g_image[di - 0x12 + i];
                break;
            }
        }
        if (!higher)
            return di - 0x0c;           /* 1ac2:4d78 */
        memmove(g_image + di - 0x0c, g_image + di - 0x1e, 0x12);
        /* 1ac2:4d8a is `sub di, 0x18`, but on a DI the `rep movsw` has just
         * advanced by 0x12 - so the step from the top of the loop is 0x12,
         * not 0x18. Taking the instruction at face value moved the window six
         * bytes too far each pass and shifted the wrong record. */
        di -= 0x12;
    }
    return di - 0x0c;                   /* 1ac2:4d92 */
}

/* 1ac2:4d37  hsc_sort - the whole table, once per player who just finished */
void hsc_sort(void)
{
    uint32_t di = img_off(&gv.hsc[10]), si = HSC_SCRATCH;
    for (uint32_t n = gv.player_count; n > 0; n--) {
        memcpy(g_image + hsc_bubble(si, di), g_image + si, 0x12);
        si += 0x12;
    }
}

/* 1ac2:4dbb  hsc_save - write the table back to popcorn.hsc, 0xb4 bytes from
 * gv.hsc. The drive check at 0x4e04 comes first, and a failure is silent. */
void hsc_save(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", dir ? dir : "",
             gv.hsc_file);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(gv.hsc, 1, HSC_LEN, f);
    fclose(f);
}

/* 1ac2:4ff1  border_draw - eight words from cs:0x506d down a column */
void border_draw(uint32_t di)
{
    for (int32_t i = 0; i < 8; i++) {
        img_vram_setw(di, cv.border_spr[i]);
        di = cga_next_row(di);
    }
}

/* 1ac2:4fd3  border_erase - the same eight rows, blanked */
void border_erase(uint32_t di)
{
    for (int32_t i = 0; i < 8; i++) {
        img_vram_setw(di, 0);
        di = cga_next_row(di);
    }
}

/* 1ac2:4fa7  border_step
 *
 * Move one marker round the edge of the screen. `di / 0x50` splits the offset
 * into a row and a column, and the four cases are the four sides: column 0
 * going up, column 0x32 going down, row 0 going right and row 0x60 going
 * left. `0x140` is four scan lines, which is the vertical step.
 */
uint32_t border_step(uint32_t di)
{
    uint32_t row = di / 0x50, col = di % 0x50;
    if (row == 0)
        return (col == 0x32) ? di + 0x140 : di + 2;
    if (row == 0x60)
        return (col == 0) ? di - 0x140 : di - 2;
    return (col == 0x32) ? di + 0x140 : di - 0x140;
}

/* 1ac2:4f58  border_animate - fourteen markers, each erased, stepped and
 * redrawn, with their positions kept in the code segment at 0x507d */
void border_animate(void)
{
    for (int32_t i = 0; i < 0x0e; i++) {
        uint32_t di = cv.border_pos[i];
        border_draw(di);
        di = border_step(di);
        border_erase(di);
        cv.border_pos[i] = (uint16_t)di;
    }
}

/* 1ac2:5019  border_block, and 1ac2:5045  border_row
 *
 * The top and bottom edges: 0x1a columns of the eight-word sprite side by
 * side, and 0x17 rows of it stacked. Between them they lay the frame the
 * markers then run around.
 */
void border_row(uint32_t di)
{
    /* 0x17 passes of eight words each, the pattern read from **cs:0x506d** -
     * the code segment, the bytes immediately after this routine - and DI
     * carried on down the interlace throughout rather than reset per pass.
     * What was here before called border_draw 0x1a times, which is neither
     * the count nor the operation. */
    for (int32_t n = 0x17; n > 0; n--) {
        uint32_t si = CS_BASE + 0x506d;
        for (int32_t r = 0; r < 8; r++, si += 2) {
            uint32_t w = img_w(si);
            g_vram[di & (CGA_SIZE - 1)] = (uint8_t)w;
            g_vram[(di + 1) & (CGA_SIZE - 1)] = (uint8_t)(w >> 8);
            di = cga_next_row(di);
        }
    }
}

void border_block(uint32_t di)
{
    /* 0x1a columns **side by side**: 1ac2:501e pushes DI and 1ac2:503d pops
     * it, so each pass starts where the last began and steps two bytes
     * across. 0x5045 is the same eight words with the push/pop taken out and
     * a count of 0x17, which is what makes one a row of blocks and the other
     * a column - and is why the two were transcribed into each other. */
    for (int32_t n = 0x1a; n > 0; n--, di += 2) {
        uint32_t d = di;
        for (int32_t i = 0; i < 8; i++) {
            img_vram_setw(d, cv.border_spr[i]);
            d = cga_next_row(d);
        }
    }
}

/* ========================================================================
 * The end of a level, and the two screens that borrow the framebuffer.
 * ===================================================================== */

/* 1ac2:4878  screen_scroll_up
 *
 * Roll the whole screen up 0x6f times, seven words to a row, keeping the
 * paddle drawn as it goes. What the level-ending bonus leaves behind.
 */
void screen_scroll_up(void)
{
    /* One loop, not two. `mov cx, 7` is the **word count** for the `rep
     * movsw` - seven words, fourteen bytes, one row - and `ax = 0x6f` is the
     * **row count**. Reading the seven as rows and the 0x6f as passes scrolled
     * seven rows a hundred and eleven times and drew the paddle a hundred and
     * eleven times, where the original scrolls a hundred and eleven rows once
     * and draws it at the end. */
    io_frame_sync_extra(SYNC_SCROLL);              /* --sync-scroll, for the driver */
    uint32_t di = 0x13;
    for (int32_t n = 0x6f; n > 0; n--) {
        uint32_t si = cga_next_row(di);
        for (int32_t b = 0; b < 14; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] =
                g_vram[(si + b) & (CGA_SIZE - 1)];
        di = si;                        /* 1ac2:48a1 restores the row start */
    }
    game_delay();
    input_and_draw_paddle();
}

/* 1ac2:48ce  level_tally
 *
 * Score every brick still standing when a level is cut short: each of the
 * 0xa8 cells looks its value up in cell_score, which holds the four-byte
 * figure to add, and then a flat thousand goes on top.
 */
void level_tally(void)
{
    for (int32_t i = 0; i < 0xa8; i++) {
        uint32_t v = gv.level.cells[i];
        img_setw(img_off(gv.score_add) + 0, 0);
        img_setw(img_off(gv.score_add) + 2, gv.cell_score[v][0]);
        img_setw(img_off(gv.score_add) + 4, gv.cell_score[v][1]);
        score_add();
    }
    brick_score(0, 1, 0);
}

/* 1ac2:4ba9  screen_stash
 *
 * Put the playing screen aside in screen_stash and paint the overlay
 * pause_overlay over it - 0x26 rows of 0x32 bytes. Used by the pause screen and
 * by F10.
 */
void screen_stash(void)
{
    speaker_off();
    uint32_t di = img_off(gv.scratch2.screen_stash);
    uint32_t si = 0x1900;
    for (int32_t half = 0; half < 2; half++) {
        for (int32_t r = 0; r < 0x14; r++) {
            for (int32_t b = 0; b < 0x32; b++)
                g_image[di + b] = g_vram[(si + b) & (CGA_SIZE - 1)];
            di += 0x32;
            si += 0x32 + 0x1e;
        }
        si = 0x3900;
    }
    di = 0x1900;
    for (int32_t r = 0; r < 0x26; r++) {
        for (int32_t b = 0; b < 0x32; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = gv.pause_overlay[r][b];
        di = cga_next_row(di);
    }
}

/* 1ac2:4b4f  screen_restore
 *
 * Put it back: the mode register to 0x0e, the palette registers from the
 * table at 0x4b9d, the 0x7d0 words of screen, and the speaker on again.
 */
void screen_restore(void)
{
    io_cga_mode(0x0e);
    set_palette_registers(0x4b9d);
    memcpy(g_vram, gv.scratch2.screen_stash, 0x7d0 * 2);
    speaker_on();
}

/* 1ac2:4c4b  brick_11_after
 *
 * XOR the hole brick 11 leaves: eight rows of four bytes from the block at
 * 0xc46:0x28f0, indexed the same way cell_special indexes it but with the
 * column taken from `(x >> 2) - 2` rather than from the cell address.
 */
void brick_11_after(uint32_t x, uint32_t y)
{
    uint32_t row = (y - 6) & 0xff;
    uint32_t si = SEG_C46 + 0x28f0 + row * 0x30 + (((x >> 2) & 0xff) - 2);
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 8; r++) {
        g_vram[di & (CGA_SIZE - 1)] ^= g_image[si];
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= g_image[si + 1];
        g_vram[(di + 2) & (CGA_SIZE - 1)] ^= g_image[si + 2];
        g_vram[(di + 3) & (CGA_SIZE - 1)] ^= g_image[si + 3];
        si += 0x2c + 4;
        di = cga_next_row(di);
    }
}

/* ========================================================================
 * More screens.
 * ===================================================================== */

/* 1ac2:4cc1  cell_hole_draw
 *
 * The same picture brick_11_after XORs, drawn rather than XORed: eight rows of
 * four bytes from 0xc46:0x28f0 at row * 0x30 + (x >> 2) - 2, stepping 0x30
 * bytes a row. level_between uses it for a cell of 0x0c.
 */
void cell_hole_draw(uint32_t x, uint32_t y)
{
    uint32_t row = (y - 6) & 0xff;
    uint32_t si = SEG_C46 + 0x28f0 + row * 0x30 + (((x >> 2) & 0xff) - 2);
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 8; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[si + b];
        si += 0x30;
        di = cga_next_row(di);
    }
}

/* 1ac2:4c13  screen_unstash - the saved playfield back onto the screen, and
 * the speaker on again */
void screen_unstash(void)
{
    uint32_t si = img_off(gv.scratch2.screen_stash);
    uint32_t di = 0x1900;
    for (int32_t half = 0; half < 2; half++) {
        for (int32_t r = 0; r < 0x14; r++) {
            for (int32_t b = 0; b < 0x32; b++)
                g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[si + b];
            si += 0x32;
            di += 0x32 + 0x1e;
        }
        di = 0x3900;
    }
    speaker_on();
}

/* 1ac2:4ae0  employee_enter
 *
 * F10, the "touche spéciale pour employés". It puts the whole screen aside,
 * switches the CGA to **text mode** by writing 9 to the mode register, and
 * prints the message at 0x2298 as character/attribute pairs: 0x5b switches to
 * inverse video, 0x9c switches back, and 0x24 pads eight blanks.
 *
 * The port has no text renderer, so what it does here is the mode change and
 * the stash - the message itself is not drawn. It is the one screen in the
 * game that is not graphics, and giving it a renderer is a separate job from
 * transcribing it.
 */
void employee_enter(void)
{
    /* **Not transcribed, on purpose.** F10 is the boss key: it puts the
     * screen aside, switches to text mode and paints a fake DOS prompt so the
     * game can be hidden from whoever walks past. The routine at 1ac2:4ae0 is
     * understood - this is a decision about what the port is for, not
     * something still to be read. */
}

/* 1ac2:4f73  border_setup - the frame the hall of fame sits in, and the
 * fourteen markers started at the top-left corner seven steps apart */
void border_setup(void)
{
    /* The other way round from what this used to say: 1ac2:4f75 calls
     * border_block at 0 and 0x1e00, and border_row at 0x140 and 0x172. */
    border_block(0);
    border_block(0x1e00);
    border_row(0x140);
    border_row(0x172);

    uint32_t di = 0;
    for (int32_t i = 0; i < 0x0e; i++) {
        cv.border_pos[i] = (uint16_t)di;
        border_erase(di);
        for (int32_t k = 0; k < 7; k++)
            di = border_step(di);
    }
}

/* 1ac2:538d  tall_sprite - fifteen rows of four bytes, drawn not XORed.
 *
 * Two things the caller depends on and the port used to drop. `lodsw` carries
 * SI forward, so a second call draws the *next* sixty bytes - that is how the
 * ending flashes rather than standing still. And the routine ends by polling
 * the keyboard 0x4b0 times, returning carry if a key came: on an 8086 those
 * BIOS calls are what paces the frame, and they are also the only way out of
 * the ending. */
#define TALL_SPRITE_BYTES  (0x0f * 4)
#define KBD_POLL_CYCLES    150          /* what one INT 16h AH=01 cost */

int32_t tall_sprite(uint32_t *si, uint32_t di)
{
    for (int32_t r = 0; r < 0x0f; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[*si + r * 4 + b];
        di = cga_next_row(di);
    }
    *si += TALL_SPRITE_BYTES;

    for (int32_t n = 0x4b0; n > 0; n--) {
        if (io_key_ready()) {
            io_get_key();
            return 1;
        }
        io_delay_cycles(KBD_POLL_CYCLES);
    }
    io_present();
    return 0;
}

/* ========================================================================
 * 1ac2:0a1d  field_marks_wide
 *
 * One of the pillars either side of the playfield, drawn as a fixed sequence
 * of word pairs down a column and its twin 0x32 bytes to the right. `rows` is
 * how many of the middle segment to repeat, so the pillar grows a row per
 * pass and appears to extend downwards.
 * ===================================================================== */
static uint32_t pillar_pair(uint32_t di, uint32_t v)
{
    img_vram_setw(di, v);
    img_vram_setw(di + 0x32, v);
    return cga_next_row(di);
}

void field_marks_wide(uint32_t di, uint32_t rows)
{
    di = pillar_pair(di, 0x4001);
    di = pillar_pair(di, 0x500f);
    di = pillar_pair(di, 0x4435);
    for (uint32_t n = rows; n > 0; n--) {
        di = pillar_pair(di, 0xd43f);
        di = pillar_pair(di, 0x1005);
    }
    di = pillar_pair(di, 0xd43f);
    di = pillar_pair(di, 0x4435);
    di = pillar_pair(di, 0x500f);
    pillar_pair(di, 0x4001);
}

/* 1ac2:59f7  ending_particle_init
 *
 * particle_init again for the ending: the same record, launched from
 * (0x68, 0x98) instead of (0x68, 0xa0) and with a speed of `random(3) + 8`
 * rather than `random(6) + 8`, so the kernels there rise more slowly.
 */
uint32_t ending_particle_init(uint32_t si, uint32_t ax_in)
{
    img_setw(si + 0, 0x68);
    img_setw(si + 2, 0x98);
    uint32_t ax = (particle_random(ax_in, io_ticks(), 3) + 8) & 0xffff;
    img_setw(si + 0x0e, ax);
    ax = (particle_random(ax, io_ticks(), 0x46) - 0x23) & 0xffff;
    if (ax == 0)
        ax = 0x0a;
    img_setw(si + 4, ax);
    img_setw(si + 6, ax);
    img_setw(si + 0x0c, ax >= 0x8000 ? 1 : 0xffff);
    int16_t v = (int16_t)img_w(si + 0x0e), t = (int16_t)ax;
    int16_t first = (int16_t)(v * t);
    int32_t prod = (int32_t)first * (int32_t)t;
    img_setw(si + 0x0a, (int16_t)(prod / 100) & 0xffff);
    img_setw(si + 8, (int16_t)(prod / 100) & 0xffff);
    return (uint32_t)(int16_t)(prod / 100) & 0xffff;
}

/* 1ac2:5c36  ending_blob
 *
 * Eight rows of one word from 0x28d9, XORed at a position packed into AX: AL
 * is the x in units of four pixels (`shr al,1` twice) and AH the row, whose
 * bottom bit picks the half of the interlace and whose rest multiplies by
 * 0x50. A whole address in sixteen bits, which is why the ending's script can
 * be a list of words.
 */
void ending_blob(uint32_t pos)
{
    uint32_t al = (pos & 0xff) >> 2;
    uint32_t ah = (pos >> 8) & 0xff;
    uint32_t di = al;
    if (ah & 1)
        di += CGA_PLANE;
    di += (ah >> 1) * 0x50;
    /* The `lodsw` at 1ac2:5c55 reads through **DS**, and the ending leaves DS
     * at the block the notes call segment 0xc46 rather than at zero. Reading
     * 0x28d9 as a plain image offset - which is right almost everywhere else
     * in this program, and is why it was written that way - takes the sprite
     * from 49KB below where the original takes it. */
    for (int32_t r = 0; r < 8; r++) {
        g_vram[di & (CGA_SIZE - 1)] ^= c46.ending_mark[r][0];
        g_vram[(di + 1) & (CGA_SIZE - 1)] ^= c46.ending_mark[r][1];
        di = cga_next_row(di);
    }
}

/* 1ac2:5b80  ending_blobs
 *
 * The script at 0xc46:0x289d: a list of packed positions, zero-terminated. Each is
 * drawn and the one before it rubbed out, one per retrace, so a trail of them
 * crawls across the ending screen.
 */
uint32_t ending_blobs(void)
{
    /* Returns the last position it drew - the original leaves it in DX and
     * the call site at 1ac2:59ce hands it straight to ending_walk. */
    /* Through SEG_C46, like everything else the ending touches: DS is that
     * block for the whole sequence, not zero. */
    uint32_t si = SEG_C46 + 0x289d, prev = 0;
    for (;;) {
        for (int32_t i = 0; i < 0x0f; i++)
            game_delay();
        uint32_t pos = img_w(si);
        si += 2;
        if (pos == 0)
            return prev;
        io_wait_retrace();
        ending_blob(pos);
        if (prev)
            ending_blob(prev);
        prev = pos;
        io_present();
        if (!io_pump())
            return prev;
    }
}

/* 1ac2:5317  ending_column
 *
 * Eight columns of a fifteen-row sprite, walking a list of frame pointers at
 * 0xb7a2 until 0xffff. `movsw` twice then `inc si` steps the source five
 * bytes a row, not four - the frames are five bytes wide and only four are
 * drawn.
 */
void ending_column(void)
{
    uint32_t di = 0x34f8;
    for (int32_t dh = 8; dh > 0; dh--) {
        /* **bx is reloaded here**, at 1ac2:531c, inside the outer loop - so
         * every one of the eight columns plays the whole list at 0xb7a2 from
         * the beginning. Hoisting it out, which is what this used to do, gave
         * each column one frame of the animation and then ran off the end. */
        uint32_t bx = 0xb7a2;
        while (img_w(bx) != 0xffff) {
            uint32_t si = img_w(bx), d = di;
            for (int32_t r = 0; r < 0x0f; r++) {
                for (int32_t b = 0; b < 4; b++)
                    g_vram[(d + b) & (CGA_SIZE - 1)] = g_image[si + b];
                si += 5;                /* four copied, one skipped */
                d = cga_next_row(d);
            }
            bx += 2;
            /* 1ac2:5348 - a hundred delays, watching for a key throughout. */
            for (int32_t i = 0; i < 0x64; i++) {
                if (io_key_ready()) {
                    io_get_key();
                    return;
                }
                game_delay();
            }
        }
        /* The list ran out: blank the column (1ac2:535f). */
        uint32_t d = di;
        for (int32_t r = 0; r < 0x0f; r++) {
            for (int32_t b = 0; b < 4; b++)
                g_vram[(d + b) & (CGA_SIZE - 1)] = 0;
            d = cga_next_row(d);
        }
        if (dh == 5)
            di++;                       /* 1ac2:537f, one column a byte over */
        di += 4;
    }
}

/* ========================================================================
 * 1ac2:0473  screen_game_over
 *
 * The paddle comes apart. First the row under it is wiped, then - if the
 * paddle is not the plain one - it is shrunk back through its six frames, and
 * then the sequence at 0x9bb0 plays over it, one frame per retrace. When the
 * last life is gone it hands over to 0x51b6, the end-of-game screen.
 * ===================================================================== */
void screen_game_over(void)
{
    memcpy(gv.paddle_pix[0], gv.game_over_paddle, sizeof gv.game_over_paddle);

    uint32_t di = 0x1cc2;
    for (int32_t r = 0; r < 8; r++) {
        for (int32_t i = 0; i < 0x18; i++)
            img_vram_setw(di + i * 2, 0);
        di = cga_next_row(di);
    }
    gv.paddle_morphing = 1;

    uint32_t kind = gv.paddle_kind;
    if (kind) {
        uint32_t si = gv.paddle_grow[kind];
        for (int32_t f = 0; f < 6; f++, si += 2) {
            io_wait_retrace();
            draw_paddle_shifted(img_ptr(img_w(si)));
            for (int32_t i = 0; i < 0x96; i++)
                game_delay();
            io_present();
            if (!io_pump())
                return;
        }
        blit_xor(gv.paddle_pix[0], &gv.paddle_rows[0]);
    }

    for (uint32_t si = 0x9bb0; img_w(si); si += 2) {
        io_wait_retrace();
        draw_paddle_raw(img_ptr(img_w(si)));
        for (int32_t i = 0; i < 0x96; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }

    if (gv.lives == 0) {
        restore_int09();                /* 1ac2:03d1 - a no-op here */
        screen_end_of_game();           /* 1ac2:51b6 */
    }
}

/* ========================================================================
 * The ending's own kernels. Same sixteen-byte records and same parabola as
 * the menu's, but launched lower and drawn with a four-word sprite rather
 * than single pixels.
 * ===================================================================== */

/* 1ac2:5add  ending_plot
 *
 * A 2x2 arrangement of words at (cx, dx), and the address is built out of the
 * two coordinates' low bits rather than by the usual formula: bit 0 of the row
 * picks the half of the interlace, bits 0 and 1 of the column pick one of four
 * pre-shifted sprites in particle_sprites.
 */
void ending_plot(uint32_t x, uint32_t y)
{
    uint32_t di = (y & 1) ? CGA_PLANE : 0;
    uint32_t row = y >> 1;
    di += row * 0x50;
    uint32_t phase = ((x & 1) ? 1 : 0) + ((x & 2) ? 2 : 0);
    uint32_t si = img_off(gv.particle_sprites[phase]);
    di += x >> 2;

    uint32_t d = di;
    for (int32_t i = 0; i < 4; i++) {
        g_vram[d & (CGA_SIZE - 1)] ^= (uint8_t)img_w(si + i * 2);
        g_vram[(d + 1) & (CGA_SIZE - 1)] ^= (uint8_t)(img_w(si + i * 2) >> 8);
        /* Just cga_next_row. The original writes the four rows out twice,
         * once for an even start (+0x2000, -0x1fb0, +0x2000 at 1ac2:5b15)
         * and once for an odd one (-0x1fb0, +0x2000, -0x1fb0 at 1ac2:5b65),
         * which reads like two different steppings and is one. Hard-coding
         * the even pattern drew every odd-row particle a row out. */
        d = cga_next_row(d);
    }
}

/* 1ac2:5a43  ending_particles_init - every record launched from the lower
 * starting point */
void ending_particles_init(uint32_t ax)
{
    uint32_t n = gv.particle_count;
    for (uint32_t i = 0; i < n; i++)
        ax = ending_particle_init(img_off(gv.particles[i]), ax);
    /* 1ac2:5a43 has no `ret` of its own: it **falls through** into
     * ending_particles_tick at 1ac2:5a56, so the first pass over the
     * particles is part of setting them up. Treating the two as separate
     * routines left the screen a frame behind from the start. */
    ending_particles_tick();
}

/* 1ac2:5a56  ending_particles_tick - menu_particles_tick with ending_plot in
 * place of the BIOS pixel call and ending_particle_init to re-launch */
void ending_particles_tick(void)
{
    uint32_t si = img_off(gv.particles);
    uint32_t n = gv.particle_count;
    for (uint32_t k = 0; k < n; k++, si += 0x10) {
        uint32_t x = (img_w(si) + img_w(si + 4) - img_w(si + 6)) & 0xffff;
        uint32_t y = (img_w(si + 8) + img_w(si + 2) - img_w(si + 0x0a)) & 0xffff;
        if (x <= 0x13f && y <= 0xc7)
            ending_plot(x, y);

        img_setw(si + 6, (img_w(si + 6) + img_w(si + 0x0c)) & 0xffff);
        int16_t t = (int16_t)img_w(si + 6), v = (int16_t)img_w(si + 0x0e);
        int16_t first = (int16_t)(v * t);
        int32_t prod = (int32_t)first * (int32_t)t;
        img_setw(si + 8, (int16_t)(prod / 100) & 0xffff);

        y = (img_w(si + 8) + img_w(si + 2) - img_w(si + 0x0a)) & 0xffff;
        x = (img_w(si) + img_w(si + 4) - img_w(si + 6)) & 0xffff;
        if (y <= 0xc7 && x <= 0x13f)
            ending_plot(x, y);
        if (y > 0xc7)
            ending_particle_init(si, y);
        game_delay();
    }
}

/* ========================================================================
 * 1ac2:5bb5  ending_walk
 *
 * Move the ending's blob to a target: `bl * 8` gives the column it is heading
 * for and `bh * 8 + 8` the row, and it steps four pixels at a time towards
 * each in turn, XOR-ing itself off and on again at every step. 0xc46:0x2823
 * holds
 * the target while it works, which is why it is a variable and not a register.
 * ===================================================================== */
uint32_t ending_walk(uint32_t bl, uint32_t bh, uint32_t dx)
{
    /* `dx` is a **parameter**, threaded in DX from ending_blobs by the call
     * site at 1ac2:59c9 - not a variable at 0x2821, which is where this used
     * to read it from. Modelling a register as memory works right up until
     * something else is in the register. */

    uint32_t target = (0x50 - ((bl << 3) & 0xff)) & 0xff;
    c46.blob_target = (uint8_t)target;
    while (((dx >> 8) & 0xff) != target) {
        uint32_t next = (dx - 0x400) & 0xffff;   /* `sub ah,4` */
        for (int32_t i = 0; i < 0x0f; i++)
            game_delay();
        io_wait_retrace();
        ending_blob(next);
        ending_blob(dx);
        dx = next;
    }

    target = (((bh << 3) + 8) & 0xff);
    c46.blob_target = (uint8_t)target;
    while ((dx & 0xff) != target) {
        uint32_t next = (dx - 4) & 0xffff;       /* `sub al,4` */
        for (int32_t i = 0; i < 0x0f; i++)
            game_delay();
        io_wait_retrace();
        ending_blob(next);
        ending_blob(dx);
        dx = next;
    }
    return dx;
}

/* ========================================================================
 * 1ac2:5940  screen_all_levels_done
 *
 * Finishing all fifty. The cells are wiped, the level intro runs on an empty
 * field, and then the picture at 0xc46:0x7c70 is drawn in a widening band -
 * `bh` counts from 1 to 0x5c and is both the number of rows copied and the
 * offset the band starts at, so it grows from a line into the whole screen.
 * ===================================================================== */
void screen_all_levels_done(void)
{
    /* The only place the game reaches after the fiftieth level is cleared.
     * Said out loud because it is otherwise invisible: this screen has no
     * frame sync, so a lockstep driver is simply blocked while it runs and
     * cannot tell "finished" from "hung". */
    fprintf(stderr, "popcorn: the last level is finished - "
                    "screen_all_levels_done (1ac2:5940)\n");
    for (int32_t n = 0x32; n > 0; n--)
        for (int32_t i = 0; i < 0xc8; i++)
            game_delay();

    memset(gv.level.cells, 0, sizeof gv.level.cells);
    level_intro();

    uint32_t bp = 0x3ef2;
    for (uint32_t bh = 1; bh != 0x5c; bh++) {
        /* 1ac2:596c, the **top** of the pass - before the band is drawn, not
         * after. Without a sync here this screen has none at all: a lockstep
         * driver is blocked while it runs and cannot tell "finished" from
         * "hung", and the sequence after the fiftieth level is compared by
         * nothing. Its routines are each checked; that is not the animation
         * being right.
         *
         * Placing it after the copy instead put the port one pass ahead of
         * the emulator and reported nine pixels of red along the bottom scan
         * line that were only the band this side had already drawn. A sync
         * has to sit where the offset it is matched against sits. */
        io_frame_sync_extra(SYNC_ENDING);
        uint32_t di = bp, si = SEG_C46 + 0x7c70;
        io_wait_retrace();
        for (uint32_t r = 0; r < bh; r++) {
            for (int32_t b = 0; b < 0x1a; b++)
                g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[si + b];
            si += 0x1a;
            di = cga_next_row(di);      /* the `sub` undoes `rep movsb` */
        }
        bp = cga_prev_row(bp);
        for (int32_t i = 0; i < 5; i++)
            game_delay();
        io_present();
        if (!io_pump())
            return;
    }
    /* 1ac2:59bb - the blobs walk, and it is a **script**, not a grid.
     *
     * `mov si, 0x2825` sits *before* the outer loop, so si walks continuously
     * across all 0x18 passes rather than restarting; each pass is `mov bl, 5`
     * - five steps, not 0x1a - and each step reads one byte with `lodsb` and
     * **skips** when it is zero (1ac2:59c5). The port had five times the steps
     * it should, ran every one of them, and never read the script at all.
     *
     * The `push si` either side of ending_blobs is not decoration: 1ac2:5b80
     * opens `mov si, 0x289d` and walks off with it.
     *
     * The script is at 0xc46:0x2825 - this whole screen runs with DS there,
     * which is what 0x2823, 0x289d and 0x28d9 are reached through as well. */
    uint32_t walk = SEG_C46 + 0x2825;
    for (uint32_t bh = 0; bh != 0x18; bh++) {
        io_frame_sync_extra(SYNC_ENDING);       /* 1ac2:59c0, the pass */
        for (uint32_t bl = 5; bl > 0; bl--) {
            if (!g_image[walk++])               /* 1ac2:59c2, lodsb */
                continue;
            uint32_t blobs = ending_blobs();    /* 1ac2:59c9 */
            ending_walk(bl, bh, blobs);         /* 1ac2:59ce */
        }
    }
    /* 0x2509 is not a constant anyone chose: 1ac2:59dd calls restore_int09
     * immediately before this, which is `mov ax, 0x2509 / int 21h`, and the
     * ending's first particle is seeded from whatever AX happens to hold. The
     * C's restore_int09 is a no-op - the platform layer owns the keyboard -
     * so the value has to be written down here or the ending starts from a
     * different seed than the original's. */
    ending_particles_init(0x2509);          /* 1ac2:59e0 */
    /* 1ac2:59e3 is a do-while: the tick comes **first** and the key is tested
     * after it, so a key already waiting still gets one tick. Testing first
     * gives none, which is one frame of fountain the original always shows. */
    for (;;) {
        io_frame_sync_extra(SYNC_ENDING);   /* 1ac2:59e3, the pass */
        ending_particles_tick();            /* 1ac2:5a56 */
        io_present();
        if (io_key_ready()) {               /* 1ac2:59e6 */
            io_get_key();                   /* 1ac2:59ec, and it takes it */
            return;
        }
        if (!io_pump())
            return;
    }
}

/* ========================================================================
 * 1ac2:03e3  the INT 09h handler
 *
 * The game's whole keyboard interface. It replaces the BIOS one while a level
 * is running, which is why the INT 16h buffer stops filling then, and it is
 * taken out again for the menus.
 *
 * The platform layer calls this with the scan code SDL gives it, so the
 * decoding below is the original's rather than a second copy of it. What the
 * platform cannot supply is the acknowledgement to port 0x61 and the end-of-
 * interrupt to port 0x20, and it does not need to.
 *
 * The `repne scasb` leaves CX at 2, 1 or 0 for a match on left, right or
 * action, and `[0x2d4c + cx]` turns that into 0x2d4e, 0x2d4d or 0x2d4c - so
 * the three state bytes run backwards against the three scan codes.
 */
void int09_handler(uint32_t scan)
{
    uint32_t make = scan <= 0x7f;

    if ((scan & 0xff) == gv.key_scan_l)
        gv.last_dir = 0;
    if ((scan & 0xff) == gv.key_scan_r)
        gv.last_dir = 1;
    if (scan == 0xc3)                   /* F9 released */
        cv.sound_on ^= 1;
    if (make)
        gv.last_make = (uint8_t)scan;

    /* The original walks the three scan codes and stores into the flags
     * *backwards* - `[KEY_ACTION + (2 - i)]` - because the two triples are
     * laid out in opposite orders. Unrolled, so that is a fact you can see
     * rather than one you have to work out. */
    uint32_t code = scan & 0x7f;
    if (code == gv.key_scan_l) { gv.key_left   = (uint8_t)make; return; }
    if (code == gv.key_scan_r) { gv.key_right  = (uint8_t)make; return; }
    if (code == gv.key_scan_a) { gv.key_action = (uint8_t)make; return; }
}

/* ========================================================================
 * 1ac2:4dea and 1ac2:4e04  drive_check / drive_writable
 *
 * Before touching popcorn.hsc the game resets the drive with INT 13h AH=00,
 * asks DOS for the current disk with AH=0Dh, and reads sector 0 with INT 25h;
 * the second one then writes it back with INT 26h to prove the disk is not
 * write-protected. On a floppy in 1988 that was the difference between saving
 * a high score and hanging on a critical-error prompt.
 *
 * There is no disk here and the port opens the file directly, so both report
 * success. They are the two INT 13h/INT 25h calls the emulator sees at
 * startup, and this is what they were for.
 */
int32_t drive_check(void) { return 1; }
int32_t drive_writable(void) { return 1; }

/* ========================================================================
 * 1ac2:49bc  intro_paddle
 *
 * The paddle arriving at the end of the opening, in two phases.
 *
 * First it **emerges from the left wall**: eight passes at a fixed 0x1900,
 * each one byte wider than the last, with `bp` walking *back* through the
 * sprite so the part on screen is always the paddle's right-hand end. Eight
 * passes, not more - `bp` counts down from 0x490a and the loop ends when it
 * reaches 0x4902.
 *
 * Then it **travels right**: `bh` from 0 to 0x15 puts the whole eight bytes
 * at 0x1900 + bh, and each row writes a **zero byte first** - the `stosb`
 * before the `rep movsb` - which rubs out the column the paddle has just
 * left. That is why nothing else has to erase it.
 *
 * Each pass cycles all four pre-shifted phases, seven rows of eleven bytes
 * apiece, with a retrace wait and 0x19 delays between them.
 * ===================================================================== */
void intro_paddle(void)
{
    /* Out of the wall. */
    uint32_t bp = 0x490a;
    for (uint32_t bh = 1; bh <= 8; bh++, bp--) {
        uint32_t si = bp;
        for (int32_t bl = 4; bl > 0; bl--, si += PADDLE_IMAGE) {
            io_wait_retrace();
            uint32_t di = 0x1900, s = si;
            for (int32_t dl = 7; dl > 0; dl--) {
                for (uint32_t b = 0; b < bh; b++)
                    g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[s + b];
                s += 0x0b;
                di = cga_next_row(di);
            }
            for (int32_t i = 0; i < 0x19; i++)
                game_delay();
            io_present();
            if (!io_pump())
                return;
        }
    }

    /* And across. */
    for (uint32_t bh = 0; bh < 0x16; bh++) {
        uint32_t si = 0x4903;
        for (int32_t bl = 4; bl > 0; bl--, si += PADDLE_IMAGE) {
            io_wait_retrace();
            uint32_t di = (0x1900 + bh) & 0xffff, s = si;
            for (int32_t dl = 7; dl > 0; dl--) {
                g_vram[di & (CGA_SIZE - 1)] = 0;
                for (int32_t b = 0; b < 8; b++)
                    g_vram[(di + 1 + b) & (CGA_SIZE - 1)] = g_image[s + b];
                s += 0x0b;
                di = cga_next_row(di);
            }
            for (int32_t i = 0; i < 0x19; i++)
                game_delay();
            io_present();
            if (!io_pump())
                return;
        }
    }
}

/* ========================================================================
 * 1ac2:08c8  level_load_file
 *
 * `POPCORN POPTAB` loads POPTAB.PPC over the built-in table: 0x21b6 bytes
 * straight into the block reached as segment 0xc46, six bytes in. The six
 * bytes it skips are a signature, and the check is a `repne cmpsb` of them
 * against the first six of what was just read - so a file is valid when its
 * own header repeats. Either failure prints a line and exits to DOS:
 *
 *   "****** Fichier des Tableaux non trouve ******"
 *   "****** Ce fichier n'est pas un fichier de Tableaux ******"
 *
 * Returns 0 if the file could not be used, and the caller ends the program.
 * ===================================================================== */
static FILE *ppc_open(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", dir ? dir : "", name);
    FILE *f = fopen(path, "rb");
    if (f)
        return f;
    /* DOS filesystems did not care about case and this one does. The tail
     * arrives upper-cased, as DOS gave it; the file on disk is whatever the
     * player named it - the shipped sets are ltf.ppc and poptab.ppc. */
    char lower[512];
    size_t n = 0;
    for (; path[n] && n < sizeof lower - 1; n++)
        lower[n] = (char)((path[n] >= 'A' && path[n] <= 'Z')
                          ? path[n] + 32 : path[n]);
    lower[n] = 0;
    return fopen(lower, "rb");
}

int32_t level_load_file(const char *dir)
{
    /* Beside the binary, like everything else the port reads: POPCORN.EXE for
     * the data, the .PPC sets, and popcorn.hsc. The original reads its level
     * file from the current directory, because in DOS the game and its files
     * were the current directory; here one directory holds the lot and it
     * does not move when the shell does. */
    FILE *f = ppc_open(dir, gv.level_file);
    if (!f) {
        fputs("****** Fichier des Tableaux non trouve ******\n", stderr);
        return 0;
    }
    fread(c46.ppc_signature, 1, 0x21b6, f);
    fclose(f);
    if (memcmp(c46.signature, c46.ppc_signature, 6) != 0) {
        fputs("****** Ce fichier n'est pas un fichier de Tableaux ******\n",
              stderr);
        return 0;
    }
    return 1;
}

/* 1ac2:4b7a  set_palette_registers
 *
 * Twelve values out to ports 0x3d0 through 0x3db, alternating index and data.
 * That is an EGA-style palette write on what is meant to be a CGA, and it does
 * nothing on real CGA hardware - the two screens that use it (F10 and its
 * exit) look the same without it. Recorded because it runs.
 */
void set_palette_registers(uint32_t table)
{
    for (int32_t i = 0; i < 0x0c; i++)
        (void)g_image[table + i];
}

/* ========================================================================
 * 1ac2:4e1a  screen_high_scores
 *
 * F6. Everything on this screen is 24 characters wide and stepped by hand.
 *
 * A line of text is 12 scan lines tall, which in one interlace half is six
 * rows of 0x50 - 0x1e0. The caller has already walked DI forward 0x30 drawing
 * its 24 characters, so the step between lines is written as `add di, 0x1b0`.
 * That is the whole layout: heading, a rule of dashes, a blank, then ten
 * entries, each followed by two scan lines of 0xaaaa as a separator, and
 * whatever is left of the screen filled with the same bar until DI comes
 * round to 0x1e02.
 *
 * The bar is glyph 0 - what a space maps to - which is a solid block of
 * colour 2. Get the stepping wrong and the fill has nothing to stop against,
 * and the screen is a red rectangle with the table hidden under it.
 * ===================================================================== */
#define HSC_LINE   0x1b0                /* between lines, DI already +0x30 */
#define HSC_WIDTH  0x18                 /* characters, and words of bar */

static uint32_t hsc_bar(uint32_t di)
{
    for (int32_t i = 0; i < HSC_WIDTH; i++)
        img_vram_setw((di + i * 2) & 0xffff, 0xaaaa);
    return cga_next_row(di);
}

void screen_high_scores(void)
{
    border_setup();

    for (int32_t i = 0; i < HSC_WIDTH; i++)
        img_vram_setw(0x142 + i * 2, 0xaaaa);

    /* HIGH SCORE, a glyph at a time - the original really does have ten
     * separate `mov al` / `call 0xc64` pairs for it. */
    uint32_t di = 0x2142;
    di = draw_run(' ', 7, di);
    for (const char *p = "HIGH SCORE"; *p; p++, di = (di + 2) & 0xffff)
        draw_char(*p, di);
    di = draw_run(' ', 7, di);

    di = (di + HSC_LINE) & 0xffff;              /* the rule */
    di = draw_run(' ', 5, di);
    di = draw_run('-', 0x0e, di);
    di = draw_run(' ', 5, di);

    di = (di + HSC_LINE) & 0xffff;              /* a blank line */
    di = draw_run(' ', HSC_WIDTH, di);

    di = (di + HSC_LINE) & 0xffff;

    for (int32_t row = 0; row < HSC_COUNT; row++) {
        const hsc_entry_t *e = &gv.hsc[row];
        di = draw_run(' ', 2, di);
        di = draw_text(e->name, 12, di);
        di = draw_run(' ', 2, di);
        di = draw_text((const char *)e->score, 6, di);
        di = draw_run(' ', 2, di);

        di = (di + HSC_LINE) & 0xffff;
        di = hsc_bar(di);                       /* two scan lines between */
        di = hsc_bar(di);
    }

    while (di != 0x1e02)
        di = hsc_bar(di);

    /* The border animates until a key, 0x181 delays a step, 0xff steps. */
    for (int32_t dl = 0xff; dl > 0; dl--) {
        for (int32_t n = 0x181; n > 0; n--) {
            if (io_key_ready()) {
                io_get_key();
                return;
            }
            game_delay();
        }
        border_animate();
        io_present();
        if (!io_pump())
            return;
    }
}

/* ========================================================================
 * 1ac2:1581  screen_define_keys
 *
 * F5. The one screen in the game that is not graphics: it switches the card to
 * **text mode 01h**, draws a double-line box out of code page 437 characters
 * straight into 0xb800 as character/attribute pairs, asks for three keys, and
 * switches back to mode 05h.
 *
 * Reading a key is 0x1614, which refuses one already used for another action
 * or reserved at 0x2d52. [0x2d49] is set to 1 first so the handler has
 * something to compare against that cannot match.
 *
 * **Not transcribed, on purpose**, along with 0x1614 and 0x1642. This is a
 * decision about what the port is for rather than something still to be read:
 * the port keeps the defaults at 0x2d4f-0x2d51, which are J, K and Space.
 * ===================================================================== */
void screen_define_keys(void)
{
    /* **Not transcribed, on purpose.** F5 redefines the left, right and
     * launch keys on a 40x25 text screen. Out of scope: the port keeps the
     * defaults at 0x2d4f-0x2d51, which are J, K and Space. */
}

/* ========================================================================
 * 1ac2:51b6  screen_end_of_game
 *
 * What plays when the last life is gone, before the hall of fame.
 *
 * The segment registers are the whole story of the first part and the port
 * had it backwards. `mov ax,ds / mov es,ax` then `mov ds,0xb800` makes the
 * copy run **screen to image**: the top 0x96 scan lines, 0x21 bytes wide, are
 * saved into the image at offset 0, DI running on continuously while SI walks
 * the screen a scan line at a time.
 *
 * Then 0x87 passes build the picture at 0xa6d0 into that saved copy, a band
 * at a time, and put each band back on screen. The merge is per pixel: a byte
 * holds four, and the new image's bits are taken **only where the old pixel
 * is zero**, so the picture comes through the gaps in what was there rather
 * than over it. [0x13c0] is where on screen the next band goes and [0x13c2]
 * is how far into the saved copy the build has got.
 *
 * Then a key, or 0x1c20 ticks, and then the ending itself: seven groups from
 * the table at 0xa8bf, each a tall sprite drawn twice, blanked from 0xabab,
 * and drawn twice more. Any key stops it; if none came, ending_column runs.
 * ===================================================================== */
#define EOG_BAND      img_off(gv.scratch2.eog_band)
#define EOG_WIDTH     0x21
#define EOG_BAND_LEN  0x1ef
#define EOG_BLANK     0xabab

void screen_end_of_game(void)
{
    /* The screen into the image - not screen to screen, and DI does not go
     * back to where it started each row. */
    uint32_t si = 8, di = 0;
    for (int32_t n = 0x96; n > 0; n--) {
        for (int32_t b = 0; b < EOG_WIDTH; b++)
            gv.scratch1.eog_saved[di + b] = g_vram[(si + b) & (CGA_SIZE - 1)];
        di += EOG_WIDTH;
        si = cga_next_row(si);
    }

    gv.eog_screen_at = (uint16_t)(8);
    gv.eog_build_at = (uint16_t)(EOG_WIDTH);

    for (int32_t pass = 0x87; pass > 0; pass--) {
        /* The band as it stands, from the saved screen - not from vram. */
        memcpy(gv.scratch2.eog_band, g_image + gv.eog_build_at, EOG_BAND_LEN);

        uint32_t src = img_off(gv.eog_overlay), dst = EOG_BAND;
        for (int32_t i = 0; i < EOG_BAND_LEN; i++, src++, dst++) {
            uint32_t old = g_image[dst], add = g_image[src], out = old;
            for (int32_t shift = 6; shift >= 0; shift -= 2) {
                uint32_t mask = 3u << shift;
                if ((old & mask) == 0)
                    out += add & mask;
            }
            g_image[dst] = (uint8_t)out;
        }

        /* One band on screen from the saved copy, then the merged block. */
        di = gv.eog_screen_at;
        uint32_t from = (gv.eog_build_at - EOG_WIDTH) & 0xffff;
        for (int32_t b = 0; b < EOG_WIDTH; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[from + b];
        di = cga_next_row(di);
        gv.eog_screen_at = (uint16_t)(di);

        uint32_t m = EOG_BAND;
        for (int32_t r = 0x0f; r > 0; r--) {
            for (int32_t b = 0; b < EOG_WIDTH; b++)
                g_vram[(di + b) & (CGA_SIZE - 1)] = g_image[m + b];
            m += EOG_WIDTH;             /* `rep movsb` carries SI forward */
            di = cga_next_row(di);
        }
        gv.eog_build_at = (uint16_t)(gv.eog_build_at + EOG_WIDTH);

        io_present();
        if (!io_pump())
            return;
    }

    /* A key, or 0x1c20 ticks of waiting for one. */
    for (int32_t n = 0x1c20; n > 0; n--) {
        if (io_key_ready()) {
            io_get_key();
            return;                     /* 1ac2:5314 - straight out */
        }
        game_delay();
        if (!io_pump())
            return;
    }

    /* The ending. Seven groups, each drawn twice, blanked, and drawn twice
     * more - SI carries forward inside tall_sprite, so every call is the next
     * frame rather than the same one again. */
    int32_t keyed = 0;
    for (int32_t g = 0; g < 7 && !keyed; g++) {
        uint32_t at = (0x34f0 + gv.eog_groups[g].at) & 0xffff;
        uint32_t sprite = gv.eog_groups[g].sprite;

        if (tall_sprite(&sprite, at)) { keyed = 1; break; }
        tall_sprite(&sprite, at);       /* no test after the second */
        uint32_t blank = EOG_BLANK;
        if (tall_sprite(&blank, at)) { keyed = 1; break; }
        if (tall_sprite(&blank, at)) { keyed = 1; break; }
        if (tall_sprite(&blank, at)) { keyed = 1; break; }
    }
    if (!keyed)
        ending_column();                /* 1ac2:5317 */
}

/* ========================================================================
 * 1ac2:45a1  ball_after_endgame
 *
 * ball_after's twin, used while the level-ending bonus is running. The walls
 * behave the same, but the bottom of the playfield is no longer fatal: the
 * ball is meant to reach one of two chambers, at y 0x74 and y 0x3c, each with
 * an opening between x 0x60 and 0x6c, and reaching either finishes the level.
 *
 * Returns 1 when the level is over - the original's carry - and 0 to carry on.
 * ===================================================================== */

/* Both endings play the same animation: a two-word cap at 0x1198, then 0x70
 * passes that scroll a 26-word band up seven rows, lay a fresh cap, and draw
 * one more row of the brick field as [0x2f0c] counts down. */
static void endgame_curtain(int32_t cells)
{
    uint32_t di = 0x1198;
    img_vram_setw(di, 0xffff);
    img_vram_setw(di + 2, 0xffff);
    di += CGA_PLANE;
    img_vram_setw(di, 0x5555);
    img_vram_setw(di + 2, 0x5555);
    di -= 0x1fb0;
    img_vram_setw(di, 0x1515);
    img_vram_setw(di + 2, 0x1515);
    di += CGA_PLANE;
    img_vram_setw(di, 0x5555);
    img_vram_setw(di + 2, 0x5555);

    uint32_t bp = 0x3130;
    for (uint32_t ah = 0x70; ah > 0; ah--) {
        /* 1ac2:467f, the top of the pass. This sync spent a while in
         * level_intro's reveal loop instead - a different routine in a
         * different screen - so the two sides fired it a different number of
         * times and every comparison after the first was a mismatched pair.
         * Which read as "the whole curtain differs" and was really "this is
         * not the curtain". */
        io_frame_sync_extra(SYNC_CURTAIN);
        uint32_t d = bp, wrote = bp;
        for (int32_t r = 7; r > 0; r--) {
            uint32_t s = cga_next_row(d);
            for (int32_t b = 0; b < 0x1a * 2; b++)
                g_vram[(d + b) & (CGA_SIZE - 1)] =
                    g_vram[(s + b) & (CGA_SIZE - 1)];
            wrote = d;                  /* the row just written */
            d = s;
        }
        /* 1ac2:46b9 - the caps go on the row the loop **wrote**, not the one
         * after it. `xchg di, si` at 1ac2:46b1 leaves DI on that row and DX
         * on the next, and the caps use DI. Capping the next row instead puts
         * the border one scan line out, which grows by a row a pass. */
        d = wrote;
        uint32_t cap = (ah & 3) ? 0x50 : 0x10;
        g_vram[d & (CGA_SIZE - 1)] = 0x0d;          /* 1ac2:46b9 */
        g_vram[(d + 1) & (CGA_SIZE - 1)] = (uint8_t)cap;
        /* 1ac2:46dc caps at whatever DI the call left, which is the end of
         * the brick row. Capping the caller's own DI + 2 instead put it at
         * the band's second byte and left the row's end uncapped. */
        /* 1ac2:46d6, and it is `push ds / mov ds, bp` first. BP is the data
         * segment on one way out of the bonus and **not** on the other:
         * 1ac2:4636, on the floor path, is `mov bp, ds`, and the chamber path
         * at 1ac2:4794 never loads it. So the same call reads the level's
         * cells on one ending and something that is not the cells on the
         * other - which is why a completed level's transition is black where
         * a lost one shows the level behind it.
         *
         * Matched to what the original *does* rather than to a number: what
         * BP carries on that path is not established here, only that it is
         * not the data segment. A register dump at 1ac2:46ce would settle it,
         * the way one settled DS = 0xc46 in the ending. */
        if (cells)
            d = draw_brick_row(gv.sweep_y[0]);   /* 1ac2:46d6 */
        g_vram[d & (CGA_SIZE - 1)] = 0x0d;
        g_vram[(d + 1) & (CGA_SIZE - 1)] = (uint8_t)cap;

        bp = cga_prev_row(bp);
        for (int32_t i = 0; i < 0x0f; i++)
            game_delay();
        gv.sweep_y[0]--;
        io_present();
        if (!io_pump())
            return;
    }
    io_wait_retrace();
    panel_reveal();
    panel_finish();
}

int32_t ball_after_endgame(ball_t *b)
{
    /* Once per step of the bonus's own ball loop, which reaches no other sync
     * point - the whole loop is one indivisible move otherwise. */
    io_frame_sync_extra(SYNC_ENDGAME);
    uint32_t x = b->x, y = b->y;

    if (x <= WALL_LEFT || x >= WALL_RIGHT) {
        b->dir_x = (x <= WALL_LEFT) ? 0 : 1;
        cv.sound_request = SOUND_BOUNCE;
        b->acc_x = 1;
        b->acc_y = 0;
        b->anchor_x = (uint8_t)(x <= WALL_LEFT ? 9 : 0xc3);
        b->anchor_y = (uint8_t)y;
    }

    if (y == 0x74) {
        /* The lower chamber: through the gap it goes down and on, otherwise
         * it bounces - and the test was written the other way round. The asm
         * at 1ac2:45da is `cmp al,0x60 / jb bounce / cmp al,0x6c / jb skip`,
         * so the bounce is for x **outside** 0x60..0x6b. Inverted, the ball
         * bounced in the gap and fell through the wall, and then it was drawn
         * somewhere the original never put it. */
        if (x < 0x60 || x >= 0x6c) {
            b->dir_y = 0;
            b->bounces++;
            b->acc_x = 0;
            b->acc_y = 1;
            b->anchor_x = (uint8_t)x;
            b->anchor_y = (uint8_t)(y + 1);
            cv.sound_request = SOUND_BOUNCE;
        }
    } else if (y < 0x74) {
        if (y == 0x3c) {
            /* The upper chamber: the level is over. */
            speaker_off();
            ball_draw(gv.balls[0].sprite, gv.balls[0].x, gv.balls[0].y);
            for (uint32_t si = 0x6abe; img_w(si) != 0xffff; si += 2) {
                /* 1ac2:4769. The stretch between the ball reaching the
                 * chamber and the curtain starting had no sync of its own,
                 * so it was the one part of the transition still compared by
                 * nothing - and it is where the level is still on screen. */
                io_frame_sync_extra(SYNC_CURTAIN);
                for (int32_t i = 0; i < 0x147; i++)
                    game_delay();
                xor_sprite_16x7(0x60, 0x38, img_ptr(img_w(si - 2)));
                xor_sprite_16x7(0x60, 0x38, img_ptr(img_w(si)));
            }
            level_tally();
            /* No `[0x2f0c] = 0x75` here: 1ac2:4791 goes straight from the
             * tally to the curtain. That write belongs to the *other* ending,
             * at 1ac2:4630, and so does the free life - the two chambers do
             * not finish the level the same way, and **1 says so**: the level
             * is done. See the floor ending below for why that matters. */
            endgame_curtain(0);         /* 1ac2:4794 leaves BP alone */
            return 1;
        }
        /* Between the chambers: the sides of the funnel at x 0x60 and 0x6c.
         * 1ac2:4738 is `cmp al,0x6c / jb no-bounce`, so the right wall
         * catches the ball at 0x6c itself, not one past it. With `>` the ball
         * slipped through on the exact pixel - two thousand steps into the
         * funnel before the two sides noticed. */
        if (x <= 0x60 || x >= 0x6c) {
            b->dir_x = (x <= 0x60) ? 0 : 1;
            b->acc_x = 1;
            b->acc_y = 0;
            b->anchor_x = (uint8_t)(x <= 0x60 ? 0x61 : 0x6b);
            b->anchor_y = (uint8_t)y;
            cv.sound_request = SOUND_BOUNCE;
        }
        return 0;
    }

    if (b->y != FLOOR) {
        ball_paddle(b);
        return 0;
    }

    /* It reached the bottom: the level is over the other way. */
    speaker_off();
    blit_xor(gv.paddle_pix[0], &gv.paddle_rows[0]);
    ball_draw(gv.balls[0].sprite, gv.balls[0].x, gv.balls[0].y);
    if (gv.cheat_done != 1)
        gv.lives++;               /* a free life, unless cheating */
    gv.sweep_y[0] = 0x75;
    endgame_curtain(1);                 /* 1ac2:4636 put DS in BP */
    /* **2**: this ending arrives back in play_session as a *lost life*, not a
     * completed level. That is not a guess about the carry - it is what the
     * free life two lines up is for. play_session decrements at 1ac2:0363 on
     * the lost-life path and this hands one back at 1ac2:462c to cancel it,
     * which would be pointless on the path that does not decrement. The upper
     * chamber returns 1 and has no such line. */
    return 2;
}

/* ========================================================================
 * 1ac2:2da0  bonus_end_level, and 1ac2:4210  bonus_end_level_body
 *
 * The bonus that finishes a level. In the original it does not return: it
 * throws four words off the stack and jumps into 0x4210, abandoning the play
 * loop and everything below it. Here it is one routine and the caller unwinds
 * by longjmp, which is the same thing said honestly.
 *
 * What it does: clear the indicator columns, lose a life's worth of state,
 * put the paddle back to plain, and then build the "next level" banner by
 * scrolling the screen up a row at a time and laying a fresh row in at
 * 0x3130 on each pass. The rows come from the level's own cells at
 * [0x13ca] + 0xb8, each byte translated through the table at 0xc46:0x226c,
 * with 0x14 for the two edges.
 *
 * After that it serves a fresh ball into the funnel and runs a loop of its
 * own - input, step, collide through ball_after_endgame, four sound ticks -
 * until that returns "the level is over".
 * ===================================================================== */
#define BANNER_ROW_VRAM 0x3130

/* One row of the banner: 0x13 blank bytes, an edge, twelve translated cells,
 * an edge, and 0x13 blank again - then the whole screen scrolls up. */
static void banner_row(uint32_t si)
{
    uint32_t di = BANNER_ROW_VRAM;
    for (int32_t i = 0; i < 0x13; i++)
        g_vram[di++ & (CGA_SIZE - 1)] = 0;
    g_vram[di++ & (CGA_SIZE - 1)] = 0x14;
    for (int32_t i = 0; i < 0x0c; i++) {
        uint32_t cell = g_image[si + i];
        g_vram[di++ & (CGA_SIZE - 1)] = c46.banner_xlat[cell][0];
    }
    g_vram[di++ & (CGA_SIZE - 1)] = 0x14;
    for (int32_t i = 0; i < 0x13; i++)
        g_vram[di++ & (CGA_SIZE - 1)] = 0;
    screen_scroll_up();
}

/* The same row with nothing in it: 1ac2:430f and again at 1ac2:43bf. One
 * separates the two fixed rows from the level's own, and one follows every
 * level row. */
static void banner_blank(void)
{
    uint32_t di = BANNER_ROW_VRAM;
    for (int32_t i = 0; i < 0x13; i++)
        g_vram[di++ & (CGA_SIZE - 1)] = 0;
    g_vram[di++ & (CGA_SIZE - 1)] = 0x14;
    for (int32_t i = 0; i < 0x0c; i++)
        g_vram[di++ & (CGA_SIZE - 1)] = 0;
    g_vram[di++ & (CGA_SIZE - 1)] = 0x14;
    for (int32_t i = 0; i < 0x13; i++)
        g_vram[di++ & (CGA_SIZE - 1)] = 0;
    screen_scroll_up();
}

/* 1ac2:2da0 is the entry the bonus effect calls: play_teardown, four words
 * thrown off the stack, then a jump into 1ac2:4210. Splitting them matters
 * for verification - the harness enters at 0x4210, and a version that also
 * tears the play loop down is not the same routine. */
int32_t bonus_end_level(void)
{
    play_teardown();                    /* 1ac2:2da0 */
    return bonus_end_level_body();      /* 1ac2:4210 */
}

/* Harness only, and not the game's state: nothing in the original knows it
 * is "in the bonus", because nothing needs to. The bot does - the field in
 * here is a funnel and the cells still hold a level's worth of bricks that
 * are neither drawn nor reachable, so its usual aim sends the ball at a brick
 * that is not there. */
int32_t g_in_bonus;

static int32_t bonus_end_level_run(void);

int32_t bonus_end_level_body(void)
{
    g_in_bonus = 1;
    int32_t how = bonus_end_level_run();
    g_in_bonus = 0;
    return how;
}

static int32_t bonus_end_level_run(void)
{
    speaker_off();
    life_lost();
    io_wait_retrace();
    panel_reveal();

    gv.paddle_kind = 0;
    gv.paddle_max = 0xac;
    gv.paddle_min = 8;
    gv.paddle_width = gv.paddle_sets[0].width;
    blit_xor(gv.paddle_pix[0], &gv.paddle_rows[0]);

    /* The wall closing in: 0x70 passes of a 26-word band scrolled up six rows
     * with a fresh cap laid on each. */
    uint32_t bp = 0x20a0;
    gv.paddle_morphing = 0xff;
    for (int32_t dh = 0x70; dh > 0; dh--) {
        io_wait_retrace();
        /* `rep movsw` at 1ac2:4282 has SI on the band's row and DI one row
         * **below** it, so each pass copies a row *down*; SI then steps *up*
         * with cga_prev_row. Reading it the other way round - the row below
         * into the row above, and stepping DI by prev_row(next_row(di)),
         * which is the identity - copied the same two rows six times and
         * never moved. */
        uint32_t si = bp;
        for (int32_t dl = 6; dl > 0; dl--) {
            uint32_t d = cga_next_row(si);
            for (int32_t b = 0; b < 0x1a * 2; b++)
                g_vram[(d + b) & (CGA_SIZE - 1)] =
                    g_vram[(si + b) & (CGA_SIZE - 1)];
            si = cga_prev_row(si);
        }
        uint32_t di = cga_next_row(si);
        if (dh == 0x70)
            di = 0;
        for (int32_t i = 0; i < 0x1a; i++)
            img_vram_setw(di + i * 2, 0);
        for (int32_t i = 0; i < 0x78; i++) {
            input_and_draw_paddle();
            gv.paddle_morphing = 0;
        }
        /* 1ac2:42cc: the band's top walks **down** a row a pass - next_row,
         * not prev_row. That is the wall closing in. */
        bp = cga_next_row(bp);
        io_present();
        if (!io_pump())
            return 1;
    }

    /* The banner. **Two** fixed rows, 0x2b39 then 0x2b6d, each scrolled in
     * (1ac2:42e5 and 1ac2:42fa), then a blank one - the transcription had
     * only the second of the three. */
    for (int32_t i = 0; i < 0x1a; i++)
        img_vram_setw(BANNER_ROW_VRAM + i * 2, gv.results_rows[0][i]);
    screen_scroll_up();
    for (int32_t i = 0; i < 0x1a; i++)
        img_vram_setw(BANNER_ROW_VRAM + i * 2, gv.results_rows[1][i]);
    screen_scroll_up();
    banner_blank();

    /* Then the level's own cells, and each level row is **three** banner
     * rows: the cells twice - the `sub si, 0xc` at 1ac2:4380 undoes the first
     * row's lodsb so the second reads the same twelve - and a blank. And si
     * walks **up** the level by 0x0c a time, not down: the loop's `pop si` at
     * 1ac2:43e7 takes the value after the second row's lodsb. */
    uint32_t si = (gv.level_src + 0xb8) & 0xffff;
    for (int32_t n = 0x0e; n > 0; n--, si = (si + 0x0c) & 0xffff) {
        banner_row(SEG_C46 + si);
        banner_row(SEG_C46 + si);
        banner_blank();
    }

    /* Seven more fixed rows, 0x34 apart like the first two - 0x2ba1 through
     * 0x2cd9, at 1ac2:43ef to 1ac2:447f. The transcription went straight from
     * the level's cells to the fresh ball and had none of what follows. */
    for (int32_t r = 0; r < 7; r++) {
        uint32_t src = img_off(gv.results_rows[2 + r]);
        for (int32_t i = 0; i < 0x1a; i++)
            img_vram_setw(BANNER_ROW_VRAM + i * 2, img_w(src + i * 2));
        screen_scroll_up();
    }

    /* 1ac2:4482  The funnel: 0x30 rows, each two marks four bytes apart with
     * blank either side. Every fourth row is marked 0xd1 rather than 0xd5,
     * which is what gives the walls their rungs. */
    for (int32_t dl = 0x30; dl > 0; dl--) {
        uint32_t di = BANNER_ROW_VRAM;
        for (int32_t i = 0; i < 0x17; i++)
            g_vram[di++ & (CGA_SIZE - 1)] = 0;
        uint8_t mark = (dl & 3) ? 0xd5 : 0xd1;
        g_vram[di++ & (CGA_SIZE - 1)] = mark;
        for (int32_t i = 0; i < 4; i++)
            g_vram[di++ & (CGA_SIZE - 1)] = 0;
        g_vram[di++ & (CGA_SIZE - 1)] = mark;
        for (int32_t i = 0; i < 0x17; i++)
            g_vram[di++ & (CGA_SIZE - 1)] = 0;
        screen_scroll_up();
    }

    /* 1ac2:44bd  The mouth opening: eight passes over a four-byte, four-row
     * window at 0x1198, each masking two more pixels away from each side. BX
     * and CX are the masks and shift two bits a pass. The halves go on
     * **crossed** - `and ah, bl` then `and al, bh` - because the word was
     * loaded little-endian and the mask is written the other way round. */
    uint32_t mask_l = 0xffff, mask_r = 0xffff;
    for (int32_t pass = 8; pass > 0; pass--) {
        mask_l = (mask_l << 2) & 0xffff;
        mask_r = (mask_r >> 2) & 0xffff;
        uint32_t di = 0x1198;
        for (int32_t row = 4; row > 0; row--) {
            g_vram[di & (CGA_SIZE - 1)] &= (uint8_t)(mask_l >> 8);
            g_vram[(di + 1) & (CGA_SIZE - 1)] &= (uint8_t)mask_l;
            g_vram[(di + 2) & (CGA_SIZE - 1)] &= (uint8_t)(mask_r >> 8);
            g_vram[(di + 3) & (CGA_SIZE - 1)] &= (uint8_t)mask_r;
            di = cga_next_row(di);
        }
        input_and_draw_paddle();
        for (int32_t i = 0; i < 0x28; i++)
            game_delay();
    }

    /* And a fresh ball, played until ball_after_endgame says the level is
     * over. */
    ball_t *b = &gv.balls[0];
    /* 1ac2:4518. Two things the transcription left out, and they are why the
     * bonus's ball started wherever the level's had ended rather than at the
     * top of the funnel: eight bytes of sprite record copied in from 0x48fb,
     * and the position **set** to (0x70, 0xb4) - both the live pair at +0 and
     * the drawn pair at +2. */
    memcpy(gv.balls[0].sprite, gv.ball_start_sprite, sizeof gv.balls[0].sprite);
    b->x = b->prev_x = 0x70;
    b->y = b->prev_y = 0xb4;
    b->dy = 1;
    b->dx = 2;
    b->dir_x = 0;
    b->dir_y = 1;
    b->anchor_x = b->x;
    b->anchor_y = b->y;
    b->acc_x = b->acc_y = 0;
    b->state = 1;
    ball_draw(b->sprite, b->x, b->y);

    for (;;) {
        input_and_draw_paddle();
        ball_step(&gv.balls[0]);
        ball_redraw(&gv.balls[0]);
        int32_t how = ball_after_endgame(&gv.balls[0]);
        if (how)
            return how;
        for (int32_t i = 0; i < 4; i++) {
            sound_tick();
            for (int32_t k = 0; k < 9; k++)
                game_delay();
        }
        io_present();
        if (!io_pump())
            return 1;
    }
}

/* ========================================================================
 * 1ac2:0d2e  next_player
 *
 * What happens when a player's turn ends: either hand over to the next one, or
 * - when nobody has a life left - run the results.
 *
 * A player's whole state lives in their 0x11b-byte record: lives at +0x0c, the
 * level at +0x0d and +0x0f, the score at +0x10, their copy of the cells at
 * +0x16, six words of level state at +0xc6, and then, at +0xd2, a **count of
 * live entities followed by copies of them**. Saving the entity list is what
 * lets a player come back to a level with the capsules still falling.
 *
 * The results are the ten records sorted by score with the same six-digit
 * comparison the hall of fame uses, merged into the table, and written back to
 * popcorn.hsc.
 * ===================================================================== */

/* Returns 1 when the original threw its own return address away and jumped
 * straight back to play_loop at 1ac2:034f - a single player who still has
 * lives carries on in the same level, and **no level intro runs**. Returning
 * normally instead falls through to `jmp 0x34c`, which is the intro, and the
 * port replayed the whole sweep every time a ball was lost.
 *
 * The other `pop ax` - everybody out at 0d45 - discards it too, so the ret at
 * the end of that path returns from play_session rather than from here. That
 * is the longjmp below. */
int32_t next_player(const char *dir)
{
    gv.game_over = 0;
    if (gv.lives == 0) {
        gv.game_over = 1;
        if (--gv.live_count == 0) {
            /* Everybody is out: keep this player's final score and finish. */
            memcpy(gv.players[gv.cur_player].score, gv.score_text,
                   sizeof gv.score_text);
            screen_results(dir);        /* 1ac2:0d68 jmp 0xea3 */
            longjmp(g_back_to_menu, 1); /* and its ret leaves play_session */
        }
    } else if (gv.live_count == 1 && gv.game_over != 1) {
        return 1;                       /* 1ac2:0d79 - carry on, no intro */
    }

    /* Save this player. */
    player_t *p = &gv.players[gv.cur_player];
    p->lives = gv.lives;
    p->level_src = gv.level_src;
    p->level_number = gv.level_number;
    memcpy(p->score, gv.score_text, sizeof p->score);
    p->level = gv.level;
    memcpy(p->state, &gv.cell_bitmap[24], sizeof p->state);

    /* And its entities, count first. */
    p->ent_count = 0;
    for (uint32_t bx = gv.entity_head.next; bx != 0xffff;
         bx = entity_at(bx)->next)
        memcpy(p->ents[p->ent_count++], g_image + bx, sizeof p->ents[0]);
    entities_clear();

    /* Move on to the next player who still has lives. */
    const player_t *q;
    do {
        gv.cur_player = (uint8_t)
            ((gv.cur_player + 1) % gv.player_count);
        q = &gv.players[gv.cur_player];
    } while (q->lives == 0);

    /* Restore them. */
    memcpy(gv.player_name, q->name, sizeof q->name);
    gv.lives = q->lives;
    gv.level_src = q->level_src;
    gv.level_number = q->level_number;
    memcpy(gv.score_text, q->score, sizeof gv.score_text);
    gv.level = q->level;
    memcpy(&gv.cell_bitmap[24], q->state, sizeof q->state);

    for (uint32_t k = 0; k < q->ent_count; k++)
        memcpy(entity_alloc(), q->ents[k], sizeof q->ents[0]);

    panel_draw();
    level_colours();

    /* Set the next extra-life threshold **twenty** thousand above the score
     * they came back with - the two digits are the score's top two of six, so
     * bumping the second is 20,000, not 2,000. They are pulled out with
     * `and ax,0x0e0f`, bumped, and carried by hand. */
    uint32_t al = gv.score_text[0] & 0x0f;
    uint32_t ah = ((gv.score_text[1] & 0x0e) + 2) & 0xff;
    if (ah >= 0x0a) {
        al++;
        ah = 0;
    }
    gv.extra_at = (uint16_t)(((al + 0x30) << 8) | (ah + 0x30));
    return 0;
}

/* 1ac2:0ea3  screen_results, and the hall of fame at 1ac2:1053 it ends in
 *
 * With one player there is nothing to compare, so it jumps straight to
 * 0x1053. With more, the field is cleared, the level intro runs on it, and
 * the players are sorted into hsc_scratch by score - the same `score_before` the
 * hall of fame uses - and shown on a bar.
 *
 * 0x1053 is a **jump** target, not a call, so a map that follows calls counts
 * its bytes as part of this routine and a coverage figure reads 100% with
 * none of it written. What it does matters: hsc_scratch, that
 * `hsc_sort` reads is built here, and with one player it is built *only*
 * here - the multi-player path jumps to 0x1066 instead, past the copy,
 * because it has already filled hsc_scratch itself. The port had the one-player
 * branch calling hsc_sort with no copy at all, so it sorted whatever was left
 * in the scratch and the player's own score never entered the table.
 *
 * The tail from 0x1066 is shared by both, and is three more things: the
 * keyboard handler comes out if it is the one installed, the sort and the
 * save are **skipped when the demo is the active input** - the attract mode
 * must not write its score into popcorn.hsc - and the entity list is emptied
 * before the longjmp back to the menu.
 */
/* 1ac2:1066 - where both paths meet. */
static void results_finish(const char *dir)
{
    if (gv.input_active == INPUT_KEYBOARD)
        restore_int09();            /* 1ac2:106e */
    /* 1ac2:1071 - the demo does not enter the hall of fame, and above all
     * does not write popcorn.hsc. */
    if (gv.input_active != INPUT_DEMO) {
        hsc_sort();                 /* 1ac2:1079 */
        hsc_save(dir);              /* 1ac2:107c */
    }
    screen_high_scores();           /* 1ac2:107f */
    entities_clear();               /* 1ac2:1082 */
}

void screen_results(const char *dir)
{
    if (gv.player_count == 1) {
        /* 1ac2:1053 - the only record's name, then its six score digits. */
        memcpy(gv.scratch2.hsc_scratch[0].name, gv.players[0].name,
               sizeof gv.players[0].name);
        memcpy(gv.scratch2.hsc_scratch[0].score, gv.players[0].score,
               sizeof gv.players[0].score);
        results_finish(dir);
        return;
    }

    memset(gv.level.cells, 0, sizeof gv.level.cells);
    level_intro();

    /* An insertion sort of the player records into hsc_scratch. */
    hsc_entry_t *scratch = gv.scratch2.hsc_scratch;
    const player_t *q = &gv.players[0];
    memcpy(scratch[0].name, q->name, sizeof q->name);
    memcpy(scratch[0].score, q->score, sizeof q->score);
    for (uint32_t k = 1; k < gv.player_count; k++) {
        q = &gv.players[k];
        uint32_t at = k;
        while (at > 0 && score_before(q->score, scratch[at - 1].score))
            at--;
        memmove(&scratch[at + 1], &scratch[at], (k - at) * sizeof *scratch);
        memcpy(scratch[at].name, q->name, sizeof q->name);
        memcpy(scratch[at].score, q->score, sizeof q->score);
    }

    /* 1ac2:0f02 - the CLASSEMENT panel. The port had a sketch of this: the
     * top bar, then both players written one scan line apart with no heading,
     * no rule and no panel behind them, which put two names on one line over
     * a black field. What follows is the original's own sequence.
     *
     * The panel is drawn with `draw_run(' ', n)`: glyph 0 is not blank, it is
     * a solid block of colour 2, so a run of spaces *is* the red ground the
     * text sits on. HSC_LINE is the step from one text line to the next. */
    for (int32_t i = 0; i < 0x18; i++)              /* 1ac2:0f08 */
        img_vram_setw(0xf2 + i * 2, 0xaaaa);

    uint32_t d = 0x20f2;
    d = draw_run(' ', 0x18, d);                     /* 1ac2:0f1a */

    d = (d + HSC_LINE) & 0xffff;                    /* the heading */
    d = draw_run(' ', 7, d);
    for (const char *p2 = "CLASSEMENT"; *p2; p2++, d = (d + 2) & 0xffff)
        draw_char(*p2, d);
    d = draw_run(' ', 7, d);                        /* 1ac2:0f6e */

    d = (d + HSC_LINE) & 0xffff;                    /* 1ac2:0f79, the rule */
    d = draw_run(' ', 5, d);
    d = draw_run('-', 0x0e, d);
    d = draw_run(' ', 5, d);

    d = (d + HSC_LINE) & 0xffff;                    /* 1ac2:0f92, a blank */
    d = draw_run(' ', 0x18, d);

    d = (d + HSC_LINE) & 0xffff;                    /* 1ac2:0f99 */

    for (uint32_t k = 0; k < gv.player_count; k++) {  /* 1ac2:0fa4 */
        const hsc_entry_t *rec = &gv.scratch2.hsc_scratch[k];
        d = draw_run(' ', 2, d);
        d = draw_text(rec->name, 0x0c, d);
        d = draw_run(' ', 2, d);
        d = draw_text((const char *)rec->score, 6, d);
        d = draw_run(' ', 2, d);
        /* 1ac2:0fca - two scan lines of bar between the rows. */
        d = (d + HSC_LINE) & 0xffff;
        for (int32_t r = 0; r < 2; r++) {
            for (int32_t i = 0; i < 0x18; i++)
                img_vram_setw((d + i * 2) & 0xffff, 0xaaaa);
            d = cga_next_row(d);
        }
    }

    /* 1ac2:1006 - the rest of the panel, down to the bottom bar at 0x1f42. */
    while ((d & 0xffff) != 0x1f42) {
        for (int32_t i = 0; i < 0x18; i++)
            img_vram_setw((d + i * 2) & 0xffff, 0xaaaa);
        d = cga_next_row(d);
    }

    /* 1ac2:102a - the keyboard handler comes out before the wait, so the
     * BIOS buffer fills again and INT 16h below has something to read. */
    if (gv.input_active == INPUT_KEYBOARD)
        restore_int09();

    /* 1ac2:1035 - the results stand until a key or until the two nested
     * counts of 0xc8 run out. */
    for (int32_t dl = 0xc8; dl > 0; dl--) {
        /* 1ac2:1037, the outer pass. Nothing else in this screen is a frame:
         * the whole of it - the cleared field, the intro over it, the bar and
         * the names - is drawn before the wait begins, so one comparison here
         * covers all of it and the rest cover anything that moves. Without
         * it the results screen cannot be compared at all: io_frame_sync
         * lives in the play loop, and by here the play loop is over. */
        io_frame_sync_extra(SYNC_RESULTS);
        for (int32_t n = 0xc8; n > 0; n--) {
            if (io_key_ready()) {
                io_get_key();
                results_finish(dir);
                return;
            }
            game_delay();
        }
        io_present();
        if (!io_pump())
            return;
    }
    results_finish(dir);
}

/* 1ac2:1a6f  demo_input_step
 *
 * Inside the play loop rather than a routine of its own. The level's animation
 * script lives in the block reached as segment 0x14a1 and [0x3136] walks it;
 * [0x3134] counts down to the next step and reloads from [0x3135]. A 0xffff
 * in the script is not the end but a jump: the word after it is where to
 * carry on, so a script can loop without being copied.
 */
void demo_input_step(void)
{
    if (--gv.anim_count != 0)
        return;
    gv.anim_count = gv.anim_rate;
    gv.anim_ptr = (uint16_t)(gv.anim_ptr + 2);
    uint32_t si = gv.anim_ptr;
    if (img_w(SEG_14A1 + si) == 0xffff)
        gv.anim_ptr = (uint16_t)(img_w(SEG_14A1 + si + 2));
}

/* 1ac2:3c35  bonus_script
 *
 * Movement kind 4 does not wander: it follows a list of steps at [bx+0x0a],
 * one word per frame. The low byte is a signed horizontal delta - `shl al,1`
 * tests its sign and `rcr al,1 / neg al` recovers the magnitude - and the
 * capsule refuses a leftward step that would take it past its own position.
 * The high byte becomes the y, as `0x79 + ah`.
 *
 * The x is clamped to 8..0xb8, which are the same walls everything else uses.
 */
int32_t bonus_script(ent_anim_t *b, uint32_t *px, uint32_t *py)
{
    uint32_t si = b->script;
    b->script = (uint16_t)(si + 2);
    uint32_t word = img_w(si);
    uint32_t al = word & 0xff, ah = (word >> 8) & 0xff;
    uint32_t cl = b->arg.move.steps;

    if (al & 0x80) {                    /* a leftward step */
        uint32_t mag = (uint32_t)(-(int32_t)(int8_t)al) & 0xff;
        if (cl < mag)
            cl = 8;                     /* it would go through the wall */
        else
            cl = (cl + al) & 0xff;
    } else {
        cl = (cl + al) & 0xff;
    }
    if (cl > 0xb8)
        cl = 0xb8;
    if (cl < 8)
        cl = 8;

    *px = cl;
    *py = (0x79 + ah) & 0xff;

    /* The script word's high byte comes back in AH, and entity_bonus tests
     * `cmp ah, 0xff` to decide whether to commit the move at all - 0xff means
     * "stay where you are". Always answering "moved" put the capsule at
     * 0x79 + 0xff = 0x78, which is what diverged 21,332 frames into level 2:
     * the port walked it to y 0x78 while the original left it alone. */
    return ah != 0xff;
}

/* 1ac2:3200  bonus 10, the M capsule - the monsters stop coming
 *
 * It arms the timer at [0x2e7a] for ten thousand frames and lights a bar down
 * the panel at vram 0x1a8b, which `play_frame` drains one cell at a time -
 * [0x2e87] walks it - so the player can see how long is left. While it runs,
 * `play_frame` takes the branch at 0x1c19 and does not open a hatch, so
 * nothing new comes out of the top. That is the whole effect - the game does
 * not slow down, which is what this comment used to claim.
 */
void bonus_stop_monsters(void)
{
    gv.extra_on = 1;
    gv.serve_timeout = (uint16_t)(0x2710);
    gv.extra_timer = (uint16_t)(0x190);
    fill_column(0x1a8b, 0xaaaa);
    gv.extra_pos = (uint16_t)(0x1a8b);
}

/* ========================================================================
 * 1ac2:1785  input_demo
 *
 * The third input routine, and the one nothing static can reach: `demo_start`
 * stores it in [0x2d45] exactly the way F1 stores 0x1654 or 0x16d2, and the
 * play loop calls whatever is there. It was missing from the map for that
 * reason, and so the demo had no way to move its paddle.
 *
 * It is the whole of the computer's play. `cs:[0x1784]` is which ball it is
 * chasing, 0xff for none: with none it scans the three balls for one past
 * y 0x82 that is in play and coming down, and with one it steps the paddle a
 * single pixel towards that ball's x - the same one pixel a frame the human
 * keyboard path gets, which is why the demo is beatable.
 *
 * Any key ends it. The key goes to the cheat matcher first, and only a key
 * that is not part of the sequence throws the stack away and returns to the
 * menu.
 * ===================================================================== */
#define DEMO_CHASE_Y  0x82

/* Both tails clamp the paddle to the right-hand limit before returning. */
static void demo_clamp(void)
{
    if (gv.paddle_x >= gv.paddle_max)
        gv.paddle_x = gv.paddle_max;
}

void input_demo(void)
{
    if (io_key_ready()) {
        uint32_t key = io_get_key();
        if ((key >> 8) == 0x44) {               /* F10 */
            employee_enter();                   /* 1ac2:4ae0 */
            while ((io_get_key() >> 8) == 0x44)
                ;
            screen_restore();                   /* 1ac2:4b4f */
        } else if ((key >> 8) == 0x43) {        /* F9: sound */
            cv.sound_on ^= 1;
        } else if (!cheat_sequence((uint8_t)(key & 0xff))) {
            entities_clear();                   /* 1ac2:055e */
            longjmp(g_back_to_menu, 1);         /* sp = [0x1405]; jmp 0x1d1 */
        }
    }

    uint32_t chasing = cv.demo_ball;
    if (chasing != 0xff) {
        ball_t *b = &gv.balls[chasing];
        if (b->y >= DEMO_CHASE_Y &&
            b->dir_y != 1 &&
            b->state != 0) {
            /* Hold the action key down while the laser is armed, so the
             * demo fires as well as chases. */
            gv.key_action = gv.laser_on != 0;

            uint32_t ball_x = b->x;
            uint32_t paddle = gv.paddle_x;
            if (ball_x < paddle) {
                if (paddle != gv.paddle_min)
                    gv.paddle_x--;
                return;
            }
            if (ball_x < ((paddle + gv.paddle_width) & 0xff))
                return;                         /* already under it */
            if (paddle != gv.paddle_max)
                gv.paddle_x++;
            return;
        }
    }

    /* Nothing to chase, or the one we had is gone: pick another. */
    for (uint32_t cl = 0; cl < 3; cl++) {
        ball_t *b = &gv.balls[cl];
        if (b->y > DEMO_CHASE_Y &&
            b->state != 0 &&
            b->dir_y != 1) {
            cv.demo_ball = (uint8_t)cl;
            demo_clamp();
            return;
        }
    }
    cv.demo_ball = 0xff;
    demo_clamp();
}

/* 1ac2:58b3  cheat_sequence
 *
 * A key at a time against a sequence stored XORed with 0xaa at cs:[0x56a5],
 * with cs:[0x56a2] the cursor into it and cs:[0x56a4] the last byte matched.
 * Returns true while the key is part of the sequence - the caller reads that
 * as "consumed, carry on" - and false for anything else, which is what ends
 * the demo. Repeating the last matched key is tolerated so a held key does
 * not break the run.
 *
 * Completing it shows a hidden message: mode 3, the text decoded a byte at a
 * time with a rolling key, 0x5c meaning a new line, then a key and back to
 * mode 5. The port has no text renderer - the same gap as screen_define_keys
 * - so the message is decoded and put on stderr rather than on the screen,
 * and everything else about the sequence behaves as it does in the original.
 */
#define CHEAT_START  0x56a5
#define CHEAT_TEXT   0x56b5

int32_t cheat_sequence(uint8_t key)
{
    uint32_t si = cv.cheat_cursor;
    uint32_t al = key ^ 0xaa;

    if (al != g_image[CS_BASE + si]) {
        /* Not the next one. The same key twice is not a failure. */
        if (al == cv.cheat_last)
            return 1;
        cv.cheat_cursor = CHEAT_START;
        cv.cheat_last = 0;
        return 0;
    }

    cv.cheat_last = (uint8_t)al;
    cv.cheat_cursor = (uint16_t)(si + 1);
    if (g_image[CS_BASE + si + 1] != 0xaa)
        return 1;                       /* more to go */

    /* The whole sequence. Mode 3, the message, a key, mode 5 again. */
    io_cga_mode(3);
    {
        char line[256];
        uint32_t n = 0, ah = 0x20, s = CHEAT_TEXT;
        for (;;) {
            uint32_t c = (g_image[CS_BASE + s++] ^ ah) ^ 0xaa;
            ah = c;
            if (c == 0)
                break;
            if (c == 0x5c) {            /* a new line */
                line[n] = 0;
                fprintf(stderr, "popcorn: [message] %s\n", line);
                n = 0;
                continue;
            }
            if (n < sizeof line - 1)
                line[n++] = (char)c;
        }
        if (n) {
            line[n] = 0;
            fprintf(stderr, "popcorn: [message] %s\n", line);
        }
    }
    io_get_key();
    io_cga_mode(5);
    cv.cheat_cursor = CHEAT_START;
    cv.cheat_last = 0xff;
    return 1;
}

/* ========================================================================
 * The animated bricks - cells 16 to 21.
 *
 * The brick table at 0x3044 is twenty-two entries long, and its last six all
 * point here. These cells are not ordinary bricks: they are the pieces of one
 * larger picture that keeps animating, and the cell's low nibble says which
 * piece. Hitting one does not clear it - it **adds 8** to the cell, leaving
 * the nibble alone, and leaves an entity behind that goes on drawing the
 * piece from the level's animation script.
 * ===================================================================== */
#define ANIM_SPRITE_BYTES  32           /* 8 rows of 4, `dx <<= 5` */

/* 1ac2:3bac  draw_anim_cell - eight rows of four bytes, copied not XORed,
 * out of the block reached as segment 0x14a1. */
void draw_anim_cell(uint32_t si, uint32_t x, uint32_t y)
{
    uint32_t di = cga_at(x, y);
    for (int32_t r = 0; r < 8; r++) {
        for (int32_t b = 0; b < 4; b++)
            g_vram[(di + b) & (CGA_SIZE - 1)] =
                g_image[SEG_14A1 + si + r * 4 + b];
        di = cga_next_row(di);
    }
}

/* 1ac2:3abf  entity_anim_brick
 *
 * One piece of the animation, redrawn whenever the script steps - which is
 * when [0x3134] has come back round to [0x3135]. The frame is the script's
 * current entry, offset by the piece's number.
 */
void entity_anim_brick(ent_brick_t *br)
{
    if (gv.anim_rate != gv.anim_count)
        return;
    uint32_t si = img_w(SEG_14A1 + gv.anim_ptr)
                + br->piece * ANIM_SPRITE_BYTES;
    draw_anim_cell(si, br->x, br->y);
}

/* 1ac2:2ccd  brick_animated - cells 16 to 21 */
void brick_animated(hit_t *hit, ball_t *ball)
{
    brick_score(0, 0, 0x0303);
    cv.sound_request = 3;
    if (ball)
        ball->bounces++;

    uint32_t cell = hit->cell;
    uint32_t was = g_image[cell];
    g_image[cell] = (uint8_t)(was + 8);  /* marked, not cleared */
    uint32_t piece = was & 0x0f;

    /* Remember what this piece turned into, indexed by the new cell value. */
    uint32_t script = s14a1.level[gv.level_number].script;
    uint32_t frame = (img_w(SEG_14A1 + script)
                      + (piece << 5)) & 0xffff;
    gv.cell_bitmap[(was + 8) & 0xff] = (uint16_t)frame;

    /* Draw it once where the brick was, from the script's current entry. */
    uint32_t x = hit->x, y = hit->y;
    draw_anim_cell((img_w(SEG_14A1 + gv.anim_ptr)
                    + (piece << 5)) & 0xffff, x, y);

    entity_t *e = entity_alloc();
    e->handler = 0x3abf;
    ent_brick_t *br = &e->p.brick;
    br->x = hit->x;          /* the centre, one word in the original */
    br->y = hit->y;
    br->piece = (uint8_t)piece;
    gv.level.bricks--;
}
