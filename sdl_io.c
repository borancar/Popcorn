/*
 * The platform layer, on SDL3.
 *
 * What the original did in hardware, and what stands in for it here:
 *
 *   INT 10h AX=0005          a 320x200 window; g_vram is the 0xb8000 aperture
 *   in al,0x3da / test al,8  a 60 Hz deadline instead of a spin
 *   INT 09h at port 0x60     SDL key events into the same three state bytes
 *   INT 33h AX=0003          the pointer, in the 640-wide space the game reads
 *   PIT channel 2 + 0x61     a square wave on an SDL audio stream
 *
 * The retrace wait is the interesting one.  The game waits for bit 3 to rise
 * and then to fall, once per screen blit, and that wait is half of what paces
 * it - the other half is the busy-wait at 1ac2:164c.  Spinning here would burn
 * a core for nothing, so it sleeps to the next 60 Hz boundary instead, which
 * is the same elapsed time without the spin.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "game.h"

uint8_t g_vram[CGA_SIZE];

/* Mode 05h on an RGB monitor: the colour-burst-kill bit in the mode-control
 * register selects this palette whatever the palette bit says.  Entry 0 is the
 * background from the colour-select register, black at the BIOS default. */
uint32_t g_palette[4] = {
    0xff000000,     /* black */
    0xff55ffff,     /* cyan */
    0xffff5555,     /* red */
    0xffffffff,     /* white */
};

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioStream *audio;
static int32_t win_scale = 3;
static int32_t quit_requested;
static uint64_t next_present_ns;
static uint64_t next_retrace_ns;
static uint32_t tone_divisor;

/* The BIOS keyboard buffer the menus read through INT 16h. Sixteen entries,
 * as the real one had, each `scan << 8 | ascii`. */
#define KEYQ 16
static uint32_t key_q[KEYQ];
static int32_t key_head, key_tail;

/* Time owed to the game's busy-wait at 0x164c. It is called in tight loops,
 * so each call is accumulated and paid off only when enough has built up to
 * be worth a syscall - the elapsed time comes out the same and no core is
 * burned spinning. */
static double delay_owed_ns;

#define FRAME_NS (SDL_NS_PER_SECOND / 60)

int32_t io_init(int32_t scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "popcorn: SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    if (scale < 1)
        scale = 1;
    win_scale = scale;
    if (!SDL_CreateWindowAndRenderer("Popcorn", CGA_W * scale, CGA_H * scale,
                                     0, &win, &ren)) {
        fprintf(stderr, "popcorn: SDL_CreateWindowAndRenderer: %s\n",
                SDL_GetError());
        return 0;
    }
    SDL_SetRenderLogicalPresentation(ren, CGA_W, CGA_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, CGA_W, CGA_H);
    if (!tex) {
        fprintf(stderr, "popcorn: SDL_CreateTexture: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec want = { SDL_AUDIO_S16, 1, 22050 };
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                      &want, NULL, NULL);
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);

    next_retrace_ns = SDL_GetTicksNS();
    return 1;
}

