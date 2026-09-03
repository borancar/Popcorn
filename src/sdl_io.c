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

/* Palette 1 at high intensity, which is what BIOS leaves in the colour-select
 * register (0x30) for mode 05h.  Entry 0 is the background from that same
 * register, black at the default.  See cga_palette_update for why the
 * colour-burst bit does not turn this into the cyan/red/white set. */
uint32_t g_palette[4] = {
    0xff000000,     /* black */
    0xff55ffff,     /* light cyan */
    0xffff55ff,     /* light magenta */
    0xffffffff,     /* white */
};

static SDL_Window *win;
static int32_t grabbed;
static uint32_t presented;      /* so a speed change can be measured */
static uint32_t retraces;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioStream *audio;
static void sound_top_up(void);
static void cga_palette_update(void);
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
    /* Until this runs, g_palette is only its static initialiser - which was a
     * second answer to the same question, and disagreed with the registers as
     * soon as POPCORN_RGBI was set.  Compute it from them once, here. */
    cga_palette_update();
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

    /* The paddle is driven by an **absolute** pointer - the game's own mouse
     * routine at 1ac2:1654 is `paddle = clamp(mouse x / 2)` - so the pointer
     * leaving the window means the paddle stops at the edge while the player
     * is still moving. Confining it to the window is what makes that input
     * usable at all. Ctrl+Alt lets go, the way a DOS box does; clicking back
     * in takes it again.
     */
    io_set_grab(1);

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
    presented++;
    sound_top_up();
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

/* One play-loop frame, paced against the **CGA refresh** rather than against
 * a busy-wait.
 *
 * The original spends `mov cx,N / loop $` here with N from POPSPEED, which is
 * how it was made to run at the same speed on a faster PC. Emulating that
 * means sleeping for what the loop would have cost, and a sleep of a quarter
 * of a millisecond is at the mercy of the scheduler's granularity - so the
 * game's speed became a property of the host rather than of the game.
 *
 * The rate to hold it at is **measured**, by `cycles.py`: run the original
 * under the emulator, hook every instruction, and sum the iAPX 86/88 manual's
 * cycle costs over a play-loop frame. That comes to about 24,500 cycles, or
 * 326 Hz at the 8 MHz the readme names - and it lands within two Hz whether
 * the paddle is moving or still, and on a different level:
 *
 *     level 10, bot moving the paddle    24541 cy   326.0 Hz
 *     level 10, paddle still             24537 cy   326.0 Hz
 *     level 1                            24419 cy   327.6 Hz
 *
 * It is worth saying why that is trustworthy rather than another guess:
 * three-quarters of the frame is the `loop $` busy-waits, which are exactly
 * 17 cycles a turn, so only the remaining quarter carries any modelling at
 * all. What the manual's table leaves out is instruction fetch - a real 8086
 * empties its prefetch queue on every branch - so the true machine is
 * somewhat slower than this and 326 is an upper bound. `cycles.py --stall`
 * says by how much for a given assumption.
 *
 * 326 Hz is 5.43 refreshes, so this is a rate rather than a count of ticks
 * per refresh. The tick runs on an absolute clock and cannot drift, which is
 * the whole complaint against the busy-wait it replaces: a sleep of a quarter
 * of a millisecond was at the mercy of the scheduler, so the game's speed had
 * become a property of the host. The screen is still presented on the
 * refresh, by io_present.
 *
 * `popcorn-dev --play-hz N` sets it, for hearing what a different assumption
 * about the stall sounds like. */
uint32_t g_play_hz = 326;

