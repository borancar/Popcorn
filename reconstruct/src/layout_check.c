/*
 * The layout, checked.
 *
 * Every offset the port relies on, held at compile time: a wrong padding size
 * stops the build here instead of moving a field. They were written next to
 * the structs in game.h and live here so the header can describe a second
 * layout - without MATCH_MEMORY_LAYOUT the structs are unpacked and the
 * padding is gone, and none of these offsets hold. This file is the matching
 * layout's half of game.h, and compiles only there.
 */
#include <stddef.h>

#include "game.h"

#ifndef MATCH_MEMORY_LAYOUT
#error "layout_check.c checks the matching layout: build with -DMATCH_MEMORY_LAYOUT"
#endif

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

ENSURE_SIZE(fall_frame_t, 4);

ENSURE_SIZE(particle_t, 16);

ENSURE_SIZE(level_t, 0xb0);
#define ENSURE_LEVEL_AT(field, off) \
    typedef char ensure_level_at_##field[offsetof(level_t, field) == (off) ? 1 : -1]
ENSURE_LEVEL_AT(cells, 0x08);

ENSURE_SIZE(player_t, 0x11b);
#define ENSURE_PLAYER_AT(field, off) \
    typedef char ensure_player_at_##field[offsetof(player_t, field) == (off) ? 1 : -1]
ENSURE_PLAYER_AT(name, 0x00);   ENSURE_PLAYER_AT(lives, 0x0c);
ENSURE_PLAYER_AT(level_src_ptr, 0x0d); ENSURE_PLAYER_AT(level_number, 0x0f);
ENSURE_PLAYER_AT(score, 0x10);  ENSURE_PLAYER_AT(level, 0x16);
ENSURE_PLAYER_AT(state, 0xc6);  ENSURE_PLAYER_AT(ent_count, 0xd2);
ENSURE_PLAYER_AT(ents, 0xd3);

ENSURE_SIZE(cell_bitmap_t, 60);

/* Every arm of the variant must fill the payload exactly - a short one would
 * silently move `next`. */
#define ENSURE_ENTITY_ARM(arm, n) \
    typedef char ensure_entity_arm_##arm[sizeof(ent_##arm##_t) == (n) ? 1 : -1]

ENSURE_ENTITY_ARM(anim, 10);   ENSURE_ENTITY_ARM(fall, 10);
ENSURE_ENTITY_ARM(hatch, 10);  ENSURE_ENTITY_ARM(cells, 10);
ENSURE_ENTITY_ARM(brick, 10);  ENSURE_ENTITY_ARM(morph, 10);

ENSURE_SIZE(entity_t, 0x0e);
#define ENSURE_ENTITY_AT(field, off) \
    typedef char ensure_entity_at_##field[offsetof(entity_t, field) == (off) ? 1 : -1]
ENSURE_ENTITY_AT(handler_fn, 0x00);
ENSURE_ENTITY_AT(p, 0x02);
ENSURE_ENTITY_AT(next_ptr, 0x0c);

ENSURE_SIZE(paddle_rows_t, 14);

ENSURE_SIZE(sweep_t, 4);

ENSURE_SIZE(mark_t, 4);

ENSURE_SIZE(eog_group_t, 4);

ENSURE_SIZE(bonus_kind_t, 4);

ENSURE_SIZE(paddle_morph_t, 409);

ENSURE_SIZE(crumble_t, 246);

ENSURE_SIZE(paddle_set_t, 4);

ENSURE_SIZE(hsc_entry_t, 0x12);

ENSURE_SIZE(hit_dir_t, 2);

ENSURE_SIZE(bonus_step_t, 2);

ENSURE_SIZE(note_t, 2);

ENSURE_SIZE(point_t, 2);

ENSURE_SIZE(hit_t, 4);

ENSURE_SIZE(global_t, 0xc460);      /* SEG_ASSETS, which is defined below */

/* offsetof checked at compile time. _Static_assert is C11 and this is C99, so
 * it is the negative-array-size trick; the failure message names the field. */
