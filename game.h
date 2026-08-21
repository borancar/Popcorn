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
extern const uint32_t g_palette[4];

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

/* The entity list the play loop walks: a chain from the head link at 0x3144,
 * each node carrying its handler at +0x00 and the next link at +0x0c, with
 * 0xffff terminating.  The node pool is at 0x3146, stride 0x0e. */
#define ENTITY_HEAD   0x3144
#define ENTITY_POOL   0x3146
#define ENTITY_STRIDE 0x0e
#define E_HANDLER     0x00
#define E_NEXT        0x0c

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
void menu_particles_init(void);   /* 1ac2:5476 */
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void menu_arrow(void);            /* 1ac2:490d */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void palette_cycle(void);         /* 1ac2:5196 */
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void employee_leave(void);        /* 1ac2:4b4f */
void demo_prepare(void);          /* 1ac2:1212 */
void demo_start(void);            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */
void play_loop(void);             /* 1ac2:1873 */
void level_load_file(void);       /* 1ac2:08c8 */
unsigned char screen_player_names(void);  /* 1ac2:10de */
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
void menu_particles_init(void);   /* 1ac2:5476 */
void menu_particles_tick(void);   /* 1ac2:53c2 */
void menu_banner_tick(void);      /* 1ac2:50df */
void menu_arrow(void);            /* 1ac2:490d */
void screen_define_keys(void);    /* 1ac2:1581 */
void screen_high_scores(void);    /* 1ac2:4e1a */
void palette_cycle(void);         /* 1ac2:5196 */
void menu_extra(void);            /* 1ac2:5171 */
void employee_enter(void);        /* 1ac2:4ae0 */
void employee_leave(void);        /* 1ac2:4b4f */
void demo_prepare(void);          /* 1ac2:1212 */
void demo_start(void);            /* 1ac2:1509 */
void play_prepare(void);          /* 1ac2:0cc5 */
void play_loop(void);             /* 1ac2:1873 */
void level_load_file(void);       /* 1ac2:08c8 */
unsigned char screen_player_names(void);  /* 1ac2:10de */

int verify_main(const char *in_path, const char *out_path);

#endif /* POPCORN_GAME_H */
