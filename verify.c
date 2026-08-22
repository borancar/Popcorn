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
 *     "PVS2"  u32 routine  u16 regs[10]  u32 ticks
 *             u32 image_len  image  u32 vram_len  vram
 *
 * with regs in the order ax bx cx dx si di bp es ds flags.
 *
 * `ticks` is the BIOS counter at 0040:006c that the game's PRNG stirs in. It
 * has to come from the emulator, not from the host clock, or every routine
 * that consults random() diverges for a reason that is not a bug - which is
 * exactly what happened to the brick handlers: the C decided to crumble a
 * brick where the original decided to remove it, and the only difference was
 * the seed.
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
    case 0x044b: level_colours(); return 1;
    case 0x10c5: draw_run((unsigned char)(r[R_AX] & 0xff), r[R_DX] & 0xff,
                          r[R_DI]); return 1;
    case 0x10d1: draw_text(r[R_SI], r[R_DX] & 0xff, r[R_DI]); return 1;
    case 0x14a7: draw_cursor(r[R_DI]); return 1;
    case 0x3146: flash_bar(r[R_DX]); return 1;
    case 0x3232: entity_alloc(); return 1;
    case 0x3257: entity_unlink(r[R_BX]); return 1;
    case 0x3668: cell_set_three(r[R_BX]); return 1;
    case 0x36fb: cells_restore(); return 1;
    case 0x30dd: pixel_xor(r[R_BX] & 0xff, r[R_AX] & 0xff); return 1;
    case 0x306b: shot_xor(r[R_BX] & 0xff, r[R_AX] & 0xff); return 1;
    case 0x3f20: bonus_hits_ball(r[R_BX], r[R_SI]); return 1;
    case 0x3f4f: sprite_shift_draw(r[R_CX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
                 return 1;
    case 0x406a: xor_sprite_20x16(r[R_CX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
                 return 1;
    case 0x40f2: xor_sprite_16xn(r[R_BX] & 0xff, r[R_AX] & 0xff, r[R_SI],
                                 r[R_CX] & 0xff); return 1;
    case 0x40c0: game_random(0, r[R_DX] & 0xff); return 1;
    case 0x3aee: entity_sparkle(r[R_BX]); return 1;
    case 0x3b2a: entity_crumble(r[R_BX]); return 1;
    case 0x390d: entity_hatch(r[R_BX]); return 1;
    case 0x39a1: bonus_release(r[R_BX]); return 1;
    case 0x39fa: entity_bonus(r[R_BX]); return 1;
    case 0x3df1: bonus_update(r[R_BX], r[R_CX] & 0xff, r[R_AX] & 0xff);
                 return 1;
    case 0x365e: entity_soften(r[R_BX]); return 1;
    case 0x366f: entity_repeat(r[R_BX]); return 1;
    case 0x3696: entity_plain(r[R_BX]); return 1;
    case 0x36a1: entity_ball_arrive(r[R_BX]); return 1;
    case 0x36f6: entity_cells_timer(r[R_BX]); return 1;
    case 0x37e0: entity_ball_hold(r[R_BX]); return 1;
    case 0x318b: extra_life(); return 1;
    case 0x2109: scroll_up_band(); return 1;
    case 0x2148: scroll_down_band(); return 1;
    case 0x22a9: draw_paddle_raw(r[R_SI]); return 1;
    case 0x2187: draw_paddle_shifted(r[R_SI]); return 1;
    case 0x2e1e: ball_on_paddle(r[R_SI]); return 1;
    case 0x2ee3: laser_fire(); return 1;
    case 0x2755: probe_cell_at(r[R_AX] & 0xff, r[R_BX] & 0xff, r[R_SI]);
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
    case 0x1fc1:                        /* field_backdrop(al = y) */
        field_backdrop(r[R_AX] & 0xff);
        return 1;
    case 0x1e50:                        /* walker_draw(cl = x) */
        walker_draw(r[R_CX] & 0xff);
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
    unsigned routine, image_len, vram_len, ticks;
    unsigned short regs[R_COUNT];
    unsigned char rb[R_COUNT * 2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PVS2", 4) ||
        !rd32(f, &routine) || fread(rb, 1, sizeof rb, f) != sizeof rb ||
        !rd32(f, &ticks) || !rd32(f, &image_len)) {
        fprintf(stderr, "%s: not a state file\n", in_path);
        fclose(f);
        return 1;
    }
    for (int i = 0; i < R_COUNT; i++)
        regs[i] = (unsigned short)(rb[i * 2] | rb[i * 2 + 1] << 8);
    io_set_ticks(ticks);

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