#define ENSURE_GLOBAL_AT(field, off) \
    typedef char ensure_global_at_##field[offsetof(global_t, field) == (off) ? 1 : -1]

/* The same for a field inside a nested struct. `a.b` cannot be pasted into an
 * identifier, so the name is given separately from the path. */
#define ENSURE_GLOBAL_AT_IN(name, path, off) \
    typedef char ensure_global_at_##name[offsetof(global_t, path) == (off) ? 1 : -1]

/* @generated-asserts begin - genvars.py rewrites between these markers */
ENSURE_GLOBAL_AT(scratch1, 0x0000);
ENSURE_GLOBAL_AT(cmd_tail, 0x13a0);
ENSURE_GLOBAL_AT(eog_screen_at, 0x13c0);
ENSURE_GLOBAL_AT(eog_build_ptr, 0x13c2);
ENSURE_GLOBAL_AT(banner_state, 0x13c4);
ENSURE_GLOBAL_AT(cga_mode, 0x13c7);
ENSURE_GLOBAL_AT(cga_colour, 0x13c8);
ENSURE_GLOBAL_AT(banner_ptr, 0x13c5);
ENSURE_GLOBAL_AT(lives, 0x13c9);
ENSURE_GLOBAL_AT(level_src_ptr, 0x13ca);
ENSURE_GLOBAL_AT(level_number, 0x13cc);
ENSURE_GLOBAL_AT(score_text, 0x13cd);
ENSURE_GLOBAL_AT(extra_at, 0x13d3);
ENSURE_GLOBAL_AT(player_name, 0x13d5);
ENSURE_GLOBAL_AT(name_prompt, 0x13e1);
ENSURE_GLOBAL_AT(player_digit, 0x13e9);
ENSURE_GLOBAL_AT(demo_name, 0x13f9);
ENSURE_GLOBAL_AT(menu_sp, 0x1405);
ENSURE_GLOBAL_AT(level_text, 0x1407);
ENSURE_GLOBAL_AT(level_num_text, 0x1410);
ENSURE_GLOBAL_AT(particle_count, 0x1413);
ENSURE_GLOBAL_AT(particles, 0x148d);
ENSURE_GLOBAL_AT(particle_seed, 0x1acd);
ENSURE_GLOBAL_AT(particle_sprites, 0x1acf);
ENSURE_GLOBAL_AT(score_add, 0x1415);
ENSURE_GLOBAL_AT(walker_work, 0x146a);
ENSURE_GLOBAL_AT(default_drive, 0x141b);
ENSURE_GLOBAL_AT(hsc_file, 0x141c);
ENSURE_GLOBAL_AT(level_file, 0x1428);
ENSURE_GLOBAL_AT(walker_anim_ptr, 0x1468);
ENSURE_GLOBAL_AT(speed_step, 0x1485);
ENSURE_GLOBAL_AT(speed_limit, 0x1486);
ENSURE_GLOBAL_AT(frame_delay, 0x1487);
ENSURE_GLOBAL_AT(frame_delay_set, 0x1489);
ENSURE_GLOBAL_AT(speed_timer, 0x148b);
ENSURE_GLOBAL_AT(msg_no_level_file, 0x2a8f);
ENSURE_GLOBAL_AT(msg_not_level_file, 0x2ac3);
ENSURE_GLOBAL_AT(prompt_define_keys, 0x2b03);
ENSURE_GLOBAL_AT(prompt_press_key, 0x2b1a);
ENSURE_GLOBAL_AT(results_rows, 0x2b39);
ENSURE_GLOBAL_AT(paddle_sets, 0x2d0d);
ENSURE_GLOBAL_AT(paddle_grow_ptr, 0x2d1d);
ENSURE_GLOBAL_AT(paddle_shrink_ptr, 0x2d25);
ENSURE_GLOBAL_AT(paddle_next, 0x2d2d);
ENSURE_GLOBAL_AT(paddle_step, 0x2d38);
ENSURE_GLOBAL_AT(paddle_kind, 0x2d39);
ENSURE_GLOBAL_AT(paddle_width, 0x2d3a);
ENSURE_GLOBAL_AT(paddle_morphing, 0x2d3b);
ENSURE_GLOBAL_AT(scratch2, 0x1aef);
ENSURE_GLOBAL_AT(morph_owner_ptr, 0x2d3c);
ENSURE_GLOBAL_AT(paddle_min, 0x2d3e);
ENSURE_GLOBAL_AT(paddle_max, 0x2d3f);
ENSURE_GLOBAL_AT(repeat_count, 0x2d40);
ENSURE_GLOBAL_AT(int09_saved_off, 0x2d41);
ENSURE_GLOBAL_AT(int09_saved_seg, 0x2d43);
ENSURE_GLOBAL_AT(input_active_fn, 0x2d45);
ENSURE_GLOBAL_AT(input_selected_fn, 0x2d47);
ENSURE_GLOBAL_AT(last_make, 0x2d49);
ENSURE_GLOBAL_AT(last_dir, 0x2d4a);
ENSURE_GLOBAL_AT(repeat_div, 0x2d4b);
ENSURE_GLOBAL_AT(key_action, 0x2d4c);
ENSURE_GLOBAL_AT(key_right, 0x2d4d);
ENSURE_GLOBAL_AT(key_left, 0x2d4e);
ENSURE_GLOBAL_AT(key_scan_l, 0x2d4f);
ENSURE_GLOBAL_AT(key_scan_r, 0x2d50);
ENSURE_GLOBAL_AT(key_scan_a, 0x2d51);
ENSURE_GLOBAL_AT(key_reserved, 0x2d52);
ENSURE_GLOBAL_AT(key_prompts, 0x2d5c);
ENSURE_GLOBAL_AT(paddle_pix, 0x2d8c);
ENSURE_GLOBAL_AT(slope_top, 0x2e2c);
ENSURE_GLOBAL_AT(slope_side, 0x2e42);
ENSURE_GLOBAL_AT(paddle_rows, 0x2e57);
ENSURE_GLOBAL_AT(paddle_x, 0x2e54);
ENSURE_GLOBAL_AT(paddle_prev_x, 0x2e55);
ENSURE_GLOBAL_AT(hold_offset, 0x2e56);
ENSURE_GLOBAL_AT(ball_alive, 0x2e73);
ENSURE_GLOBAL_AT(hit_count, 0x2e74);
ENSURE_GLOBAL_AT(hits, 0x2e89);
ENSURE_GLOBAL_AT(brick_handler_fn, 0x3044);
ENSURE_GLOBAL_AT(cell_bitmap, 0x3080);
ENSURE_GLOBAL_AT_IN(cell_bitmap_animated, cell_bitmap.animated_ptr, 0x30b0);
ENSURE_GLOBAL_AT(cell_score, 0x30bc);
ENSURE_GLOBAL_AT(caught, 0x2e75);
ENSURE_GLOBAL_AT(hold_timer, 0x2e76);
ENSURE_GLOBAL_AT(paddle_lost, 0x2e78);
ENSURE_GLOBAL_AT(extra_on, 0x2e79);
ENSURE_GLOBAL_AT(serve_timeout, 0x2e7a);
ENSURE_GLOBAL_AT(extra_timer, 0x2e7c);
ENSURE_GLOBAL_AT(laser_on, 0x2e7e);
ENSURE_GLOBAL_AT(laser_y, 0x2e7f);
ENSURE_GLOBAL_AT(laser_x, 0x2e80);
ENSURE_GLOBAL_AT(net_on, 0x2e81);
ENSURE_GLOBAL_AT(net_life, 0x2e82);
ENSURE_GLOBAL_AT(net_timer, 0x2e84);
ENSURE_GLOBAL_AT(net_pos, 0x2e85);
ENSURE_GLOBAL_AT(extra_pos, 0x2e87);
ENSURE_GLOBAL_AT(hit_dirs, 0x2e99);
ENSURE_GLOBAL_AT(balls, 0x2ea1);
ENSURE_GLOBAL_AT(backdrop_phase, 0x2efb);
ENSURE_GLOBAL_AT(sweep, 0x2efc);
ENSURE_GLOBAL_AT(sweep_y, 0x2f0c);
ENSURE_GLOBAL_AT(level, 0x2f10);
ENSURE_GLOBAL_AT(cell_guard, 0x2fc0);
ENSURE_GLOBAL_AT(anim_count, 0x3134);
ENSURE_GLOBAL_AT(anim_rate, 0x3135);
ENSURE_GLOBAL_AT(anim_ptr, 0x3136);
ENSURE_GLOBAL_AT(entity_head, 0x3138);
ENSURE_GLOBAL_AT(entity_free_ptr, 0x3138);
ENSURE_GLOBAL_AT(entity_remove, 0x313a);
ENSURE_GLOBAL_AT(entity_prev_ptr, 0x3142);
ENSURE_GLOBAL_AT(entities, 0x3146);
ENSURE_GLOBAL_AT(bonus_cap, 0x3384);
ENSURE_GLOBAL_AT(capsule_frames_ptr, 0x3385);
ENSURE_GLOBAL_AT(popup_frames_ptr, 0x339b);
ENSURE_GLOBAL_AT(bonus_odds, 0x33b1);
ENSURE_GLOBAL_AT(bonus_handler_fn, 0x33bc);
ENSURE_GLOBAL_AT(rng_state, 0x33d2);
ENSURE_GLOBAL_AT(hit_kind, 0x33d4);
ENSURE_GLOBAL_AT(bonus_pending, 0x33d5);
ENSURE_GLOBAL_AT(bonus_live, 0x33d6);
ENSURE_GLOBAL_AT(field_marks, 0x33d7);
ENSURE_GLOBAL_AT(sprite_work, 0x33f7);
ENSURE_GLOBAL_AT(bonus_move_fn, 0x3447);
ENSURE_GLOBAL_AT(players, 0x344f);
ENSURE_GLOBAL_AT(hsc, 0x3e42);
ENSURE_GLOBAL_AT(player_count, 0x3f08);
ENSURE_GLOBAL_AT(live_count, 0x3f09);
ENSURE_GLOBAL_AT(cur_player, 0x3f0a);
ENSURE_GLOBAL_AT(cheat_text, 0x3f0b);
ENSURE_GLOBAL_AT(cheat_done, 0x3f1b);
ENSURE_GLOBAL_AT(cheat_at_ptr, 0x3f1c);
ENSURE_GLOBAL_AT(banner_text, 0x3f1e);
ENSURE_GLOBAL_AT(arrow_head_sprite, 0x4890);
ENSURE_GLOBAL_AT(frame_corner_right, 0x48bd);
ENSURE_GLOBAL_AT(frame_corner_left, 0x48d2);
ENSURE_GLOBAL_AT(life_sprite, 0x48e7);
ENSURE_GLOBAL_AT(ball_start_sprite, 0x48fb);
ENSURE_GLOBAL_AT(paddle_sprites, 0x4903);
ENSURE_GLOBAL_AT(capsule_table0, 0x4dd3);
ENSURE_GLOBAL_AT(capsule_appear, 0x4e13);
ENSURE_GLOBAL_AT(capsule_letter0, 0x4e47);
ENSURE_GLOBAL_AT(capsule_vanish, 0x4eeb);
ENSURE_GLOBAL_AT(capsule_bank, 0x4f3b);
ENSURE_GLOBAL_AT(popup_table0, 0x5823);
ENSURE_GLOBAL_AT(popup_appear, 0x5863);
ENSURE_GLOBAL_AT(popup_vanish, 0x5897);
ENSURE_GLOBAL_AT(popup_vanish_ruled, 0x58cb);
ENSURE_GLOBAL_AT(popup_vanish_last, 0x58e7);
ENSURE_GLOBAL_AT(popup_table, 0x5903);
ENSURE_GLOBAL_AT(paddle_morph, 0x5b83);
ENSURE_GLOBAL_AT(hatch_script_ptr, 0x604e);
ENSURE_GLOBAL_AT(mark_sprite, 0x6078);
ENSURE_GLOBAL_AT(hatch_frame, 0x60c2);
ENSURE_GLOBAL_AT(brick_bitmap, 0x63a6);
ENSURE_GLOBAL_AT(crumble, 0x6506);
ENSURE_GLOBAL_AT(brick8_roll_ptr, 0x67e8);
ENSURE_GLOBAL_AT(brick8_score, 0x681c);
ENSURE_GLOBAL_AT(brick8_roll, 0x6838);
ENSURE_GLOBAL_AT(teleport_out_ptr, 0x6abc);
ENSURE_GLOBAL_AT(teleport_in_ptr, 0x6ace);
ENSURE_GLOBAL_AT(teleport_frame, 0x6ae0);
ENSURE_GLOBAL_AT(brick10_hold_ptr, 0x6b88);
ENSURE_GLOBAL_AT(brick10_hold, 0x6b9c);
ENSURE_GLOBAL_AT(sweep_sprite, 0x6d2c);
ENSURE_GLOBAL_AT(intro_feed, 0x6d36);
ENSURE_GLOBAL_AT(backdrop_ptr, 0x6d95);
ENSURE_GLOBAL_AT(backdrop, 0x6d9f);
ENSURE_GLOBAL_AT(walker_frame_ptr, 0x751f);
ENSURE_GLOBAL_AT(walker_frame, 0x7533);
ENSURE_GLOBAL_AT(walker_drop_ptr, 0x75db);
ENSURE_GLOBAL_AT(walker_drop, 0x75e7);
ENSURE_GLOBAL_AT(hatch_open_ptr, 0x770d);
ENSURE_GLOBAL_AT(hatch_shut_ptr, 0x7717);
ENSURE_GLOBAL_AT(walk_hatch_frame, 0x7721);
ENSURE_GLOBAL_AT(curtain_image, 0x7805);
ENSURE_GLOBAL_AT(bonus_path, 0x8320);
ENSURE_GLOBAL_AT(panel, 0x85f0);
ENSURE_GLOBAL_AT(font, 0x9020);
ENSURE_GLOBAL_AT(pause_overlay, 0x93e0);
ENSURE_GLOBAL_AT(paddle_dissolve_frames_ptr, 0x9bb0);
ENSURE_GLOBAL_AT(paddle_dissolve_frame, 0x9bd6);
ENSURE_GLOBAL_AT(banner_font, 0xa3c0);
ENSURE_GLOBAL_AT(eog_overlay, 0xa6d0);
ENSURE_GLOBAL_AT(eog_groups, 0xa8bf);
ENSURE_GLOBAL_AT(eog_group_sprite, 0xa8db);
ENSURE_GLOBAL_AT(eog_blank, 0xabab);
ENSURE_GLOBAL_AT(bonus_kinds, 0xac60);
ENSURE_GLOBAL_AT(bonus_anim0, 0xac80);
ENSURE_GLOBAL_AT(bonus_anim1, 0xae78);
ENSURE_GLOBAL_AT(bonus_anim2, 0xb12e);
ENSURE_GLOBAL_AT(bonus_anim3, 0xb27a);
ENSURE_GLOBAL_AT(bonus_anim4, 0xb50e);
ENSURE_GLOBAL_AT(sparkle_ptr, 0xb7a2);
ENSURE_GLOBAL_AT(sparkle, 0xb7bc);
ENSURE_GLOBAL_AT(bonus_anim5, 0xbb7c);
ENSURE_GLOBAL_AT(bonus_anim6, 0xbe10);
ENSURE_GLOBAL_AT(bonus_anim7, 0xc152);
/* @generated-asserts end */

