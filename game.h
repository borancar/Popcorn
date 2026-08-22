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

extern unsigned char *g_image;          /* the whole unpacked load image */

/* Read the game's data out of the player's own POPCORN.EXE. */
unsigned char *exepack_load(const char *path, size_t *out_len);

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

extern unsigned char g_vram[CGA_SIZE];

/* Step a CGA offset on by one scan line, the way the game does. */
static inline unsigned cga_next_row(unsigned di)
{
    return di < CGA_PLANE ? di + CGA_PLANE : di - (CGA_PLANE - CGA_STRIDE);
}

/* And back up one, which the intro animations use to draw upwards. Note the
 * original's test is `cmp di,0x2000 / ja`, strictly greater, so an offset of
 * exactly 0x2000 takes the other branch - kept, because it is what runs. */
static inline unsigned cga_prev_row(unsigned di)
{
    return di > CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
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
#define KEY_ACTION  0x2d4c
#define KEY_RIGHT   0x2d4d
#define KEY_LEFT    0x2d4e
#define KEY_SCAN_L  0x2d4f             /* the configured scan codes */
#define KEY_SCAN_R  0x2d50
#define KEY_SCAN_A  0x2d51

/* --------------------------------------------------------------- state ---
 *
 * Offsets of the game variables identified so far.  These are not C
 * declarations: they are indices into g_image, so that a transcribed routine
 * reads the same address the disassembly shows.
 */
#define PADDLE_X      0x2e54           /* left edge, pixels */
#define PADDLE_MIN    0x2d3e           /* 8 */
#define PADDLE_MAX    0x2d3f           /* 172 */
#define PADDLE_W          28
#define PADDLE_ROW       186

/* The ball pool: four entries of 0x1e bytes at 0x2ea1, walked by the play loop
 * at 1ac2:1873 and stepped by the Bresenham routine at 1ac2:27d7. */
#define BALLS         0x2ea1
#define BALL_STRIDE   0x1e
#define BALL_COUNT       4
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
#define ENTITY_HEAD   0x3144
#define ENTITY_POOL   0x3146
#define ENTITY_STRIDE 0x0e
#define E_HANDLER     0x00
#define E_NEXT        0x0c
#define ENTITY_REMOVE 0x313a            /* a handler sets this to be unlinked */
#define ENTITY_PREV   0x3142            /* the node before the current one */
#define ENTITY_FREE   0x3138            /* head of the free list */

/* --------------------------------------------------------- the backend ---
 *
 * What the game needs from the platform.  sdl_io.c is the one implementation;
 * the split exists so that the game code below it never mentions SDL.
 */
int  io_init(int scale);
void io_shutdown(void);
void io_present(void);                 /* g_vram to the screen */
int  io_pump(void);                    /* poll input; 0 means quit */
void io_wait_retrace(void);            /* what `in al,0x3da` / test 8 did */
void io_sound(unsigned divisor);       /* PIT channel 2; 0 silences */
void io_delay_cycles(unsigned cycles); /* what the busy-wait at 0x164c cost */
int  io_key_ready(void);               /* INT 16h AH=01 */
unsigned io_get_key(void);             /* INT 16h AH=00: scan<<8 | ascii */
void io_flush_keys(void);
void io_script_key(unsigned scan, unsigned ms);
int  io_save_shot(const char *path);
void io_set_deadline(unsigned ms, const char *shot, const char *vram);

/* Little-endian accessors, so a transcribed `mov ax,[0x3144]` reads the way it
 * reads in the disassembly. */
static inline unsigned img_w(unsigned off)
{
    return (unsigned)g_image[off] | ((unsigned)g_image[off + 1] << 8);
}
static inline void img_setw(unsigned off, unsigned v)
{
    g_image[off] = (unsigned char)v;
    g_image[off + 1] = (unsigned char)(v >> 8);
}

/* ------------------------------------------------------- the game code ---
 *
 * Each carries the image offset it was transcribed from; verify.c maps those
 * offsets to these calls, and the mapping of registers to arguments is part of
 * what the check asserts.
 */
void ball_step(unsigned ball);                          /* 1ac2:27d7 */
void input_keyboard(void);                              /* 1ac2:1712 */
void input_mouse(unsigned mouse_x, unsigned buttons);   /* 1ac2:169f */
void save_screen(void);                                 /* 1ac2:5099 */
void restore_screen(void);                              /* 1ac2:50bc */
void paddle_row_offsets(unsigned x, unsigned rows_out); /* 1ac2:22de */
void blit_xor(unsigned pixels, unsigned rows);          /* 1ac2:2281 */
void draw_paddle(unsigned sprite);                      /* 1ac2:221a */
void draw_char(unsigned char c, unsigned di);           /* 1ac2:0c64 */
unsigned game_random(unsigned ticks, unsigned limit);   /* 1ac2:40c0 */
void speaker_on(void);                                  /* 1ac2:0085 */
void speaker_off(void);                                 /* 1ac2:0090 */
void sound_tick(void);                                  /* 1ac2:0097 */
void game_delay(void);                                  /* 1ac2:164c */
void read_speed_setting(unsigned speed);                /* 1ac2:5680 */
void build_shifted_sprites(void);                       /* 1ac2:14b3 */
void load_high_scores(const char *dir);                 /* 1ac2:4d96 */
void intro_curtain(void);                               /* 1ac2:078b */
void intro_logo(void);                                  /* 1ac2:54d6 */
void intro_reveal(void);                                /* 1ac2:55e5 */
void intro_scroll(void);                                /* 1ac2:4a7a */
void game_main(const char *dir, unsigned speed);

/* --------------------------------------------------------- not yet done ---
 * Implemented as no-ops in stubs.c; see the note at the top of that file.
 */
void menu_particles_init(unsigned ax_in);   /* 1ac2:5476 */
void plot_pixel(unsigned x, unsigned y, unsigned colour);
void plot_pixel_xor(unsigned x, unsigned y, unsigned colour);
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void banner_shift(void);          /* 1ac2:5140 */
void brick_11_after(unsigned x, unsigned y);  /* 1ac2:4c4b */
unsigned particle_random(unsigned ax, unsigned ticks, unsigned limit); /* 1ac2:5448 */
unsigned particle_init(unsigned si, unsigned ax_in);  /* 1ac2:548a */
#define PARTICLES 0x148d
void menu_arrow(void);            /* 1ac2:490d */
void arrow_head(unsigned di);     /* 1ac2:492f */
void arrow_tail(unsigned di);     /* 1ac2:4957 */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void palette_cycle(void);         /* 1ac2:5196 */
void flush_keys(void);            /* 1ac2:0106 */
void install_int09(void);         /* 1ac2:03b0 */
void restore_int09(void);         /* 1ac2:03d1 */
void input_and_draw_paddle(void); /* 1ac2:48af */
void cheat_match(unsigned char c);/* 1ac2:5171 */
void io_cga_mode(unsigned v);
void io_cga_colour(unsigned v);
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void employee_leave(void);        /* 1ac2:4b4f */
void demo_start(void);            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */

void level_load_file(void);       /* 1ac2:08c8 */
unsigned char screen_player_names(void);  /* 1ac2:10de */
int  name_field(unsigned di, unsigned char *abort); /* 1ac2:13b8 */
void play_frame(void);            /* 1ac2:1212 */
unsigned frame_band(unsigned di, unsigned fill);  /* 1ac2:1354 */
void panel_reveal(void);          /* 1ac2:0911 */
void field_marks(void);           /* 1ac2:0598 */
void field_marks_wide(unsigned di);  /* 1ac2:0a1d */

/* A word into the framebuffer, wrapping like the 16-bit offset it is. */
static inline void img_vram_setw(unsigned di, unsigned v)
{
    g_vram[di & (CGA_SIZE - 1)] = (unsigned char)v;
    g_vram[(di + 1) & (CGA_SIZE - 1)] = (unsigned char)(v >> 8);
}
void panel_finish(void);          /* 1ac2:09c5 */

/* Called by play_loop; the ones still empty live in stubs.c too. */
int  play_loop(void);             /* 1ac2:1873 - transcribed */
void draw_text(unsigned src, unsigned count, unsigned di);  /* 1ac2:10d1 */
void level_draw(void);            /* 1ac2:1c4f */
void walker_draw(unsigned x);     /* 1ac2:1e50 */
void walker_step(unsigned x);     /* 1ac2:1e23 */
void ball_draw(unsigned sprite, unsigned x, unsigned y);    /* 1ac2:2881 */
int  ball_redraw(unsigned ball);  /* 1ac2:2827 */
int  ball_on_paddle(unsigned ball); /* 1ac2:2e1e */
void read_new_key(unsigned which);  /* 1ac2:1614 */
int  score_before(unsigned si, unsigned di); /* 1ac2:108c */
void ball_after(unsigned ball);   /* 1ac2:247f */
void ball_bricks(unsigned ball);  /* 1ac2:254d */
void brick_hit(unsigned slot, unsigned cell, unsigned ball);
void xor_sprite_16xn(unsigned x, unsigned y, unsigned src, unsigned rows); /* 1ac2:40f2 */
void brick_1(unsigned slot, unsigned ball);     /* 1ac2:28cb */
void brick_2(unsigned slot, unsigned ball);     /* 1ac2:2985 */
void brick_3(unsigned slot, unsigned ball);     /* 1ac2:2a3f */
void brick_solid(unsigned slot, unsigned ball); /* 1ac2:3221 */
void brick_5(unsigned slot, unsigned ball);     /* 1ac2:2a73 */
void brick_6(unsigned slot, unsigned ball);     /* 1ac2:2ab4 */
void brick_7(unsigned slot, unsigned ball);     /* 1ac2:2af5 */
void brick_8(unsigned slot, unsigned ball);     /* 1ac2:2b36 */
void brick_9(unsigned slot, unsigned ball);     /* 1ac2:2b9d */
void entity_soften(unsigned bx);      /* 1ac2:365e */
void entity_repeat(unsigned bx);      /* 1ac2:366f */
void entity_plain(unsigned bx);       /* 1ac2:3696 */
void entity_ball_arrive(unsigned bx); /* 1ac2:36a1 */
void entity_cells_timer(unsigned bx); /* 1ac2:36f6 */
void brick_10(unsigned slot, unsigned ball);    /* 1ac2:2c59 */
void brick_11(unsigned slot, unsigned ball);    /* 1ac2:2d68 */
void xor_sprite_16x7(unsigned x, unsigned y, unsigned src); /* 1ac2:3b64 */
void score_add(void);             /* 1ac2:413d */
void extra_life(void);            /* 1ac2:318b */
void fill_column(unsigned di, unsigned value);  /* 1ac2:41b1 */
void bonus_points(void);          /* 1ac2:2daa */
void bonus_catch(void);           /* 1ac2:2def */
void bonus_laser(void);           /* 1ac2:2e03 */
void bonus_multiball(void);       /* 1ac2:2e16 */
void bonus_nothing(void);         /* 1ac2:3231 */
void bonus_net(void);             /* 1ac2:3119 */
void bonus_reverse(void);         /* 1ac2:315b */
void bonus_speed(void);           /* 1ac2:31e8 */
void bonus_end_level(void);       /* 1ac2:2da0 */
void bonus_effect(unsigned kind);
void scroll_up_band(void);        /* 1ac2:2109 */
void scroll_down_band(void);      /* 1ac2:2148 */
void draw_paddle_raw(unsigned src);      /* 1ac2:22a9 */
void draw_paddle_shifted(unsigned src);  /* 1ac2:2187 */
void ball_paddle(unsigned ball);  /* 1ac2:2316 */
void laser_fire(void);            /* 1ac2:2ee3 */
void probe_cell_at(unsigned x, unsigned y, unsigned slot); /* 1ac2:2755 */
void play_teardown(void);         /* 1ac2:41d4 */
void entity_call(unsigned node);  /* the call at 1ac2:1b5e */
void entity_capsule(unsigned bx);   /* 1ac2:3273 */
void entity_paddle_fx(unsigned bx); /* 1ac2:3386 */
void morph_begin(unsigned bx, unsigned table, unsigned kind); /* 1ac2:34c5 */
void morph_step(unsigned bx);       /* 1ac2:34d7 */
void entity_popup(unsigned bx);     /* 1ac2:3561 */
void entity_capsule_frames(unsigned bx, unsigned table);
void entity_ball_hold(unsigned bx); /* 1ac2:37e0 */
void ball_place(unsigned ball, unsigned x, unsigned y);
void bonus_update(unsigned bx, unsigned nx, unsigned ny); /* 1ac2:3df1 */
unsigned pixel_xor(unsigned x, unsigned y);        /* 1ac2:30dd */
void shot_xor(unsigned x, unsigned y);             /* 1ac2:306b */
void bonus_hits_ball(unsigned bx, unsigned ball);  /* 1ac2:3f20 */
void entity_bonus(unsigned bx);     /* 1ac2:39fa */
void entity_unknown(unsigned bx);
void entity_multiball(unsigned bx);  /* 1ac2:3717 */
void entity_unlink(unsigned node);/* 1ac2:3257 */
unsigned entity_alloc(void);      /* 1ac2:3232 */
void draw_run(unsigned char c, unsigned count, unsigned di); /* 1ac2:10c5 */
void draw_cursor(unsigned di);    /* 1ac2:14a7 */
void copy_string_text(unsigned src, unsigned dst);            /* 1ac2:1642 */
void flash_bar(unsigned pattern); /* 1ac2:3146 */
void cell_set_three(unsigned node);/* 1ac2:3668 */
void cells_restore(void);         /* 1ac2:36fb */

void bonus_spawn(void);           /* 1ac2:3d95 */
void xor_sprite_20x16(unsigned x, unsigned y, unsigned src); /* 1ac2:406a */
void sprite_shift_draw(unsigned x, unsigned y, unsigned src);/* 1ac2:3f4f */
void entity_sparkle(unsigned bx); /* 1ac2:3aee */
void entity_crumble(unsigned bx); /* 1ac2:3b2a */
void entity_hatch(unsigned bx);   /* 1ac2:390d */
void bonus_release(unsigned bx);  /* 1ac2:39a1 */
int  bonus_move_right(unsigned *px, unsigned *py); /* 1ac2:3c66 */
int  bonus_move_left(unsigned *px, unsigned *py);  /* 1ac2:3cf3 */
int  bonus_move_up(unsigned *px, unsigned *py);    /* 1ac2:3caf */
int  bonus_move_down(unsigned *px, unsigned *py);  /* 1ac2:3d3c */
int  bonus_steer(unsigned bx, unsigned *px, unsigned *py);  /* 1ac2:3bf7 */
int  bonus_script(unsigned bx, unsigned *px, unsigned *py); /* 1ac2:3c35 */
void demo_input_step(void);       /* the recorded-input cursor at 0x3134 */
void game_input(void);            /* calls whichever routine [0x2d45] names */
void io_mouse_warp(unsigned x, unsigned y);
unsigned io_ticks(void);
void io_set_ticks(unsigned t);
unsigned io_mouse_x(void);
unsigned io_mouse_buttons(void);

extern jmp_buf g_back_to_menu;
void play_session(void);          /* 1ac2:02f5 */
void panel_draw(void);            /* 1ac2:0b0b */
void level_colours(void);         /* 1ac2:044b */
void level_intro(void);           /* 1ac2:1eb9 */
void draw_brick_row(unsigned y);  /* 1ac2:2034 */
void draw_sprite_20x6(unsigned x, unsigned y, unsigned src); /* 1ac2:20b9 */
void cell_special(unsigned row, unsigned di);   /* 1ac2:41e5 */
void field_backdrop(unsigned y);  /* 1ac2:1fc1 */
void life_lost(void);             /* 1ac2:0735 */
void entities_clear(void);        /* 1ac2:055e */
void level_between(void);         /* 1ac2:05f8 */
void screen_game_over(void);      /* 1ac2:0473 */
void screen_end_of_game(void);    /* 1ac2:0d2e */
void screen_level_done(void);     /* 1ac2:0521 */
void screen_all_levels_done(void);/* 1ac2:5940 */
void speaker_on(void);                                  /* 1ac2:0085 */
void speaker_off(void);                                 /* 1ac2:0090 */
void sound_tick(void);                                  /* 1ac2:0097 */
void game_delay(void);                                  /* 1ac2:164c */
void read_speed_setting(unsigned speed);                /* 1ac2:5680 */
void build_shifted_sprites(void);                       /* 1ac2:14b3 */
void load_high_scores(const char *dir);                 /* 1ac2:4d96 */
void intro_curtain(void);                               /* 1ac2:078b */
void intro_logo(void);                                  /* 1ac2:54d6 */
void intro_reveal(void);                                /* 1ac2:55e5 */
void intro_scroll(void);                                /* 1ac2:4a7a */
void game_main(const char *dir, unsigned speed);

/* --------------------------------------------------------- not yet done ---
 * Implemented as no-ops in stubs.c; see the note at the top of that file.
 */
void menu_particles_init(unsigned ax_in);   /* 1ac2:5476 */
void plot_pixel(unsigned x, unsigned y, unsigned colour);
void plot_pixel_xor(unsigned x, unsigned y, unsigned colour);
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void banner_shift(void);          /* 1ac2:5140 */
void brick_11_after(unsigned x, unsigned y);  /* 1ac2:4c4b */
unsigned particle_random(unsigned ax, unsigned ticks, unsigned limit); /* 1ac2:5448 */
unsigned particle_init(unsigned si, unsigned ax_in);  /* 1ac2:548a */
#define PARTICLES 0x148d
void menu_arrow(void);            /* 1ac2:490d */
void arrow_head(unsigned di);     /* 1ac2:492f */
void arrow_tail(unsigned di);     /* 1ac2:4957 */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void palette_cycle(void);         /* 1ac2:5196 */
void flush_keys(void);            /* 1ac2:0106 */
void install_int09(void);         /* 1ac2:03b0 */
void restore_int09(void);         /* 1ac2:03d1 */
void input_and_draw_paddle(void); /* 1ac2:48af */
void cheat_match(unsigned char c);/* 1ac2:5171 */
void io_cga_mode(unsigned v);
void io_cga_colour(unsigned v);
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void employee_leave(void);        /* 1ac2:4b4f */
void demo_start(void);            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */

void level_load_file(void);       /* 1ac2:08c8 */
unsigned char screen_player_names(void);  /* 1ac2:10de */
int  name_field(unsigned di, unsigned char *abort); /* 1ac2:13b8 */
void play_frame(void);            /* 1ac2:1212 */
unsigned frame_band(unsigned di, unsigned fill);  /* 1ac2:1354 */
void panel_reveal(void);          /* 1ac2:0911 */
void field_marks(void);           /* 1ac2:0598 */
void field_marks_wide(unsigned di);  /* 1ac2:0a1d */

void panel_finish(void);          /* 1ac2:09c5 */

/* Called by play_loop; the ones still empty live in stubs.c too. */
int  play_loop(void);             /* 1ac2:1873 - transcribed */
void draw_text(unsigned src, unsigned count, unsigned di);  /* 1ac2:10d1 */
void level_draw(void);            /* 1ac2:1c4f */
void walker_draw(unsigned x);     /* 1ac2:1e50 */
void walker_step(unsigned x);     /* 1ac2:1e23 */
void ball_draw(unsigned sprite, unsigned x, unsigned y);    /* 1ac2:2881 */
int  ball_redraw(unsigned ball);  /* 1ac2:2827 */
int  ball_on_paddle(unsigned ball); /* 1ac2:2e1e */
void read_new_key(unsigned which);  /* 1ac2:1614 */
int  score_before(unsigned si, unsigned di); /* 1ac2:108c */
void ball_after(unsigned ball);   /* 1ac2:247f */
void ball_bricks(unsigned ball);  /* 1ac2:254d */
void brick_hit(unsigned slot, unsigned cell, unsigned ball);
void xor_sprite_16xn(unsigned x, unsigned y, unsigned src, unsigned rows); /* 1ac2:40f2 */
void brick_1(unsigned slot, unsigned ball);     /* 1ac2:28cb */
void brick_2(unsigned slot, unsigned ball);     /* 1ac2:2985 */
void brick_3(unsigned slot, unsigned ball);     /* 1ac2:2a3f */
void brick_solid(unsigned slot, unsigned ball); /* 1ac2:3221 */
void brick_5(unsigned slot, unsigned ball);     /* 1ac2:2a73 */
void brick_6(unsigned slot, unsigned ball);     /* 1ac2:2ab4 */
void brick_7(unsigned slot, unsigned ball);     /* 1ac2:2af5 */
void brick_8(unsigned slot, unsigned ball);     /* 1ac2:2b36 */
void brick_9(unsigned slot, unsigned ball);     /* 1ac2:2b9d */
void entity_soften(unsigned bx);      /* 1ac2:365e */
void entity_repeat(unsigned bx);      /* 1ac2:366f */
void entity_plain(unsigned bx);       /* 1ac2:3696 */
void entity_ball_arrive(unsigned bx); /* 1ac2:36a1 */
void entity_cells_timer(unsigned bx); /* 1ac2:36f6 */
void brick_10(unsigned slot, unsigned ball);    /* 1ac2:2c59 */
void brick_11(unsigned slot, unsigned ball);    /* 1ac2:2d68 */
void xor_sprite_16x7(unsigned x, unsigned y, unsigned src); /* 1ac2:3b64 */
void score_add(void);             /* 1ac2:413d */
void extra_life(void);            /* 1ac2:318b */
void fill_column(unsigned di, unsigned value);  /* 1ac2:41b1 */
void bonus_points(void);          /* 1ac2:2daa */
void bonus_catch(void);           /* 1ac2:2def */
void bonus_laser(void);           /* 1ac2:2e03 */
void bonus_multiball(void);       /* 1ac2:2e16 */
void bonus_nothing(void);         /* 1ac2:3231 */
void bonus_net(void);             /* 1ac2:3119 */
void bonus_reverse(void);         /* 1ac2:315b */
void bonus_speed(void);           /* 1ac2:31e8 */
void bonus_end_level(void);       /* 1ac2:2da0 */
void bonus_effect(unsigned kind);
void scroll_up_band(void);        /* 1ac2:2109 */
void scroll_down_band(void);      /* 1ac2:2148 */
void draw_paddle_raw(unsigned src);      /* 1ac2:22a9 */
void draw_paddle_shifted(unsigned src);  /* 1ac2:2187 */
void ball_paddle(unsigned ball);  /* 1ac2:2316 */
void laser_fire(void);            /* 1ac2:2ee3 */
void probe_cell_at(unsigned x, unsigned y, unsigned slot); /* 1ac2:2755 */
void play_teardown(void);         /* 1ac2:41d4 */
void entity_call(unsigned node);  /* the call at 1ac2:1b5e */
void entity_capsule(unsigned bx);   /* 1ac2:3273 */
void entity_paddle_fx(unsigned bx); /* 1ac2:3386 */
void morph_begin(unsigned bx, unsigned table, unsigned kind); /* 1ac2:34c5 */
void morph_step(unsigned bx);       /* 1ac2:34d7 */
void entity_popup(unsigned bx);     /* 1ac2:3561 */
void entity_capsule_frames(unsigned bx, unsigned table);
void entity_ball_hold(unsigned bx); /* 1ac2:37e0 */
void ball_place(unsigned ball, unsigned x, unsigned y);
void bonus_update(unsigned bx, unsigned nx, unsigned ny); /* 1ac2:3df1 */
unsigned pixel_xor(unsigned x, unsigned y);        /* 1ac2:30dd */
void shot_xor(unsigned x, unsigned y);             /* 1ac2:306b */
void bonus_hits_ball(unsigned bx, unsigned ball);  /* 1ac2:3f20 */
void entity_bonus(unsigned bx);     /* 1ac2:39fa */
void entity_unknown(unsigned bx);
void entity_multiball(unsigned bx);  /* 1ac2:3717 */
void entity_unlink(unsigned node);/* 1ac2:3257 */
unsigned entity_alloc(void);      /* 1ac2:3232 */
void draw_run(unsigned char c, unsigned count, unsigned di); /* 1ac2:10c5 */
void draw_cursor(unsigned di);    /* 1ac2:14a7 */
void copy_string_text(unsigned src, unsigned dst);            /* 1ac2:1642 */
void flash_bar(unsigned pattern); /* 1ac2:3146 */
void cell_set_three(unsigned node);/* 1ac2:3668 */
void cells_restore(void);         /* 1ac2:36fb */

void bonus_spawn(void);           /* 1ac2:3d95 */
void xor_sprite_20x16(unsigned x, unsigned y, unsigned src); /* 1ac2:406a */
void sprite_shift_draw(unsigned x, unsigned y, unsigned src);/* 1ac2:3f4f */
void entity_sparkle(unsigned bx); /* 1ac2:3aee */
void entity_crumble(unsigned bx); /* 1ac2:3b2a */
void entity_hatch(unsigned bx);   /* 1ac2:390d */
void bonus_release(unsigned bx);  /* 1ac2:39a1 */
int  bonus_move_right(unsigned *px, unsigned *py); /* 1ac2:3c66 */
int  bonus_move_left(unsigned *px, unsigned *py);  /* 1ac2:3cf3 */
int  bonus_move_up(unsigned *px, unsigned *py);    /* 1ac2:3caf */
int  bonus_move_down(unsigned *px, unsigned *py);  /* 1ac2:3d3c */
int  bonus_steer(unsigned bx, unsigned *px, unsigned *py);  /* 1ac2:3bf7 */
int  bonus_script(unsigned bx, unsigned *px, unsigned *py); /* 1ac2:3c35 */
void demo_input_step(void);       /* the recorded-input cursor at 0x3134 */
void game_input(void);            /* calls whichever routine [0x2d45] names */
void io_mouse_warp(unsigned x, unsigned y);
unsigned io_ticks(void);
void io_set_ticks(unsigned t);
unsigned io_mouse_x(void);
unsigned io_mouse_buttons(void);

extern jmp_buf g_back_to_menu;
void play_session(void);          /* 1ac2:02f5 */
void panel_draw(void);            /* 1ac2:0b0b */
void level_colours(void);         /* 1ac2:044b */
void level_intro(void);           /* 1ac2:1eb9 */
void draw_brick_row(unsigned y);  /* 1ac2:2034 */
void draw_sprite_20x6(unsigned x, unsigned y, unsigned src); /* 1ac2:20b9 */
void cell_special(unsigned row, unsigned di);   /* 1ac2:41e5 */
void field_backdrop(unsigned y);  /* 1ac2:1fc1 */
void life_lost(void);             /* 1ac2:0735 */
void entities_clear(void);        /* 1ac2:055e */
void level_between(void);         /* 1ac2:05f8 */
void screen_game_over(void);      /* 1ac2:0473 */
void screen_end_of_game(void);    /* 1ac2:0d2e */
void screen_level_done(void);     /* 1ac2:0521 */
void screen_all_levels_done(void);/* 1ac2:5940 */

int verify_main(const char *in_path, const char *out_path);

#endif /* POPCORN_GAME_H */
