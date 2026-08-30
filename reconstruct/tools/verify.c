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
 * Some routines say nothing to memory and everything to their caller -
 * 1ac2:40c0, the PRNG, only bumps a counter by a constant whatever seed it was
 * given. Comparing memory alone passes those whatever they compute, which is
 * not a check at all, so a routine may also report a **result** and have that
 * compared too.
 *
 * The state file is deliberately dumb:
 *
 *     "PVS2"  u32 routine  u16 regs[10]  u32 ticks
 *             u32 image_len  image  u32 vram_len  vram
 *             [u16 mouse_x  u16 buttons  u16 pending_key]
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
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

enum { R_AX, R_BX, R_CX, R_DX, R_SI, R_DI, R_BP, R_ES, R_DS, R_FL, R_COUNT };

/* A routine's answer to its caller, where it has one. -1 means "this routine
 * says nothing the caller reads that is not already in memory". */
static int64_t g_result = -1;

/* Every routine transcribed so far, by the image offset it was read from.
 * The argument mapping is part of what is being asserted: getting it wrong
 * shows up as a mismatch, which is the point. */
/* 1ac2:1a4f and 1ac2:1a6f are deliberately absent. Neither is a routine
 * entry - 0x1a4f is a call site, `call word ptr [0x2d45]`, and 0x1a6f is an
 * inline block inside play_loop - so the harness's model does not apply to
 * them: it reads [SP] as a return address when what is there is play_loop's
 * own frame, lets the "original" run to somewhere arbitrary, and reports the
 * difference as a failure of the C. Both were in the outstanding list for
 * most of a session on the strength of that. The C routines are real and are
 * exercised through play_loop; only verifying them *as routines* is wrong. */
/* The ball a brick handler was struck by, or none. See the note on 0x28cb. */
static ball_t *ball_or_none(uint32_t off)
{
    return off ? ball_at(off) : NULL;
}