/* The two facts the chain rests on, checked rather than described: the head
 * node sits at 0x3138, and its `next` therefore lands on 0x3144 - the word
 * every walk starts from. Change the declarations inside the union, or the
 * seven bytes of payload between entity_remove and entity_prev_ptr, and the second
 * one stops holding; after that entity_alloc appends to the wrong place and
 * entity_unlink corrupts the list, both silently. This way the build stops. */
typedef char ensure_head_next_lands_on_3144[
    offsetof(global_t, entity_head.next_ptr) == 0x3144 ? 1 : -1];

ENSURE_SIZE(assets_t, 0x85ae);

#define ENSURE_ASSETS_AT(field, off) \
    typedef char ensure_assets_at_##field[offsetof(assets_t, field) == (off) ? 1 : -1]
ENSURE_ASSETS_AT(ppc_signature, 0x0006);
ENSURE_ASSETS_AT(levels, 0x000c);
ENSURE_ASSETS_AT(banner_xlat, 0x226c);
ENSURE_ASSETS_AT(boss_screen, 0x22a8);
ENSURE_ASSETS_AT(blob_target, 0x2823);
ENSURE_ASSETS_AT(ending_mark, 0x28d9);
ENSURE_ASSETS_AT(hole_picture, 0x28f0);
ENSURE_ASSETS_AT(scroll_rows, 0x488a);
ENSURE_ASSETS_AT(reveal, 0x4d84);
ENSURE_ASSETS_AT(bravo_cells, 0x2825);
ENSURE_ASSETS_AT(blob_script, 0x289d);
ENSURE_ASSETS_AT(logo, 0x6b60);
ENSURE_ASSETS_AT(screen_save, 0x3df0);
ENSURE_ASSETS_AT(ending_band, 0x7c70);

