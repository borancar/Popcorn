/*
 * Run the port one frame at a time, beside the emulator, on the same inputs.
 *
 * The differential harness in verify.py settles one routine at a time: it
 * cannot say what happens over a hundred frames of a real game, where a
 * difference of one pixel in frame three is a ball on the other side of the
 * paddle by frame ninety. This does that instead - both sides start from the
 * same captured state, both are given the same paddle input every frame by
 * the same bot, and after every frame the whole image and the whole screen
 * are compared.
 *
 * Driving the input from outside is the point. Letting each side read its own
 * mouse would make a divergence in the picture indistinguishable from a
 * divergence in what the player did.
 *
 * The sync point is 1ac2:1a62, the top of the play loop's frame - it reloads
 * the frame delay from [0x1489], which happens exactly once a frame. The port
 * calls io_frame_sync() at the same place in play_loop.
 *
 * Protocol, on stdout and stdin so no files are involved:
 *
 *   port -> driver   "PFRM" u32 frame  u32 image_len image  u32 vram_len vram
 *   driver -> port   u16 mouse_x  u16 buttons  u32 ticks  u8 stop  3 pad
 *
 * The port writes nothing else to stdout while this is running; SDL is never
 * started, and every io_ call that would have waited or presented returns at
 * once, so a frame costs what the C costs and nothing else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int active;
static unsigned ls_frame;
static unsigned ls_mouse_x, ls_mouse_btn;

int io_lockstep(void) { return active; }
unsigned io_lockstep_mouse_x(void) { return ls_mouse_x; }
unsigned io_lockstep_buttons(void) { return ls_mouse_btn; }
void io_lockstep_warp(unsigned x) { ls_mouse_x = x; }

static void put32(unsigned v)
{
    unsigned char b[4] = { v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 };
    fwrite(b, 1, 4, stdout);
}

void io_frame_sync(void)
{
    if (!active)
        return;
    fwrite("PFRM", 1, 4, stdout);
    put32(ls_frame);
    put32(IMAGE_LEN);
    fwrite(g_image, 1, IMAGE_LEN, stdout);
    put32(CGA_SIZE);
    fwrite(g_vram, 1, CGA_SIZE, stdout);
    fflush(stdout);

    unsigned char cmd[12];
    if (fread(cmd, 1, sizeof cmd, stdin) != sizeof cmd)
        exit(0);                        /* the driver has finished with us */
    ls_mouse_x   = cmd[0] | cmd[1] << 8;
    ls_mouse_btn = cmd[2] | cmd[3] << 8;
    io_set_ticks(cmd[4] | cmd[5] << 8 | cmd[6] << 16 | (unsigned)cmd[7] << 24);
    if (cmd[8])
        exit(0);
    ls_frame++;
}

/* The same PVS2 state file verify.py writes, minus the parts only a single
 * routine needs: what matters here is the image, the screen and the tick
 * count the PRNG is seeded from. */
static int rd32(FILE *f, unsigned *out)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *out = b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24;
    return 1;
}

int lockstep_main(const char *state_path)
{
    FILE *f = fopen(state_path, "rb");
    if (!f) {
        perror(state_path);
        return 1;
    }
    char magic[4];
    unsigned routine, ticks, image_len, vram_len;
    unsigned char regs[20];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PVS2", 4) ||
        !rd32(f, &routine) || fread(regs, 1, sizeof regs, f) != sizeof regs ||
        !rd32(f, &ticks) || !rd32(f, &image_len)) {
        fprintf(stderr, "%s: not a state file\n", state_path);
        return 1;
    }
    g_image = malloc(image_len);
    if (!g_image || fread(g_image, 1, image_len, f) != image_len) {
        fprintf(stderr, "%s: truncated image\n", state_path);
        return 1;
    }
    if (!rd32(f, &vram_len) || vram_len != CGA_SIZE ||
        fread(g_vram, 1, CGA_SIZE, f) != CGA_SIZE) {
        fprintf(stderr, "%s: truncated vram\n", state_path);
        return 1;
    }
    fclose(f);
    io_set_ticks(ticks);

    active = 1;
    /* One command before anything runs. The serve wait at 1ac2:1a41 reads the
     * action button, so a port that has not been told what the player is
     * doing waits out the whole timeout while the emulator serves at once -
     * and the two are a hundred frames apart before the first comparison. */
    {
        unsigned char cmd[12];
        if (fread(cmd, 1, sizeof cmd, stdin) != sizeof cmd)
            return 0;
        ls_mouse_x   = cmd[0] | cmd[1] << 8;
        ls_mouse_btn = cmd[2] | cmd[3] << 8;
        io_set_ticks(cmd[4] | cmd[5] << 8 | cmd[6] << 16 |
                     (unsigned)cmd[7] << 24);
    }
    play_loop();                        /* 1ac2:1873 */
    return 0;
}