static int32_t dispatch(uint32_t routine, const uint16_t *r)
{
    switch (routine) {
    case 0x27d7:                        /* ball_step(si = the ball) */
        ball_step(ball_at(r[R_SI]));
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
        ball_draw(g_image + r[R_SI], r[R_BX] & 0xff, r[R_AX] & 0xff);
        return 1;
    /* These answer in the **carry flag**, not in memory: play_loop reads
     * `jae` after each of them. A routine can leave the image byte-identical
     * and still say the opposite thing, and until this was reported the
     * harness called that agreement. */
    case 0x2827:                        /* ball_redraw(si = the ball) */
        g_result = ball_redraw(ball_at(r[R_SI]));
        return 1;
    case 0x044b: level_colours(); return 1;
    case 0x10c5: draw_run((uint8_t)(r[R_AX] & 0xff), r[R_DX] & 0xff,
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
    case 0x3f20: bonus_hits_ball(&entity_at(r[R_BX])->p.anim.sprite, ball_at(r[R_SI])); return 1;
    case 0x3f4f: sprite_shift_draw(r[R_CX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
                 return 1;
    case 0x406a: xor_sprite_20x16(r[R_CX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
                 return 1;
    case 0x40f2: xor_sprite_16xn(r[R_BX] & 0xff, r[R_AX] & 0xff, r[R_SI],
                                 r[R_CX] & 0xff); return 1;
    case 0x40c0:
        if (getenv("POPCORN_DEBUG_RNG"))
            fprintf(stderr, "rng: ticks=%u stir0=%04x state=%04x dl=%02x\n",
                    io_ticks(), (uint32_t)(g_image[0x3164] |
                    g_image[0x3165] << 8), (uint32_t)(g_image[0x33d2] |
                    g_image[0x33d3] << 8), r[R_DX] & 0xff);
        g_result = game_random(io_ticks(), r[R_DX] & 0xff);
        return 1;
    case 0x51b6: screen_end_of_game(); return 1;
    /* Never dispatched before, so never checked once. Each is a routine the
     * port has and the harness had no way to ask about; the argument
     * registers are read off the routine's own first few instructions. */
    case 0x0a1d: field_marks_wide(r[R_DI], (r[R_CX] >> 8) & 0xff); return 1;
    case 0x3200: bonus_stop_monsters(); return 1;
    case 0x41e5: cell_special(r[R_AX] & 0xff, r[R_CX] & 0xff, r[R_DI]);
                 return 1;
    case 0x4b7a: set_palette_registers(r[R_SI]); return 1;
    case 0x4c4b: brick_11_after(r[R_CX] & 0xff, r[R_AX] & 0xff); return 1;
    case 0x4cc1: cell_hole_draw(r[R_CX] & 0xff, r[R_AX] & 0xff); return 1;
    /* 0x4e1a, the high-score screen, is left out: it returns on a key, and
     * the C reads the port's keyboard while the original reads the
     * emulator's. The two would return after different numbers of border
     * steps and the difference would be the harness, not the port. */
    /* 1ac2:4ae0 (the boss key) and 1ac2:1581 with its two helpers (redefine
     * keys) are **not dispatched**, because they are not transcribed: both are
     * deliberate no-ops in the port. Checking a no-op against the original
     * would report a decision as a difference. */
    case 0x3abf: entity_anim_brick(&entity_at(r[R_BX])->p.brick); return 1;
    case 0x3bac: draw_anim_cell(r[R_SI], r[R_CX] & 0xff, r[R_AX] & 0xff);
                 return 1;
    /* The brick handlers, all of them. Only two were dispatched, so the
     * heart of the game - what happens when the ball meets a cell - could not
     * be checked at all. SI is the hit record and BP the ball, or zero when
     * something other than a ball did the hitting. */
    /* BP is the ball, and the original passes **zero** for "no ball": an
     * animated brick can be struck by the moving picture rather than by a
     * ball, and cell_special goes the same way. ball_at(0) is a perfectly
     * good pointer to image offset 0, so every `if (ball)` in the brick
     * handlers passes and they bounce and score with a ball made of the
     * image's first thirty bytes - writing bounces at 0x1d, which is what
     * three of the level 8 comparisons were reporting. */
    case 0x28cb: brick_1(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2985: brick_2(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2a3f: brick_3(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x3221: brick_solid(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2a73: brick_5(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2ab4: brick_6(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2af5: brick_7(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2b36: brick_8(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2b9d: brick_9(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2c59: brick_10(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x2ccd: brick_animated(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x3aee: entity_sparkle(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x3b2a: entity_crumble(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x390d: entity_hatch(&entity_at(r[R_BX])->p.hatch); return 1;
    case 0x39a1: bonus_release(&entity_at(r[R_BX])->p.hatch); return 1;
    case 0x39fa: entity_bonus(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x3df1: bonus_update(&entity_at(r[R_BX])->p.anim.sprite, r[R_CX] & 0xff, r[R_AX] & 0xff);
                 return 1;
    case 0x365e: entity_soften(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x366f: entity_repeat(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x3696: entity_plain(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x36a1: entity_ball_arrive(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x36f6: entity_cells_timer(&entity_at(r[R_BX])->p.cells); return 1;
    case 0x37e0: entity_ball_hold(&entity_at(r[R_BX])->p.anim); return 1;
    case 0x318b: extra_life(); return 1;
    case 0x2109: scroll_up_band(); return 1;
    case 0x2148: scroll_down_band(); return 1;
    case 0x22a9: draw_paddle_raw(r[R_SI]); return 1;
    case 0x2187: draw_paddle_shifted(r[R_SI]); return 1;
    case 0x2e1e: g_result = ball_on_paddle(ball_at(r[R_SI])); return 1;
    case 0x2ee3: laser_fire(); return 1;
    case 0x2755: probe_cell_at(r[R_AX] & 0xff, r[R_BX] & 0xff, hit_at(r[R_SI]));
                 return 1;
    case 0x3273: entity_capsule(&entity_at(r[R_BX])->p.fall); return 1;
    case 0x3561: entity_popup(&entity_at(r[R_BX])->p.fall); return 1;
    case 0x2daa: bonus_points(); return 1;
    case 0x2def: bonus_catch(); return 1;
    case 0x2e03: bonus_laser(); return 1;
    case 0x2e16: bonus_multiball(); return 1;
    case 0x3231: bonus_wider_paddle(); return 1;
    case 0x3119: bonus_net(); return 1;
    case 0x315b: bonus_reverse(); return 1;
    case 0x31e8: bonus_slower_ball(); return 1;
    case 0x41b1: fill_column(r[R_DI], r[R_AX]); return 1;
    case 0x3717: entity_multiball(); return 1;
    case 0x3386: entity_paddle_fx(&entity_at(r[R_BX])->p.morph); return 1;
    case 0x05f8: level_between(); return 1;
    case 0x492f: arrow_head(r[R_DI]); return 1;
    case 0x4957: arrow_tail(r[R_DI]); return 1;
    case 0x490d: menu_arrow(); return 1;
    case 0x50df: menu_banner_tick(); return 1;
    case 0x5140: banner_shift(); return 1;
    case 0x53c2: menu_particles_tick(); return 1;
    case 0x548a: g_result = particle_init(r[R_SI], r[R_AX]); return 1;
    case 0x5476: menu_particles_init(r[R_AX]); return 1;
    case 0x5448: g_result = particle_random(r[R_AX], io_ticks(), r[R_DX]);
                 return 1;
    case 0x0598: field_marks(); return 1;
    case 0x0911: panel_reveal(); return 1;
    case 0x1354: frame_band(r[R_DI], r[R_AX]); return 1;
    case 0x2d68: brick_11(hit_at(r[R_SI]), ball_or_none(r[R_BP])); return 1;
    case 0x3d95: bonus_spawn(); return 1;
    case 0x0cc5: play_prepare(); return 1;
    case 0x1509: demo_start(); return 1;
    case 0x2034:                        /* draw_brick_row(al = screen row) */
        draw_brick_row(r[R_AX] & 0xff);
        return 1;
    case 0x20b9:                        /* draw_sprite_20x6(bl, al, si) */
        draw_sprite_20x6(r[R_BX] & 0xff, r[R_AX] & 0xff, r[R_SI]);
        return 1;
    case 0x247f:                        /* ball_after(si = the ball) */
        ball_after(ball_at(r[R_SI]));
        return 1;
    case 0x2316:                        /* ball_paddle(si = the ball) */
        ball_paddle(ball_at(r[R_SI]));
        return 1;
    case 0x254d:                        /* ball_bricks(si = the ball) */
        ball_bricks(ball_at(r[R_SI]));
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
        draw_char((uint8_t)(r[R_AX] & 0xff), r[R_DI]);
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
    case 0x0090:
        speaker_on();
        return 1;
    case 0x0097:
        sound_tick();
        return 1;
    case 0x0106:
        flush_keys();
        return 1;
    case 0x055e:
        entities_clear();
        return 1;
    case 0x0735:
        life_lost();
        return 1;
    case 0x09c5:
        panel_finish();
        return 1;
    case 0x0b0b:
        panel_draw();
        return 1;
    case 0x1212:
        play_frame();
        return 1;
    case 0x14b3:
        build_shifted_sprites();
        return 1;
    case 0x164c:
        game_delay();
        return 1;
    case 0x27b7:
        drop_duplicate_hits();
        return 1;
    case 0x41d4:
        play_teardown();
        return 1;
    /* 1ac2:2da0 and 1ac2:4210 are **not dispatched**, and that is not an
     * oversight. The bonus does not return to its caller: 0x2da0 throws four
     * words off the stack and the ending's own `ret` lands in play_session.
     * verify.py stops the original at the return address it read off the
     * stack at entry, which this routine never uses - so the original runs on
     * through the level change and beyond, and is compared against a C
     * function that stopped at the end of the screen. That reports a
     * difference of hundreds of bytes for a reason that has nothing to do
     * with the transcription, and reported one all through 2026-08-23.
     *
     * The screen is checked by sidebyside.py instead, which does not care
     * where a routine returns to: four hundred thousand frames identical,
     * through this bonus and out the other side into the next level.
     *
     * entity_paddle_fx has the same limit on the one call in sixty that fires
     * the bonus, and is dispatched anyway - the other fifty-nine are worth
     * having. */
    case 0x4878:
        screen_scroll_up();
        return 1;
    case 0x48af:
        input_and_draw_paddle();
        return 1;
    case 0x48ce:
        level_tally();
        return 1;
    case 0x4b4f:
        screen_restore();
        return 1;
    case 0x4ba9:
        screen_stash();
        return 1;
    case 0x4c13:
        screen_unstash();
        return 1;
    case 0x4d37:
        hsc_sort();
        return 1;
    case 0x4f58:
        border_animate();
        return 1;
    case 0x4f73:
        border_setup();
        return 1;
    case 0x5196:
        palette_cycle();
        return 1;
    case 0x5317:
        ending_column();
        return 1;
    case 0x5a43:
        ending_particles_init(r[R_AX]);
        return 1;
    case 0x5a56:
        ending_particles_tick();
        return 1;
    case 0x5b80:
        g_result = ending_blobs();
        return 1;
    case 0x03d1:
        install_int09();
        return 1;
    case 0x0473:
        screen_game_over();
        return 1;
    case 0x0521:
        screen_level_done();
        return 1;
    case 0x078b:
        intro_curtain();
        return 1;
    case 0x1eb9:
        level_intro();
        return 1;
    case 0x49bc:
        intro_paddle();
        return 1;
    case 0x4a7a:
        intro_scroll();
        return 1;
    case 0x54d6:
        intro_logo();
        return 1;
    case 0x55e5:
        intro_reveal();
        return 1;
    case 0x108c:                        /* score_before(si, di) */
        g_result = score_before(g_image + r[R_SI], r[R_DI]);
        return 1;
    case 0x34c5:                        /* morph_begin(bx, si, dx) */
        morph_begin(&entity_at(r[R_BX])->p.morph, r[R_SI], r[R_DX]);
        return 1;
    case 0x34d7:                        /* morph_step(bx) */
        morph_step(&entity_at(r[R_BX])->p.morph);
        return 1;
    case 0x3bf7: {                      /* bonus_steer(bx, cl, al) */
        /* entity_bonus hands these in CL and AL - the capsule's x
         * and y. Passing zero instead made the routine work on a
         * capsule at the origin, take a different branch, and draw
         * twice from the PRNG where the original drew nothing. The
         * failure that reported was the harness's, not the port's. */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_steer(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    case 0x3c35: {                      /* bonus_script(bx, cl, al) */
        /* entity_bonus hands these in CL and AL - the capsule's x
         * and y. Passing zero instead made the routine work on a
         * capsule at the origin, take a different branch, and draw
         * twice from the PRNG where the original drew nothing. The
         * failure that reported was the harness's, not the port's. */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_script(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    /* The four directions a falling capsule can step, 1ac2:3447's table.
     * x is in CL and y in AL, the same as bonus_steer above; what comes back
     * is the carry - blocked, or took the step.
     *
     * The stepped x and y themselves are **not compared**: they go back in
     * CL and AL and the harness reads one output value, not the register
     * file. What is compared is the decision, which is the part with the cell
     * arithmetic in it; the step itself is one `inc` behind that decision. */
    case 0x3c66: {                      /* bonus_move_right(bx, cl, al) */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_move_right(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    case 0x3caf: {                      /* bonus_move_up */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_move_up(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    case 0x3cf3: {                      /* bonus_move_left */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_move_left(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    case 0x3d3c: {                      /* bonus_move_down */
        uint32_t x = r[R_CX] & 0xff, y = r[R_AX] & 0xff;
        g_result = bonus_move_down(&entity_at(r[R_BX])->p.anim, &x, &y);
        return 1;
    }
    case 0x45a1:                        /* ball_after_endgame(si) */
        g_result = ball_after_endgame(ball_at(r[R_SI]));
        return 1;
    case 0x4d5d:                        /* hsc_bubble(si, di) */
        g_result = hsc_bubble(r[R_SI], r[R_DI]);
        return 1;
    case 0x4fa7:                        /* border_step(di) */
        g_result = border_step(r[R_DI]);
        return 1;
    case 0x4fd3:                        /* border_erase(di) */
        border_erase(r[R_DI]);
        return 1;
    case 0x4ff1:                        /* border_draw(di) */
        border_draw(r[R_DI]);
        return 1;
    case 0x5019: border_block(r[R_DI]); return 1;
    case 0x5045:                        /* border_row(di) */
        border_row(r[R_DI]);
        return 1;
    case 0x5171:                        /* cheat_match(al) */
        cheat_match(r[R_AX] & 0xff);
        return 1;
    case 0x538d: {                      /* tall_sprite(si, di) */
        uint32_t si = r[R_SI];
        g_result = tall_sprite(&si, r[R_DI]);
        return 1;
    }
    case 0x59f7:                        /* ending_particle_init(si, ax) */
        g_result = ending_particle_init(r[R_SI], r[R_AX]);
        return 1;
    case 0x5add:                        /* ending_plot(cx, dx) */
        ending_plot(r[R_CX], r[R_DX]);
        return 1;
    case 0x5bb5:                        /* ending_walk(bl, bh, dx) */
        g_result = ending_walk(r[R_BX] & 0xff, (r[R_BX] >> 8) & 0xff,
                               r[R_DX]);
        return 1;
    case 0x5c36:                        /* ending_blob(ax) */
        ending_blob(r[R_AX]);
        return 1;
    case 0x1c4f:                        /* level_draw */
        level_draw();
        return 1;
    case 0x1e23:                        /* walker_step(cl = x) */
        walker_step(r[R_CX] & 0xff);
        return 1;
    case 0x1785:                        /* input_demo */
        input_demo();
        return 1;
    case 0x58b3:                        /* cheat_sequence(al) */
        g_result = cheat_sequence((uint8_t)(r[R_AX] & 0xff));
        return 1;
    default:
        return 0;
    }
}

static int32_t rd32(FILE *f, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *out = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    return 1;
}

int32_t verify_main(const char *in_path, const char *out_path)
{
    FILE *f = fopen(in_path, "rb");
    if (!f) {
        perror(in_path);
        return 1;
    }
    char magic[4];
    uint32_t routine, image_len, vram_len, ticks;
    uint16_t regs[R_COUNT];
    uint8_t rb[R_COUNT * 2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PVS2", 4) ||
        !rd32(f, &routine) || fread(rb, 1, sizeof rb, f) != sizeof rb ||
        !rd32(f, &ticks) || !rd32(f, &image_len)) {
        fprintf(stderr, "%s: not a state file\n", in_path);
        fclose(f);
        return 1;
    }
    for (int32_t i = 0; i < R_COUNT; i++)
        regs[i] = (uint16_t)(rb[i * 2] | rb[i * 2 + 1] << 8);
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
    /* Optional trailing pair: the pointer the emulator had. Older state files
     * stop after the vram and simply do not pin it. */
    {
        uint8_t mb[6];
        if (fread(mb, 1, sizeof mb, f) == sizeof mb) {
            io_pin_mouse((uint32_t)(mb[0] | mb[1] << 8),
                         (uint32_t)(mb[2] | mb[3] << 8));
            io_pin_key((uint32_t)(mb[4] | mb[5] << 8));
        }
    }
    fclose(f);

    /* bonus_effect's level-ending case abandons the play loop by longjmp, the
     * way 1ac2:2da0 abandons it by throwing four words off the stack. In the
     * game that lands in play_session; here there is no play_session, and an
     * unarmed longjmp is undefined behaviour - it crashed the checker rather
     * than reporting anything. Arming it makes the routine's effect complete
     * at the jump, which is what it is. */
    if (setjmp(g_bonus_done) != 0) {
        /* fall through to writing the result out */
    } else if (!dispatch(routine, regs)) {
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
    uint8_t res[5];
    res[0] = g_result >= 0;
    res[1] = (uint8_t)(g_result & 0xff);
    res[2] = (uint8_t)((g_result >> 8) & 0xff);
    res[3] = res[4] = 0;
    fwrite(res, 1, sizeof res, o);
    fclose(o);
    return 0;
}