ENSURE_SIZE(level_anim_t, 4);

ENSURE_SIZE(anim_sprite_t, 32);

ENSURE_SIZE(anim_group_t, 192);

ENSURE_SIZE(animations_t, 0x600a);

#define ENSURE_ANIMATIONS_AT(field, off) \
    typedef char ensure_animations_at_##field[offsetof(animations_t, field) == (off) ? 1 : -1]
ENSURE_ANIMATIONS_AT(anim0, 0x00d0);
ENSURE_ANIMATIONS_AT(anim1, 0x1790);
ENSURE_ANIMATIONS_AT(anim2, 0x2640);
ENSURE_ANIMATIONS_AT(anim3, 0x3b80);
ENSURE_ANIMATIONS_AT(anim4, 0x4e80);
ENSURE_ANIMATIONS_AT(anim5, 0x5860);

ENSURE_SIZE(runtime_t, 0x5c6e);

#define ENSURE_CODE_AT(field, off) \
    typedef char ensure_code_at_##field[offsetof(runtime_t, field) == (off) ? 1 : -1]
ENSURE_CODE_AT(tunes, 0x0000);
ENSURE_CODE_AT(sound_on, 0x0084);
ENSURE_CODE_AT(sound_request, 0x00f4);
ENSURE_CODE_AT(sound_timer, 0x00f5);
ENSURE_CODE_AT(sound_ptr, 0x00f6);
ENSURE_CODE_AT(sound_tunes_ptr, 0x00f8);
ENSURE_CODE_AT(delay_entry, 0x164c);
ENSURE_CODE_AT(delay_count, 0x164e);
ENSURE_CODE_AT(demo_ball, 0x1784);
ENSURE_CODE_AT(crtc_text80, 0x4b91);
ENSURE_CODE_AT(crtc_graphics, 0x4b9d);
ENSURE_CODE_AT(border_spr, 0x506d);
ENSURE_CODE_AT(border_pos, 0x507d);
ENSURE_CODE_AT(cheat_cursor_ptr, 0x56a2);
ENSURE_CODE_AT(cheat_last, 0x56a4);
ENSURE_CODE_AT(cheat_keys, 0x56a5);
ENSURE_CODE_AT(cheat_text, 0x56b5);
ENSURE_CODE_AT(frame_phase, 0x5c6d);

ENSURE_SIZE(image_t, IMAGE_LEN);
#define ENSURE_IMAGE_AT(field, off) \
    typedef char ensure_image_at_##field[offsetof(image_t, field) == (off) ? 1 : -1]
ENSURE_IMAGE_AT(seg_global, 0x00000);
ENSURE_IMAGE_AT(seg_assets, SEG_ASSETS);
ENSURE_IMAGE_AT(seg_animations, SEG_ANIMATIONS);
ENSURE_IMAGE_AT(seg_runtime, SEG_RUNTIME);