void io_frame_pace(void)
{
    static uint64_t next_tick_ns;

    if (io_lockstep() || g_play_hz == 0)
        return;
    keep_alive();
    uint64_t now = SDL_GetTicksNS();
    uint64_t period = SDL_NS_PER_SECOND / g_play_hz;
    if (next_tick_ns > now) {
        SDL_DelayNS(next_tick_ns - now);
    } else if (now - next_tick_ns > SDL_NS_PER_SECOND / 4) {
        next_tick_ns = now;                 /* a quarter second behind: the
                                             * window was dragged or the game
                                             * did something long. Give up the
                                             * debt rather than sprint. */
    }
    /* Anything smaller than that is the sleep overshooting - tens of
     * microseconds against a three-millisecond period - and is made up by the
     * next tick sleeping less, not thrown away. Resetting to `now` every time
     * cost 4% of the rate. */
    next_tick_ns += period;
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

/* The pointer the emulator had, pinned for a verification run.
 *
 * game_input reads the mouse, so any routine that calls it - and the
 * between-level screens all do, through input_and_draw_paddle - puts the
 * paddle wherever *this* process's pointer happens to be. That is not the
 * emulator's, so the paddle lands somewhere else and the routine is reported
 * as differing for a reason that is not the transcription. The same argument
 * as the BIOS tick count, one layer up.
 */
static int32_t mouse_pinned;
static uint32_t pinned_x, pinned_btn;

void io_pin_mouse(uint32_t x, uint32_t buttons)
{
    mouse_pinned = 1;
    pinned_x = x;
    pinned_btn = buttons;
}


void io_mouse_warp(uint16_t x, uint16_t y)
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

static void autoplay_tick(void)
{
    static uint32_t last = 0xffffffffu;
    /* Never under lockstep. The pinned mouse is read *before* the lockstep
     * one, so a run with both would have the port playing itself while the
     * driver believed it was in control - and the comparison would be two
     * different games rather than two views of one. */
    if (io_lockstep())
        return;
    if (!autoplay_on() || last == presented)
        return;
    last = presented;
    autoplay_step();
}

uint32_t io_mouse_x(void)
{
    autoplay_tick();
    if (mouse_pinned)
        return pinned_x;
    if (io_lockstep())
        return io_lockstep_mouse_x();
    return (uint32_t)(mouse_x < 0 ? 0 : mouse_x > 639 ? 639 : mouse_x);
}
uint32_t io_mouse_buttons(void)
{
    autoplay_tick();
    if (mouse_pinned)
        return pinned_btn;
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

/* The key the emulator had waiting, pinned for a verification run.
 *
 * A routine that reads the BIOS buffer - input_demo hands whatever it finds
 * to the cheat matcher - takes a different branch when this process's queue
 * is empty and the emulator's was not. Same argument as the pointer and the
 * tick count: the harness has to hand over what the original saw, or it
 * reports a difference in the input as a difference in the transcription.
 */
static int32_t key_pinned;
static uint32_t pinned_key;

void io_pin_key(uint32_t k)
{
    key_pinned = 1;
    pinned_key = k;
}

int32_t io_key_ready(void)
{
    if (key_pinned)
        return pinned_key != 0;
    if (io_lockstep())
        return 0;
    return key_head != key_tail;
}

uint16_t io_get_key(void)
{
    if (key_pinned) {
        uint32_t k = pinned_key;
        pinned_key = 0;                 /* the buffer had one, not a stream */
        return k;
    }
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
static struct { uint32_t scan, ms; int32_t done, shift; } script[SCRIPT_MAX];
static int32_t script_n;

void io_script_key_shift(uint32_t scan, uint32_t ms, int32_t shift)
{
    if (script_n < SCRIPT_MAX) {
        script[script_n].scan = scan;
        script[script_n].ms = ms;
        script[script_n].shift = shift;
        script[script_n].done = 0;
        script_n++;
    }
}

/* What the BIOS would have put in its buffer beside the scan code. Real key
 * events get this from SDL; a scripted one had nothing, so --keys could press
 * F1 but could not type a player's name - name_field reads the ASCII byte and
 * saw zero every time. Set 1 scan codes, the printable half of the keyboard. */
/* What shift does to a symbol on the US layout the BIOS assumes.
 *
 * Letters fold to upper case and everything else was left alone, which made
 * `_` impossible to type: it is shift and the minus key, and both paths
 * handed the game a plain `-`. That is not cosmetic - the demo's cheat is
 * "pop_corn LACRAL", and a key that is not the next one in the sequence ends
 * the demo, so the fourth character sent you back to the menu every time. */
static uint32_t shifted_symbol(uint32_t a)
{
    switch (a) {
    case '1': return '!';   case '2': return '@';   case '3': return '#';
    case '4': return '$';   case '5': return '%';   case '6': return '^';
    case '7': return '&';   case '8': return '*';   case '9': return '(';
    case '0': return ')';   case '-': return '_';   case '=': return '+';
    case '[': return '{';   case ']': return '}';   case ';': return ':';
    case '\'': return '"'; case '`': return '~';   case '\\': return '|';
    case ',': return '<';   case '.': return '>';   case '/': return '?';
    default:  return a;
    }
}

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
        /* An unmodified scan code is an unshifted key, so a letter is
         * lower-case - the same as the live path above. `--keys ^1e@...`
         * presses it with shift, which is how a mixed-case string like the
         * cheat gets typed by a script. */
        uint32_t a = ascii_of_scan(sc);
        if (script[i].shift)
            a = shifted_symbol(a);
        else if (a >= 'A' && a <= 'Z')
            a += 32;
        key_push(sc, a);
        if (sc == global.key_scan_l) global.key_left = 1;
        if (sc == global.key_scan_r) global.key_right = 1;
        if (sc == global.key_scan_a) global.key_action = 1;
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
static const char *image_path;

void io_set_deadline(uint32_t ms, const char *shot, const char *vram)
{
    deadline_ns = ms ? SDL_GetTicksNS() + (uint64_t)ms * SDL_NS_PER_MS : 0;
    shot_path = shot;
    vram_path = vram;
}

/* The load image as the run leaves it. "Did typing this set that byte" is not
 * a question the image at startup can answer, and the deadline exits the
 * process, so it has to be written from here. */
void io_set_deadline_image(const char *path)
{
    image_path = path;
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
    if (image_path) {
        FILE *f = fopen(image_path, "wb");
        if (f) {
            fwrite(g_image, 1, IMAGE_LEN, f);
            fclose(f);
        }
    }
    /* How many frames the run actually got through, so a change to the speed
     * setting can be measured rather than felt. [0x148b] counts down once per
     * play-loop frame. */
    /* What the run actually got through, so a speed setting can be measured
     * rather than felt. The delay POPSPEED sets is called **once** per
     * play-loop frame (1ac2:1c3c) but 0x96 times per frame of an animation,
     * so it weighs on the two completely differently - which is worth being
     * able to show rather than argue about. */
    fprintf(stderr, "popcorn: %u frames presented, %u retrace waits\n",
            presented, retraces);
    io_shutdown();
    exit(0);
}

void io_wait_retrace(void)
{
    if (io_lockstep())
        return;
    retraces++;
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

void io_set_grab(int32_t on)
{
    if (!win)
        return;
    grabbed = on ? 1 : 0;
    SDL_SetWindowMouseGrab(win, grabbed ? true : false);
    if (grabbed)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
}

int32_t io_grabbed(void) { return grabbed; }

/* Whether the game's own INT 09h handler is in place. While it is, scan codes
 * still reach int09_handler but stop reaching the BIOS key buffer, which is
 * what the real handler's failure to chain amounts to. */
static int32_t int09_installed;

void io_set_int09_installed(int32_t on)
{
    int09_installed = on ? 1 : 0;
    if (int09_installed)
        io_flush_keys();        /* what the BIOS buffer held is unreachable */
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
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            /* Clicking in the window takes the pointer back. The click still
             * goes through to the game as the action button, which is what a
             * player pressing it expects. */
            if (!grabbed)
                io_set_grab(1);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int32_t down = ev.type == SDL_EVENT_KEY_DOWN;
            /* Ctrl+Alt lets the pointer go, and is not passed on: it is the
             * one chord the game has no use for. */
            if (down && (ev.key.mod & SDL_KMOD_CTRL)
                     && (ev.key.mod & SDL_KMOD_ALT)) {
                io_set_grab(0);
                break;
            }
            int32_t sc = scancode_of(ev.key.scancode);
            if (!sc)
                break;
            /* Hand it to the game's own INT 09h handler rather than
             * decoding it here a second time. */
            int09_handler(down ? (uint32_t)sc : (uint32_t)sc | 0x80);
            /* ... and separately, what the BIOS would have put in its buffer
             * for the menus to read through INT 16h. Only on the make: a
             * break code never reached that buffer. */
            /* The BIOS buffer, but only while the BIOS owns the keyboard.
             * The game's own INT 09h handler does not chain, so with it
             * installed INT 16h has nothing to read - see install_int09. */
            if (down && !int09_installed) {
                uint32_t ascii = 0;
                if (ev.key.key >= 0x20 && ev.key.key < 0x7f) {
                    /* The ASCII the BIOS would have buffered - which means
                     * **case as typed**, not folded.
                     *
                     * This upper-cased everything, and that silently broke the
                     * cheat: the string at 0x3f0b is `LACRAL software`, caps
                     * then lower, and cheat_match compares byte for byte, so
                     * the match died on the `s` and no amount of typing it
                     * correctly would do anything. Nothing else noticed,
                     * because the menu dispatches on the scan code in the high
                     * byte and the high-score name is upper-case anyway. */
                    ascii = (uint32_t)ev.key.key;
                    int32_t caps = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                    if (ev.key.mod & SDL_KMOD_CAPS)
                        caps = !caps;   /* shift and caps lock cancel out */
                    if (caps && ascii >= 'a' && ascii <= 'z')
                        ascii = (uint32_t)SDL_toupper((int32_t)ascii);
                    else if (caps)
                        ascii = shifted_symbol(ascii);
                } else if (ev.key.scancode == SDL_SCANCODE_RETURN)
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

/* ------------------------------------------------------------------------
 * DOS text.
 *
 * Everything the program has to say is **CP437**, the character set the
 * machine drew it in: 0x82 is an e-acute, 0x85 an a-grave. The port has no
 * text mode to draw it in and prints it instead, and printing those bytes raw
 * drops every accent on a terminal that reads UTF-8 - which turned the
 * authors' own statement into a page with holes in it.
 *
 * This is the upper half. The lower half is ASCII and needs nothing.
 * ------------------------------------------------------------------------ */
static const uint16_t cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
    0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
    0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
    0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

uint32_t io_cp437_utf8(char *out, uint32_t n, uint32_t cap, uint8_t c)
{
    uint32_t u = c < 0x80 ? c : cp437_high[c - 0x80];
    if (u < 0x80) {
        if (n + 1 < cap)
            out[n++] = (char)u;
    } else if (u < 0x800) {
        if (n + 2 < cap) {
            out[n++] = (char)(0xc0 | (u >> 6));
            out[n++] = (char)(0x80 | (u & 0x3f));
        }
    } else if (n + 3 < cap) {
        out[n++] = (char)(0xe0 | (u >> 12));
        out[n++] = (char)(0x80 | ((u >> 6) & 0x3f));
        out[n++] = (char)(0x80 | (u & 0x3f));
    }
    return n;
}

void io_print_dos(const char *what, const uint8_t *dos, uint32_t n)
{
    char line[1024];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        k = io_cp437_utf8(line, k, sizeof line, dos[i]);
    line[k] = 0;
    fprintf(stderr, "popcorn: [%s] %s\n", what, line);
}

/* The PC speaker holds a note until it is told otherwise, and so must this.
 *
 * sound_tick only calls io_sound when the **note changes** - it returns early
 * at 1ac2:00a5 while the current one is still being held - so queueing a
 * fixed buffer here played the first thirty milliseconds of every note and
 * then silence. A note of ten ticks is about a sixth of a second.
 *
 * So io_sound only records the tone, and io_present tops the stream up every
 * frame while it lasts. tone_phase carries across the top-ups: restarting the
 * waveform at zero each time puts a click at every frame boundary.
 */
#define SND_RATE   22050
static uint32_t tone_phase;

void io_sound(uint32_t divisor)
{
    if (divisor != tone_divisor)
        tone_phase = 0;
    tone_divisor = divisor;
}

/* Keep about 60ms queued, so a late frame does not leave a gap. */
static void sound_top_up(void)
{
    if (!audio)
        return;
    if (!tone_divisor) {
        tone_phase = 0;
        return;
    }
    int32_t queued = SDL_GetAudioStreamQueued(audio);
    if (queued < 0)
        queued = 0;
    int32_t want = SND_RATE * 60 / 1000 * (int32_t)sizeof(int16_t);
    if (queued >= want)
        return;

    /* PIT channel 2 counts down from the divisor at 1.193182 MHz, and the
     * speaker sees the square wave that produces. */
    double period = (double)SND_RATE / (1193182.0 / (double)tone_divisor);
    if (period < 2.0)
        period = 2.0;
    int32_t n = (want - queued) / (int32_t)sizeof(int16_t);
    static int16_t buf[SND_RATE / 1000 * 80];
    if (n > (int32_t)(sizeof buf / sizeof *buf))
        n = (int32_t)(sizeof buf / sizeof *buf);
    for (int32_t i = 0; i < n; i++) {
        double t = (double)(tone_phase + (uint32_t)i) / (period / 2.0);
        buf[i] = (t - (double)(int64_t)t < 0.5) ? 6000 : -6000;
    }
    tone_phase += (uint32_t)n;
    SDL_PutAudioStreamData(audio, buf, (int32_t)(n * sizeof *buf));
}

/* The two CGA registers F8 cycles. The port keeps its own palette rather than
 * a register file, so these translate: 0x3d9 bits 4 and 5 pick the intensity
 * and the palette.  0x3d8 bit 2 kills the colour burst, and deliberately does
 * nothing here - see cga_palette_update. */
static uint32_t cga_mode_reg = 0x0e, cga_colour_reg = 0x30;

static const uint32_t CGA16[16] = {
    0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
    0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
    0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
    0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff,
};

/* A real CGA's own output, which is not the default - see below.  Both
 * binaries take --rgbi for it; there is no environment variable, because a
 * thing that changes what the game looks like belongs where a player can see
 * it. */
static int32_t cga_rgbi;

void io_set_rgbi(int32_t on)
{
    cga_rgbi = on != 0;
    cga_palette_update();
}

/* The CGA's four palettes, and the colour-burst bit does **not** choose
 * between them.
 *
 * On a real CGA, clearing that bit in 320x200 - which is what BIOS mode 05h
 * does, and this game asks for mode 5 at 1ac2:01a1 - forces cyan, red and
 * white on an RGBI monitor whatever the palette bit says.  A card that only
 * emulates the mode does not carry the quirk over, and by 1988 that is what
 * the game was being played on: EGA and VGA boards driving a monitor through
 * the newer connector.  There the palette bit governs, and with BIOS leaving
 * 0x30 in the colour-select register the game comes up in palette 1 at high
 * intensity - light cyan, light magenta, white.  DOSBox shows the same, and
 * it is the picture people remember.
 *
 * **This is a deliberate departure from emulation.py**, which models the CGA
 * itself and renders the cyan/red/white set.  sidebyside cannot see the
 * difference - it compares video memory and the image, and the palette is in
 * neither - so it is written down here instead. */
static void cga_palette_update(void)
{
    static const uint8_t sets[4][3] = {
        { 2, 4, 6 }, { 10, 12, 14 },        /* palette 0, dim and bright */
        { 3, 5, 7 }, { 11, 13, 15 },        /* palette 1 */
    };
    /* What a real CGA substitutes when the burst is off, whatever the palette
     * bit says: cyan, red and white.  Only with POPCORN_RGBI. */
    static const uint8_t rgbi[2][3] = { { 3, 4, 7 }, { 11, 12, 15 } };
    uint32_t bright = (cga_colour_reg >> 4) & 1;
    const uint8_t *fg =
        (cga_rgbi && ((cga_mode_reg >> 2) & 1))
            ? rgbi[bright]
            : sets[((cga_colour_reg >> 5) & 1) * 2 + bright];
    uint32_t *p = (uint32_t *)g_palette;
    p[0] = CGA16[cga_colour_reg & 0x0f];
    for (int32_t i = 0; i < 3; i++)
        p[i + 1] = CGA16[fg[i]];
}

void io_cga_mode(uint32_t v)   { cga_mode_reg = v;   cga_palette_update(); }
void io_cga_colour(uint32_t v) { cga_colour_reg = v; cga_palette_update(); }
