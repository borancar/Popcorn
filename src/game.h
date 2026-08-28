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

extern uint8_t *g_image;          /* the whole unpacked load image */
extern const char *g_dir;               /* "" - everything is relative to the
                                         * current directory, as it was in DOS */

/* The player's POPCORN.EXE: $POPCORN_EXE, else the usual places relative to
 * where the port was started. NULL if there is none. */
const char *find_exe(void);
/* find_exe, then unpack into g_image. Says what it found, or why it found
 * nothing. Returns the length, or 0. */
size_t popcorn_load_image(void);

/* Read the game's data out of the player's own POPCORN.EXE. */
uint8_t *exepack_load(const char *path, size_t *out_len);

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

extern uint8_t g_vram[CGA_SIZE];

/* Step a CGA offset on by one scan line, the way the game does. */
static inline uint32_t cga_next_row(uint32_t di)
{
    return di < CGA_PLANE ? di + CGA_PLANE : di - (CGA_PLANE - CGA_STRIDE);
}

/* And back up one. The test is `cmp di,0x2000` and then whichever branch the
 * routine happens to use, and they are not all the same - which matters only
 * at exactly 0x2000, and there it matters a lot. Counted over the whole code
 * segment: 103 sites step down with `jb`, five step up treating 0x2000 as the
 * odd half (`jae` at 0xc4c, 0x46e5, 0x483a and `jb` at 0x9f3, 0x1320), and
 * four - all inside intro_logo - use `ja` and treat 0x2000 as the even half.
 *
 * Getting this wrong sends an offset of 0x2000 to 0x3fb0, past the bottom of
 * the visible screen into the padding at the end of the plane. That is what
 * the frame's scroll was doing. */
static inline uint32_t cga_prev_row(uint32_t di)
{
    return di >= CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
}

/* intro_logo's pair: `ja`, so 0x2000 goes the other way. Only 1ac2:54d6 uses
 * these, at 54f6, 5535, 557a and 55b6. */
static inline uint32_t cga_prev_row_ja(uint32_t di)
{
    return di > CGA_PLANE ? di - CGA_PLANE : di + (CGA_PLANE - CGA_STRIDE);
}

