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
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "game.h"

unsigned char g_vram[CGA_SIZE];

/* Mode 05h on an RGB monitor: the colour-burst-kill bit in the mode-control
 * register selects this palette whatever the palette bit says.  Entry 0 is the
 * background from the colour-select register, black at the BIOS default. */
const uint32_t g_palette[4] = {
    0xff000000,     /* black */
    0xff55ffff,     /* cyan */
    0xffff5555,     /* red */
    0xffffffff,     /* white */
};

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioStream *audio;
static uint64_t next_retrace_ns;
static unsigned tone_divisor;

#define FRAME_NS (SDL_NS_PER_SECOND / 60)

int io_init(int scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "popcorn: SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    if (scale < 1)
        scale = 1;
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

void io_present(void)
{
    uint32_t *px;
    int pitch;
    if (!SDL_LockTexture(tex, NULL, (void **)&px, &pitch))
        return;
    for (int y = 0; y < CGA_H; y++) {
        const unsigned char *row =
            g_vram + (y & 1 ? CGA_PLANE : 0) + (y >> 1) * CGA_STRIDE;
        uint32_t *out = (uint32_t *)((unsigned char *)px + (size_t)y * pitch);
        for (int x = 0; x < CGA_STRIDE; x++) {
            unsigned b = row[x];
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

void io_wait_retrace(void)
{
    uint64_t now = SDL_GetTicksNS();
    if (next_retrace_ns > now)
        SDL_DelayNS(next_retrace_ns - now);
    else
        next_retrace_ns = now;              /* behind: do not build up debt */
    next_retrace_ns += FRAME_NS;
}

/* Scan codes, so the game's own key-configuration screen keeps working: it
 * stores whatever the keyboard produced, and compares against it later. */
static int scancode_of(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_SPACE:  return 0x39;
    case SDL_SCANCODE_LEFT:   return 0x4b;
    case SDL_SCANCODE_RIGHT:  return 0x4d;
    default: break;
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        static const unsigned char az[26] = {
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

int io_pump(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            return 0;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int down = ev.type == SDL_EVENT_KEY_DOWN;
            int sc = scancode_of(ev.key.scancode);
            if (!sc)
                break;
            /* Exactly what the INT 09h handler at 1ac2:03e3 does with a scan
             * code: compare it against the three configured keys and record
             * whether it is held. */
            if (sc == g_image[KEY_SCAN_L]) g_image[KEY_LEFT] = (unsigned char)down;
            if (sc == g_image[KEY_SCAN_R]) g_image[KEY_RIGHT] = (unsigned char)down;
            if (sc == g_image[KEY_SCAN_A]) g_image[KEY_ACTION] = (unsigned char)down;
            break;
        }
        default:
            break;
        }
    }
    return 1;
}

void io_sound(unsigned divisor)
{
    if (!audio || divisor == tone_divisor)
        return;
    tone_divisor = divisor;
    /* PIT channel 2 counts down from the divisor at 1.193182 MHz, and the
     * speaker sees the square wave that produces. */
    if (!divisor)
        return;
    double hz = 1193182.0 / (double)divisor;
    const int rate = 22050, ms = 30;
    int n = rate * ms / 1000;
    static int16_t buf[22050 / 1000 * 40];
    double period = rate / hz;
    for (int i = 0; i < n && i < (int)(sizeof buf / sizeof *buf); i++)
        buf[i] = (i / (period / 2.0) - (int)(i / (period / 2.0)) < 0.5)
                     ? 6000 : -6000;
    SDL_PutAudioStreamData(audio, buf, (int)(n * sizeof *buf));
}
