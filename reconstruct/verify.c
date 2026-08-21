/*
 * Run one transcribed routine against a state captured from the emulator.
 *
 * A routine that compiles proves nothing, and one that looks right on screen
 * proves very little more: a blitter can be wrong in ways that still draw
 * something plausible. So each is checked the only way that settles it -
 * against the code it replaces, on the same inputs.
 *
 * ../verify.py captures the machine at the entry to a routine, lets the
 * original run to its return, and captures again. This side loads the "before"
 * state, calls the C routine with the arguments the registers held, and writes
 * what it produced. The Python side diffs that against the "after".
 *
 * The state file is deliberately dumb:
 *
 *     "PVS1"  u32 routine  u16 regs[10]  u32 image_len  image  u32 vram_len  vram
 *
 * with regs in the order ax bx cx dx si di bp es ds flags.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

enum { R_AX, R_BX, R_CX, R_DX, R_SI, R_DI, R_BP, R_ES, R_DS, R_FL, R_COUNT };

/* Every routine transcribed so far, by the image offset it was read from.
 * The argument mapping is part of what is being asserted: getting it wrong
 * shows up as a mismatch, which is the point. */
static int dispatch(unsigned routine, const unsigned short *r)
{
    switch (routine) {
    case 0x27d7:                        /* ball_step(si = the ball) */
        ball_step(r[R_SI]);
        return 1;
    case 0x22de:                        /* paddle_row_offsets(bl, di) */
        paddle_row_offsets(r[R_BX] & 0xff, r[R_DI]);
        return 1;
    case 0x2281:                        /* blit_xor(si = pixels, di = rows) */
        blit_xor(r[R_SI], r[R_DI]);
        return 1;
    case 0x221a:                        /* draw_paddle(si = sprite) */
        draw_paddle(r[R_SI]);
        return 1;
    case 0x2881:                        /* ball_draw(si = sprite, bl, al) */
        ball_draw(r[R_SI], r[R_BX] & 0xff, r[R_AX] & 0xff);
        return 1;
    case 0x2827:                        /* ball_redraw(si = the ball) */
        ball_redraw(r[R_SI]);
        return 1;
    case 0x2034:                        /* draw_brick_row(al = screen row) */
        draw_brick_row(r[R_AX] & 0xff);
        return 1;
    case 0x20b9:                        /* draw_sprite_20x6(bl, al, si) */
        draw_sprite_20x6(r[R_BX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
        return 1;
    case 0x247f:                        /* ball_after(si = the ball) */
        ball_after(r[R_SI]);
        return 1;
    case 0x2316:                        /* ball_paddle(si = the ball) */
        ball_paddle(r[R_SI]);
        return 1;
    case 0x254d:                        /* ball_bricks(si = the ball) */
        ball_bricks(r[R_SI]);
        return 1;
    case 0x413d:                        /* score_add, no arguments */
        score_add();
        return 1;
    case 0x3b64:                        /* xor_sprite_16x7(cl, al, si) */
        xor_sprite_16x7(r[R_CX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
        return 1;
    case 0x0c64:                        /* draw_char(al, di) */
        draw_char((unsigned char)(r[R_AX] & 0xff), r[R_DI]);
        return 1;
    case 0x1712:                        /* input_keyboard, no arguments */
        input_keyboard();
        return 1;
    case 0x5099:
        save_screen();
        return 1;
    case 0x50bc:
        restore_screen();
        return 1;
    default:
        return 0;
    }
}

static int rd32(FILE *f, unsigned *out)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *out = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    return 1;
}

int verify_main(const char *in_path, const char *out_path)
{
    FILE *f = fopen(in_path, "rb");
    if (!f) {
        perror(in_path);
        return 1;
    }
    char magic[4];
    unsigned routine, image_len, vram_len;
    unsigned short regs[R_COUNT];
    unsigned char rb[R_COUNT * 2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PVS1", 4) ||
        !rd32(f, &routine) || fread(rb, 1, sizeof rb, f) != sizeof rb ||
        !rd32(f, &image_len)) {
        fprintf(stderr, "%s: not a state file\n", in_path);
        fclose(f);
        return 1;
    }
    for (int i = 0; i < R_COUNT; i++)
        regs[i] = (unsigned short)(rb[i * 2] | rb[i * 2 + 1] << 8);

    g_image = malloc(image_len);
    if (!g_image || fread(g_image, 1, image_len, f) != image_len) {
        fprintf(stderr, "%s: truncated image\n", in_path);
        fclose(f);
        return 1;
    }
    if (!rd32(f, &vram_len) || vram_len != CGA_SIZE ||
        fread(g_vram, 1, CGA_SIZE, f) != CGA_SIZE) {
        fprintf(stderr, "%s: truncated vram\n", in_path);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (!dispatch(routine, regs)) {
        fprintf(stderr, "no C routine for %#x\n", routine);
        return 2;                       /* distinct from a mismatch */
    }

    FILE *o = fopen(out_path, "wb");
    if (!o) {
        perror(out_path);
        return 1;
    }
    fwrite(g_image, 1, image_len, o);
    fwrite(g_vram, 1, CGA_SIZE, o);
    fclose(o);
    return 0;
}