static inline uint32_t cga_next_row_ja(uint32_t di)
{
    return di > CGA_PLANE ? di - (CGA_PLANE - CGA_STRIDE) : di + CGA_PLANE;
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
int32_t  io_init(int32_t scale);
void io_shutdown(void);
void io_present(void);                 /* g_vram to the screen */

/* Lockstep, for sidebyside.py: the port runs a frame and hands the whole
 * machine over, and the driver hands back the input for the next one. */
int32_t  io_lockstep(void);
uint32_t io_lockstep_mouse_x(void);
uint32_t io_lockstep_buttons(void);
void io_lockstep_warp(uint32_t x);
void io_frame_sync(void);              /* at 1ac2:1c3f, the frame's close */
void io_log_random(uint32_t dl);       /* one game_random, for sidebyside */
int32_t  lockstep_main(const char *state_path);
uint64_t io_ms(void);             /* wall clock, for measurement only */
int32_t  io_pump(void);                    /* poll input; 0 means quit */
void io_wait_retrace(void);            /* what `in al,0x3da` / test 8 did */
void io_sound(uint32_t divisor);       /* PIT channel 2; 0 silences */
void io_delay_cycles(uint32_t cycles); /* what the busy-wait at 0x164c cost */
void io_frame_pace(void);              /* one play-loop frame, against the 60 Hz screen */
extern uint32_t g_play_hz;             /* measured play-loop rate, 326 Hz */
int32_t  io_key_ready(void);               /* INT 16h AH=01 */
uint32_t io_get_key(void);             /* INT 16h AH=00: scan<<8 | ascii */
void io_flush_keys(void);
void io_script_key_shift(uint32_t scan, uint32_t ms, int32_t shift);
#define io_script_key(scan, ms) io_script_key_shift((scan), (ms), 0)
int32_t  io_save_shot(const char *path);
void io_set_deadline(uint32_t ms, const char *shot, const char *vram);
void io_set_deadline_image(const char *path);
void io_set_int09_installed(int32_t on);

/* Little-endian accessors, so a transcribed `mov ax,[0x3144]` reads the way it
 * reads in the disassembly. */
static inline uint32_t img_w(uint32_t off)
{
    return (uint32_t)g_image[off] | ((uint32_t)g_image[off + 1] << 8);
}
static inline void img_setw(uint32_t off, uint32_t v)
{
    g_image[off] = (uint8_t)v;
    g_image[off + 1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------- the game code ---
 *
 * Each carries the image offset it was transcribed from; verify.c maps those
 * offsets to these calls, and the mapping of registers to arguments is part of
 * what the check asserts.
 */
void ball_step(uint32_t ball);                          /* 1ac2:27d7 */
void input_keyboard(void);                              /* 1ac2:1712 */
void input_mouse(uint32_t mouse_x, uint32_t buttons);   /* 1ac2:169f */
void save_screen(void);                                 /* 1ac2:5099 */
void restore_screen(void);                              /* 1ac2:50bc */
void paddle_row_offsets(uint32_t x, uint32_t rows_out); /* 1ac2:22de */
void blit_xor(uint32_t pixels, uint32_t rows);          /* 1ac2:2281 */
void draw_paddle(uint32_t sprite);                      /* 1ac2:221a */
void draw_char(uint8_t c, uint32_t di);           /* 1ac2:0c64 */
uint32_t game_random(uint32_t ticks, uint32_t limit);   /* 1ac2:40c0 */
void speaker_on(void);                                  /* 1ac2:0085 */
void speaker_off(void);                                 /* 1ac2:0090 */
void sound_tick(void);                                  /* 1ac2:0097 */
void io_set_grab(int32_t on);
/* Opt-in lockstep sync points, one bit each, for comparing inside a screen
 * that has a loop of its own and so reaches io_frame_sync not at all. */
#ifndef SYNC_SCROLL
#define SYNC_SCROLL   1                 /* screen_scroll_up, once a row */
#define SYNC_ENDGAME  2                 /* ball_after_endgame, once a step */
#define SYNC_RESULTS  4                 /* screen_results' wait, once a pass */
#define SYNC_CURTAIN  8                 /* the ending animation, once a pass */
#define SYNC_ENDING  16                 /* after level 49, once a pass */
#define SYNC_INTRO   32                 /* the level intro, once a pass */
#endif
void io_frame_sync_extra(int32_t which);
void io_lockstep_extra_sync(int32_t mask);
int32_t io_grabbed(void);
void game_delay(void);                                  /* 1ac2:164c */
void read_speed_setting(uint32_t speed);                /* 1ac2:5680 */
void build_shifted_sprites(void);                       /* 1ac2:14b3 */
void load_high_scores(const char *dir);                 /* 1ac2:4d96 */
void intro_curtain(void);                               /* 1ac2:078b */
void intro_logo(void);                                  /* 1ac2:54d6 */
void intro_reveal(void);                                /* 1ac2:55e5 */
void intro_scroll(void);                                /* 1ac2:4a7a */
void intro_paddle(void);                                /* 1ac2:49bc */
void int09_handler(uint32_t scan);                      /* 1ac2:03e3 */
int32_t  drive_check(void);                                 /* 1ac2:4dea */
int32_t  drive_writable(void);                              /* 1ac2:4e04 */
#define LAST_DIR 0x2d4a
void game_main(const char *dir, const char *levels);

/* --------------------------------------------------------- not yet done ---
 * Implemented as no-ops in stubs.c; see the note at the top of that file.
 */
void menu_particles_init(uint32_t ax_in);   /* 1ac2:5476 */
void plot_pixel(uint32_t x, uint32_t y, uint32_t colour);
void plot_pixel_xor(uint32_t x, uint32_t y, uint32_t colour);
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void banner_shift(void);          /* 1ac2:5140 */
void brick_11_after(uint32_t x, uint32_t y);  /* 1ac2:4c4b */
uint32_t particle_random(uint32_t ax, uint32_t ticks, uint32_t limit); /* 1ac2:5448 */
uint32_t particle_init(uint32_t si, uint32_t ax_in);  /* 1ac2:548a */
#define PARTICLES 0x148d
void menu_arrow(void);            /* 1ac2:490d */
void arrow_head(uint32_t di);     /* 1ac2:492f */
void arrow_tail(uint32_t di);     /* 1ac2:4957 */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void hsc_sort(void);              /* 1ac2:4d37 */
void hsc_save(const char *dir);   /* 1ac2:4dbb */
void border_draw(uint32_t di);    /* 1ac2:4ff1 */
void border_erase(uint32_t di);   /* 1ac2:4fd3 */
uint32_t border_step(uint32_t di);/* 1ac2:4fa7 */
void border_animate(void);        /* 1ac2:4f58 */
void border_row(uint32_t di);     /* 1ac2:5019 */
void border_block(uint32_t di);   /* 1ac2:5045 */
void palette_cycle(void);         /* 1ac2:5196 */
void flush_keys(void);            /* 1ac2:0106 */
void install_int09(void);         /* 1ac2:03b0 */
void restore_int09(void);         /* 1ac2:03d1 */
void input_and_draw_paddle(void); /* 1ac2:48af */
void cheat_match(uint8_t c);/* 1ac2:5171 */
void io_cga_mode(uint32_t v);
void io_cga_colour(uint32_t v);
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void cell_hole_draw(uint32_t x, uint32_t y);  /* 1ac2:4cc1 */
void screen_unstash(void);        /* 1ac2:4c13 */
void border_setup(void);          /* 1ac2:4f73 */
int32_t  tall_sprite(uint32_t *si, uint32_t di);   /* 1ac2:538d */
void screen_scroll_up(void);      /* 1ac2:4878 */
void level_tally(void);           /* 1ac2:48ce */
void screen_stash(void);          /* 1ac2:4ba9 */
void screen_restore(void);        /* 1ac2:4b4f */
void demo_start(void);
void input_demo(void);            /* 1ac2:1785 */
int32_t  cheat_sequence(uint8_t key); /* 1ac2:58b3 */            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */

int32_t  level_load_file(const char *dir);  /* 1ac2:08c8 */
void set_palette_registers(uint32_t table);  /* 1ac2:4b7a */
uint8_t screen_player_names(void);  /* 1ac2:10de */
int32_t  name_field(uint32_t di, uint8_t *abort); /* 1ac2:13b8 */
void play_frame(void);            /* 1ac2:1212 */
uint32_t frame_band(uint32_t di, uint32_t fill);  /* 1ac2:1354 */
void panel_reveal(void);          /* 1ac2:0911 */
void field_marks(void);           /* 1ac2:0598 */
void field_marks_wide(uint32_t di, uint32_t rows);  /* 1ac2:0a1d */
uint32_t ending_particle_init(uint32_t si, uint32_t ax_in); /* 1ac2:59f7 */
void ending_blob(uint32_t pos);   /* 1ac2:5c36 */
uint32_t ending_blobs(void);          /* 1ac2:5b80 */
void ending_column(void);         /* 1ac2:5317 */

/* A word into the framebuffer, wrapping like the 16-bit offset it is. */
static inline void img_vram_setw(uint32_t di, uint32_t v)
{
    g_vram[di & (CGA_SIZE - 1)] = (uint8_t)v;
    g_vram[(di + 1) & (CGA_SIZE - 1)] = (uint8_t)(v >> 8);
}
void panel_finish(void);          /* 1ac2:09c5 */

/* Called by play_loop; the ones still empty live in stubs.c too. */
extern int32_t g_resume_at_frame_top;   /* lockstep: skip play_loop's prologue */
extern int32_t g_resume_in_session;     /* lockstep: rejoin play_session's retry loop */
extern int32_t g_resume_in_bonus;       /* lockstep: rejoin inside the bonus */
extern int32_t g_start_level;           /* --level N, or -1 for the first */
extern int32_t g_in_bonus;              /* the end-of-level bonus is running */
int32_t  play_loop(void);             /* 1ac2:1873 - transcribed */
uint32_t draw_text(uint32_t src, uint32_t count, uint32_t di);  /* 1ac2:10d1 */
void level_draw(void);            /* 1ac2:1c4f */
void walker_draw(uint32_t x);     /* 1ac2:1e50 */
void walker_step(uint32_t x);     /* 1ac2:1e23 */
void ball_draw(uint32_t sprite, uint32_t x, uint32_t y);    /* 1ac2:2881 */
int32_t  ball_redraw(uint32_t ball);  /* 1ac2:2827 */
int32_t  ball_on_paddle(uint32_t ball); /* 1ac2:2e1e */
void read_new_key(uint32_t which);  /* 1ac2:1614 */
int32_t  score_before(uint32_t si, uint32_t di); /* 1ac2:108c */
void ball_after(uint32_t ball);   /* 1ac2:247f */
int32_t  ball_after_endgame(uint32_t ball);  /* 1ac2:45a1 */
void ball_bricks(uint32_t ball);  /* 1ac2:254d */
void brick_hit(uint32_t slot, uint32_t cell, uint32_t ball);
void xor_sprite_16xn(uint32_t x, uint32_t y, uint32_t src, uint32_t rows); /* 1ac2:40f2 */
void brick_1(uint32_t slot, uint32_t ball);     /* 1ac2:28cb */
void brick_2(uint32_t slot, uint32_t ball);     /* 1ac2:2985 */
void brick_3(uint32_t slot, uint32_t ball);     /* 1ac2:2a3f */
void brick_solid(uint32_t slot, uint32_t ball);       /* 1ac2:3221 */
void brick_animated(uint32_t slot, uint32_t ball);   /* 1ac2:2ccd */
void entity_anim_brick(uint32_t bx);                 /* 1ac2:3abf */
void draw_anim_cell(uint32_t si, uint32_t x, uint32_t y); /* 1ac2:3bac */
void brick_5(uint32_t slot, uint32_t ball);     /* 1ac2:2a73 */
void brick_6(uint32_t slot, uint32_t ball);     /* 1ac2:2ab4 */
void brick_7(uint32_t slot, uint32_t ball);     /* 1ac2:2af5 */
void brick_8(uint32_t slot, uint32_t ball);     /* 1ac2:2b36 */
void brick_9(uint32_t slot, uint32_t ball);     /* 1ac2:2b9d */
void entity_soften(uint32_t bx);      /* 1ac2:365e */
void entity_repeat(uint32_t bx);      /* 1ac2:366f */
void entity_plain(uint32_t bx);       /* 1ac2:3696 */
void entity_ball_arrive(uint32_t bx); /* 1ac2:36a1 */
void entity_cells_timer(uint32_t bx); /* 1ac2:36f6 */
void brick_10(uint32_t slot, uint32_t ball);    /* 1ac2:2c59 */
void brick_11(uint32_t slot, uint32_t ball);    /* 1ac2:2d68 */
void xor_sprite_16x7(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:3b64 */
void score_add(void);             /* 1ac2:413d */
void extra_life(void);            /* 1ac2:318b */
void fill_column(uint32_t di, uint32_t value);  /* 1ac2:41b1 */
void bonus_points(void);          /* 1ac2:2daa */
void bonus_catch(void);           /* 1ac2:2def */
void bonus_laser(void);           /* 1ac2:2e03 */
void bonus_multiball(void);       /* 1ac2:2e16 */
void bonus_wider_paddle(void);         /* 1ac2:3231 */
void bonus_net(void);             /* 1ac2:3119 */
void bonus_reverse(void);         /* 1ac2:315b */
void bonus_slower_ball(void);     /* 1ac2:31e8 */
void bonus_stop_monsters(void);   /* 1ac2:3200 */
int32_t bonus_end_level(void);    /* 1ac2:2da0 */
int32_t bonus_end_level_body(void); /* 1ac2:4210 */
void bonus_effect(uint32_t kind);
void scroll_up_band(void);        /* 1ac2:2109 */
void scroll_down_band(void);      /* 1ac2:2148 */
void draw_paddle_raw(uint32_t src);      /* 1ac2:22a9 */
void draw_paddle_shifted(uint32_t src);  /* 1ac2:2187 */
void ball_paddle(uint32_t ball);  /* 1ac2:2316 */
void laser_fire(void);            /* 1ac2:2ee3 */
void probe_cell_at(uint32_t x, uint32_t y, uint32_t slot); /* 1ac2:2755 */
void play_teardown(void);         /* 1ac2:41d4 */
void entity_call(uint32_t node);  /* the call at 1ac2:1b5e */
void entity_capsule(uint32_t bx);   /* 1ac2:3273 */
void entity_paddle_fx(uint32_t bx); /* 1ac2:3386 */
void morph_begin(uint32_t bx, uint32_t table, uint32_t kind); /* 1ac2:34c5 */
void morph_step(uint32_t bx);       /* 1ac2:34d7 */
void entity_popup(uint32_t bx);     /* 1ac2:3561 */
void entity_capsule_frames(uint32_t bx, uint32_t table);
void entity_ball_hold(uint32_t bx); /* 1ac2:37e0 */
void ball_place(uint32_t ball, uint32_t x, uint32_t y);
void bonus_update(uint32_t bx, uint32_t nx, uint32_t ny); /* 1ac2:3df1 */
uint32_t pixel_xor(uint32_t x, uint32_t y);        /* 1ac2:30dd */
void shot_xor(uint32_t x, uint32_t y);             /* 1ac2:306b */
void bonus_hits_ball(uint32_t bx, uint32_t ball);  /* 1ac2:3f20 */
void entity_bonus(uint32_t bx);     /* 1ac2:39fa */
void entity_unknown(uint32_t bx);
void entity_multiball(uint32_t bx);  /* 1ac2:3717 */
void entity_unlink(uint32_t node);/* 1ac2:3257 */
uint32_t entity_alloc(void);      /* 1ac2:3232 */
uint32_t draw_run(uint8_t c, uint32_t count, uint32_t di); /* 1ac2:10c5 */
void draw_cursor(uint32_t di);    /* 1ac2:14a7 */
void define_keys_prompt(uint32_t src, uint32_t dst);            /* 1ac2:1642 */
void flash_bar(uint32_t pattern); /* 1ac2:3146 */
void cell_set_three(uint32_t node);/* 1ac2:3668 */
void cells_restore(void);         /* 1ac2:36fb */

void bonus_spawn(void);           /* 1ac2:3d95 */
void xor_sprite_20x16(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:406a */
void sprite_shift_draw(uint32_t x, uint32_t y, uint32_t src);/* 1ac2:3f4f */
void entity_sparkle(uint32_t bx); /* 1ac2:3aee */
void entity_crumble(uint32_t bx); /* 1ac2:3b2a */
void entity_hatch(uint32_t bx);   /* 1ac2:390d */
void bonus_release(uint32_t bx);  /* 1ac2:39a1 */
int32_t  bonus_move_right(uint32_t bx, uint32_t *px, uint32_t *py); /* 1ac2:3c66 */
int32_t  bonus_move_left(uint32_t bx, uint32_t *px, uint32_t *py);  /* 1ac2:3cf3 */
int32_t  bonus_move_up(uint32_t bx, uint32_t *px, uint32_t *py);    /* 1ac2:3caf */
int32_t  bonus_move_down(uint32_t bx, uint32_t *px, uint32_t *py);  /* 1ac2:3d3c */
int32_t  bonus_steer(uint32_t bx, uint32_t *px, uint32_t *py);  /* 1ac2:3bf7 */
int32_t  bonus_script(uint32_t bx, uint32_t *px, uint32_t *py); /* 1ac2:3c35 */
void demo_input_step(void);       /* 1ac2:1a6f */
void drop_duplicate_hits(void);   /* 1ac2:27b7 */
uint32_t hsc_bubble(uint32_t si, uint32_t di); /* 1ac2:4d5d */
void game_input(void);            /* calls whichever routine [0x2d45] names */
void io_mouse_warp(uint32_t x, uint32_t y);
uint32_t io_ticks(void);
void io_set_ticks(uint32_t t);
void io_pin_mouse(uint32_t x, uint32_t buttons);
/* The bot, in autoplay.c. --autoplay on popcorn-dev; it plays through
 * io_pin_mouse, which is the same door lockstep uses. */
void autoplay_enable(uint32_t seed);
int32_t autoplay_on(void);
void autoplay_step(void);
void io_pin_key(uint32_t k);
uint32_t io_mouse_x(void);
uint32_t io_mouse_buttons(void);

extern jmp_buf g_back_to_menu;
extern jmp_buf g_bonus_done;
void play_session(void);          /* 1ac2:02f5 */
void panel_draw(void);            /* 1ac2:0b0b */
void level_colours(void);         /* 1ac2:044b */
void level_intro(void);           /* 1ac2:1eb9 */
uint32_t draw_brick_row(uint32_t y);  /* 1ac2:2034 */
void draw_sprite_20x6(uint32_t x, uint32_t y, uint32_t src); /* 1ac2:20b9 */
void cell_special(uint32_t row, uint32_t col, uint32_t di); /* 1ac2:41e5 */
void field_backdrop(uint32_t y);  /* 1ac2:1fc1 */
void life_lost(void);             /* 1ac2:0735 */
void entities_clear(void);        /* 1ac2:055e */
void level_between(void);         /* 1ac2:05f8 */
void screen_game_over(void);      /* 1ac2:0473 */
void ending_plot(uint32_t x, uint32_t y);     /* 1ac2:5add */
void ending_particles_init(uint32_t ax); /* 1ac2:5a43 */
void ending_particles_tick(void); /* 1ac2:5a56 */
int32_t next_player(const char *dir);/* 1ac2:0d2e */
void screen_results(const char *dir);   /* 1ac2:0ea3 */
void screen_end_of_game(void);
void screen_level_done(void);     /* 1ac2:0521 */
void screen_all_levels_done(void);/* 1ac2:5940 */
uint32_t ending_walk(uint32_t bl, uint32_t bh, uint32_t dx); /* 1ac2:5bb5 */

int32_t verify_main(const char *in_path, const char *out_path);

#endif /* POPCORN_GAME_H */