void io_shutdown(void)
{
    if (audio) SDL_DestroyAudioStream(audio);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

static void present_now(void)
{
    if (getenv("POPCORN_FPS")) {
        static uint64_t t0; static uint32_t n;
        uint64_t now = SDL_GetTicksNS();
        if (!t0) t0 = now;
        n++;
        if (now - t0 >= SDL_NS_PER_SECOND) {
            fprintf(stderr, "popcorn: [fps] %u presents/s\n", n);
            n = 0; t0 = now;
        }
    }
    uint32_t *px;
    int32_t pitch;
    if (!SDL_LockTexture(tex, NULL, (void **)&px, &pitch))
        return;
    for (int32_t y = 0; y < CGA_H; y++) {
        const uint8_t *row =
            g_vram + (y & 1 ? CGA_PLANE : 0) + (y >> 1) * CGA_STRIDE;
        uint32_t *out = (uint32_t *)((uint8_t *)px + (size_t)y * pitch);
        for (int32_t x = 0; x < CGA_STRIDE; x++) {
            uint32_t b = row[x];
            *out++ = g_palette[(b >> 6) & 3];
            *out++ = g_palette[(b >> 4) & 3];
            *out++ = g_palette[(b >> 2) & 3];
            *out++ = g_palette[b & 3];
        }
    }
    SDL_UnlockTexture(tex);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* Put the framebuffer on the screen, at most sixty times a second.
 *
 * The throttle belongs here, not only in keep_alive: the transcribed routines
 * call io_present themselves wherever the original finished a picture, and a
 * routine that finishes several in a frame - the level draw finishes four -
 * used to present every one of them. Offscreen that costs nothing and the
 * rate sat at sixty; through a real compositor each present is a texture
 * upload the game loop has to wait for, so the game ran slower the more it
 * drew. Measured at 116 presents a second on the menu.
 *
 * On the original there was no such thing as presenting: the CRT read the
 * framebuffer continuously, so drawing twice between two scans showed only
 * the second. Dropping a present nothing could have seen is what the hardware
 * did, not a shortcut. */
uint64_t io_ms(void)
{
    return (uint64_t)SDL_GetTicks();
}

void io_present(void)
{
    if (io_lockstep())
        return;
    uint64_t now = SDL_GetTicksNS();
    if (now < next_present_ns)
        return;
    next_present_ns = now + SDL_NS_PER_SECOND / 60;
    present_now();
}

/* Show what has been drawn and answer the window manager. Called from the
 * busy-wait as well as the retrace wait, because the game spends the whole
 * opening sequence inside the busy-wait and nothing else would run: on the
 * original the screen was simply always live, and a port that only presents
 * from its frame loop leaves the window blank and unresponsive for the first
 * half-minute. Shares io_present's budget, so the two cannot double up. */
static void keep_alive(void)
{
    if (io_lockstep())
        return;
    uint64_t now = SDL_GetTicksNS();
    if (now < next_present_ns)
        return;
    next_present_ns = now + SDL_NS_PER_SECOND / 60;
    present_now();
    io_pump();
}

void io_delay_cycles(uint32_t cycles)
{
    if (io_lockstep())
        return;
    delay_owed_ns += cycles * (1e9 / 8000000.0);   /* an 8 MHz 8086 */
    if (delay_owed_ns >= 1e6) {                    /* a millisecond or more */
        uint64_t ns = (uint64_t)delay_owed_ns;
        delay_owed_ns -= ns;
        SDL_DelayNS(ns);
    }
    keep_alive();
}

/* The pointer, in the 640-wide virtual screen INT 33h reports - the game does
 * `shr cx,1` on it to get a 320-pixel x. Tracked from motion events rather
 * than polled, so it is right even when the pointer leaves the window.
 *
 * The renderer is letterboxing a 320x200 logical size into whatever the window
 * is, so window pixels have to go through SDL_RenderCoordinatesFromWindow
 * before they mean anything - scaling by the window size alone is wrong the
 * moment the window is not exactly 8:5.
 */
static float mouse_x = 320, mouse_y = 100;

void io_mouse_warp(uint32_t x, uint32_t y)
{
    /* A warp is what the next read returns, in lockstep as much as here: the
     * play loop centres the pointer at 1ac2:1925 before the serve, and a port
     * that kept the driver's last value would start the paddle somewhere the
     * emulator never put it. */
    if (io_lockstep()) {
        io_lockstep_warp(x);
        return;
    }
    mouse_x = (float)x;
    mouse_y = (float)y;
    /* Put the real pointer where the game thinks it is, or the next motion
     * event snaps the paddle back to wherever it actually was. */
    if (win)
        SDL_WarpMouseInWindow(win, x * 0.5f * (float)win_scale,
                              y * (float)win_scale);
}

uint32_t io_mouse_x(void)
{
    if (io_lockstep())
        return io_lockstep_mouse_x();
    return (uint32_t)(mouse_x < 0 ? 0 : mouse_x > 639 ? 639 : mouse_x);
}
uint32_t io_mouse_buttons(void)
{
    if (io_lockstep())
        return io_lockstep_buttons();
    float fx, fy;
    return SDL_GetMouseState(&fx, &fy) & 3;
}

/* The BIOS tick counter at 0040:006c, which the PRNG stirs in. Overridable so
 * that a verification run can be handed the value the original saw - otherwise
 * every routine that consults random() diverges for a reason that is not a
 * bug. */
static uint32_t forced_ticks;
static int32_t ticks_forced;

void io_set_ticks(uint32_t t)
{
    forced_ticks = t;
    ticks_forced = 1;
}

uint32_t io_ticks(void)
{
    if (ticks_forced)
        return forced_ticks;
    return (uint32_t)(SDL_GetTicks() * 182 / 10000);   /* 18.2 Hz */
}

int32_t io_key_ready(void)
{
    if (io_lockstep())
        return 0;
    return key_head != key_tail;
}

uint32_t io_get_key(void)
{
    if (key_head == key_tail)
        return 0;
    uint32_t k = key_q[key_head];
    key_head = (key_head + 1) % KEYQ;
    return k;
}

void io_flush_keys(void)
{
    key_head = key_tail = 0;
}

/* Scripted input, for unattended runs: a list of `scan@ms` fired from the
 * retrace wait, which every frame and every animation goes through. The same
 * idea as emulation.py's --keys, and used the same way - to reach a screen
 * without a person at the keyboard. */
void key_push(uint32_t scan, uint32_t ascii);

#define SCRIPT_MAX 32
static struct { uint32_t scan, ms; int32_t done; } script[SCRIPT_MAX];
static int32_t script_n;

void io_script_key(uint32_t scan, uint32_t ms)
{
    if (script_n < SCRIPT_MAX) {
        script[script_n].scan = scan;
        script[script_n].ms = ms;
        script[script_n].done = 0;
        script_n++;
    }
}

/* What the BIOS would have put in its buffer beside the scan code. Real key
 * events get this from SDL; a scripted one had nothing, so --keys could press
 * F1 but could not type a player's name - name_field reads the ASCII byte and
 * saw zero every time. Set 1 scan codes, the printable half of the keyboard. */
static uint32_t ascii_of_scan(uint32_t sc)
{
    static const char t[0x3a] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
        [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
        [0x0c] = '-', [0x0e] = 0x08, [0x01] = 0x1b, [0x1c] = 0x0d,
        [0x39] = ' ',
        [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
        [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
        [0x1e] = 'A', [0x1f] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
        [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
        [0x2c] = 'Z', [0x2d] = 'X', [0x2e] = 'C', [0x2f] = 'V', [0x30] = 'B',
        [0x31] = 'N', [0x32] = 'M',
    };
    return sc < sizeof t ? (uint8_t)t[sc] : 0;
}

static void script_pump(void)
{
    if (!script_n)
        return;
    uint64_t now = SDL_GetTicks();
    for (int32_t i = 0; i < script_n; i++) {
        if (script[i].done || now < script[i].ms)
            continue;
        script[i].done = 1;
        uint32_t sc = script[i].scan;
        key_push(sc, ascii_of_scan(sc));
        if (sc == g_image[KEY_SCAN_L]) g_image[KEY_LEFT] = 1;
        if (sc == g_image[KEY_SCAN_R]) g_image[KEY_RIGHT] = 1;
        if (sc == g_image[KEY_SCAN_A]) g_image[KEY_ACTION] = 1;
        fprintf(stderr, "popcorn: [keys] scan %#04x at %ums\n", sc, script[i].ms);
    }
}

void key_push(uint32_t scan, uint32_t ascii)
{
    int32_t next = (key_tail + 1) % KEYQ;
    if (next == key_head)
        return;                          /* full: the real BIOS beeped */
    key_q[key_tail] = scan << 8 | ascii;
    key_tail = next;
}

/* Write the framebuffer out as it stands, for comparing against the emulator.
 * Decodes exactly the way io_present() does, so a difference in the picture is
 * a difference in the game and not in how it was saved. */
int32_t io_save_shot(const char *path)
{
    SDL_Surface *s = SDL_CreateSurface(CGA_W, CGA_H, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return 0;
    for (int32_t y = 0; y < CGA_H; y++) {
        const uint8_t *row =
            g_vram + (y & 1 ? CGA_PLANE : 0) + (y >> 1) * CGA_STRIDE;
        uint32_t *out = (uint32_t *)((uint8_t *)s->pixels
                                     + (size_t)y * s->pitch);
        for (int32_t x = 0; x < CGA_STRIDE; x++) {
            uint32_t b = row[x];
            *out++ = g_palette[(b >> 6) & 3];
            *out++ = g_palette[(b >> 4) & 3];
            *out++ = g_palette[(b >> 2) & 3];
            *out++ = g_palette[b & 3];
        }
    }
    int32_t ok = SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    return ok;
}

/* A deadline for unattended runs: --run-ms. Checked from the retrace wait,
 * which every animation and every frame goes through. */
static uint64_t deadline_ns;
static const char *shot_path;
static const char *vram_path;

void io_set_deadline(uint32_t ms, const char *shot, const char *vram)
{
    deadline_ns = ms ? SDL_GetTicksNS() + (uint64_t)ms * SDL_NS_PER_MS : 0;
    shot_path = shot;
    vram_path = vram;
}

/* The unattended deadline. Checked from both the retrace wait and the event
 * pump, because the play loop goes through the pump every frame but not
 * through the retrace wait - it paces itself on its own delay instead. */
static void check_deadline(void)
{
    if (!deadline_ns || SDL_GetTicksNS() < deadline_ns)
        return;
    if (shot_path)
        io_save_shot(shot_path);
    if (vram_path) {
        FILE *f = fopen(vram_path, "wb");
        if (f) {
            fwrite(g_vram, 1, CGA_SIZE, f);
            fclose(f);
        }
    }
    io_shutdown();
    exit(0);
}

void io_wait_retrace(void)
{
    if (io_lockstep())
        return;
    script_pump();
    check_deadline();
    keep_alive();
    uint64_t now = SDL_GetTicksNS();
    if (next_retrace_ns > now)
        SDL_DelayNS(next_retrace_ns - now);
    else
        next_retrace_ns = now;              /* behind: do not build up debt */
    next_retrace_ns += FRAME_NS;
}

/* Scan codes, so the game's own key-configuration screen keeps working: it
 * stores whatever the keyboard produced, and compares against it later. */
static int32_t scancode_of(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:    return 0x01;
    case SDL_SCANCODE_RETURN:    return 0x1c;
    case SDL_SCANCODE_SPACE:     return 0x39;
    case SDL_SCANCODE_LEFT:      return 0x4b;
    case SDL_SCANCODE_RIGHT:     return 0x4d;
    /* A key that is not here is dropped before it reaches the queue, which is
     * how backspace came to do nothing in the name box: the ASCII for it was
     * being worked out a few lines further on, in code this `return 0` made
     * unreachable. The digits and the dash were missing for the same reason,
     * and the game takes both in a player's name. */
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_MINUS:     return 0x0c;
    case SDL_SCANCODE_0:         return 0x0b;
    default: break;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 0x02 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        static const uint8_t az[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
            0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
            0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
        };
        return az[sc - SDL_SCANCODE_A];
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10)
        return 0x3b + (sc - SDL_SCANCODE_F1);
    return 0;
}

int32_t io_pump(void)
{
    if (io_lockstep())
        return 1;
    script_pump();
    check_deadline();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            /* Closing the window has to work from inside the opening
             * animations too, and those are twenty seconds of busy-wait with
             * no loop that checks a return value. */
            quit_requested = 1;
            io_shutdown();
            exit(0);
        case SDL_EVENT_MOUSE_MOTION: {
            float lx, ly;
            SDL_RenderCoordinatesFromWindow(ren, ev.motion.x, ev.motion.y,
                                            &lx, &ly);
            mouse_x = lx * 2.0f;         /* the game's screen is 640 wide */
            mouse_y = ly;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int32_t down = ev.type == SDL_EVENT_KEY_DOWN;
            int32_t sc = scancode_of(ev.key.scancode);
            if (!sc)
                break;
            /* Hand it to the game's own INT 09h handler rather than
             * decoding it here a second time. */
            int09_handler(down ? (uint32_t)sc : (uint32_t)sc | 0x80);
            /* ... and separately, what the BIOS would have put in its buffer
             * for the menus to read through INT 16h. Only on the make: a
             * break code never reached that buffer. */
            if (down) {
                uint32_t ascii = 0;
                if (ev.key.key >= 0x20 && ev.key.key < 0x7f)
                    ascii = (uint32_t)SDL_toupper((int32_t)ev.key.key);
                else if (ev.key.scancode == SDL_SCANCODE_RETURN)
                    ascii = 0x0d;
                else if (ev.key.scancode == SDL_SCANCODE_ESCAPE)
                    ascii = 0x1b;
                else if (ev.key.scancode == SDL_SCANCODE_BACKSPACE)
                    ascii = 0x08;
                key_push((uint32_t)sc, ascii);
            }
            break;
        }
        default:
            break;
        }
    }
    return !quit_requested;
}

void io_sound(uint32_t divisor)
{
    if (!audio || divisor == tone_divisor)
        return;
    tone_divisor = divisor;
    /* PIT channel 2 counts down from the divisor at 1.193182 MHz, and the
     * speaker sees the square wave that produces. */
    if (!divisor)
        return;
    double hz = 1193182.0 / (double)divisor;
    const int32_t rate = 22050, ms = 30;
    int32_t n = rate * ms / 1000;
    static int16_t buf[22050 / 1000 * 40];
    double period = rate / hz;
    for (int32_t i = 0; i < n && i < (int32_t)(sizeof buf / sizeof *buf); i++)
        buf[i] = (i / (period / 2.0) - (int32_t)(i / (period / 2.0)) < 0.5)
                     ? 6000 : -6000;
    SDL_PutAudioStreamData(audio, buf, (int32_t)(n * sizeof *buf));
}

/* The two CGA registers F8 cycles. The port keeps its own palette rather than
 * a register file, so these translate: 0x3d9 bits 4 and 5 pick the intensity
 * and the palette, and 0x3d8 bit 2 kills the colour burst - which on an RGB
 * monitor is what selects the cyan/red/white set the game normally runs in. */
static uint32_t cga_mode_reg = 0x0e, cga_colour_reg = 0x30;

static const uint32_t CGA16[16] = {
    0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
    0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
    0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
    0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff,
};

static void cga_palette_update(void)
{
    static const uint8_t sets[8][3] = {
        { 2, 4, 6 }, { 10, 12, 14 },        /* palette 0, dim and bright */
        { 3, 5, 7 }, { 11, 13, 15 },        /* palette 1 */
        { 3, 4, 7 }, { 11, 12, 15 },        /* burst off: cyan, red, white */
        { 3, 4, 7 }, { 11, 12, 15 },
    };
    uint32_t row = ((cga_mode_reg >> 2) & 1) * 4 +
                   ((cga_colour_reg >> 5) & 1) * 2 +
                   ((cga_colour_reg >> 4) & 1);
    uint32_t *p = (uint32_t *)g_palette;
    p[0] = CGA16[cga_colour_reg & 0x0f];
    for (int32_t i = 0; i < 3; i++)
        p[i + 1] = CGA16[sets[row][i]];
}

void io_cga_mode(uint32_t v)   { cga_mode_reg = v;   cga_palette_update(); }
void io_cga_colour(uint32_t v) { cga_colour_reg = v; cga_palette_update(); }
