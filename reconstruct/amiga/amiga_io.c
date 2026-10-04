/*
 * The platform layer, on AmigaOS 3.2 with Picasso96.
 *
 * What the original did in hardware, and what stands in for it here:
 *
 *   INT 10h AX=0005          a 320x200 P96 screen, 8 bit CLUT; g_vram is
 *                            still the 0xb8000 aperture, decoded to chunky
 *                            and handed to p96WritePixelArray on a present
 *   in al,0x3da / test al,8  a 60 Hz absolute deadline on timer.device
 *   INT 09h at port 0x60     IDCMP RAWKEY, remapped to the PC's set-1 scan
 *                            codes, fed to the game's own int09_handler
 *   INT 16h AH=00/01         the same sixteen-entry BIOS-style key queue
 *   INT 33h AX=0003          accumulated MOUSEMOVE deltas in the 640-wide
 *                            virtual screen the game reads
 *   PIT channel 2 + 0x61     a square wave synthesized into audio.device
 *
 * The display is a P96 CLUT8 mode: the game's four CGA colours become the
 * first four palette entries, so presenting is a 2bpp-to-8bit decode plus
 * one p96WritePixelArray.  If the card has no CLUT mode at 320x200x8, the
 * decode targets 16-bit RGB instead and the palette is folded into the
 * pixels; the game cannot tell the difference.
 *
 * The retrace wait and the play-loop pace are the same absolute-clock
 * scheme as sdl_io.c: the game is timed against microseconds, not against
 * a refresh count, so a PAL machine's 50 Hz display does not change the
 * speed of the game one bit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/interrupts.h>
#include <devices/timer.h>
#include <devices/audio.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <utility/tagitem.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <proto/exec.h>
#include <inline/alib.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/Picasso96.h>

#include "game.h"

/* libnix startup opens dos.library itself; these three are ours. */
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Device *TimerBase;
struct Library *P96Base;

/* A bigger stack than the 4 KB DOS default; the game is not deep, but the
 * decode-and-present path and libnix stdio like the headroom. */
long __stack = 32768;

uint8_t g_vram[CGA_SIZE];

/* The palette the decode folds through: four ARGB entries.  Identical
 * content to the SDL port's - the CGA colour registers model is copied
 * below, registers and all. */
uint32_t g_palette[4] = {
    0xff000000,     /* black */
    0xff55ffff,     /* light cyan */
    0xffff55ff,     /* light magenta */
    0xffffffff,     /* white */
};

/* The same measured play-loop rate the SDL port holds: 326 Hz, from
 * cycles.py's instruction-cost sum over a frame of the original. */
uint32_t g_play_hz = 326;

static struct Screen *scr;
static struct Window *win;

/* The chunky upload buffer and its P96 description.  For a CLUT8 screen
 * it holds one palette index per pixel; otherwise 16-bit RGB. */
static uint8_t *framebuf;
static size_t framebuf_len;
static struct RenderInfo ri;
static RGBFTYPE screen_format;

/* 60 Hz present and retrace deadlines, in microseconds since boot. */
static uint32_t presented;
static uint32_t retraces;
static uint64_t next_present_us;
static uint64_t next_retrace_us;

/* Time owed to the game's busy-wait at 0x164c; the same accumulate-then-
 * sleep scheme as the SDL port so no 8086 cycle ever spins a 68k.  An 8 MHz
 * 8086 cycle is exactly 125 ns, so the accounting stays in integers. */
static uint64_t delay_owed_ns;

/* ---------------------------------------------------------------- timer ---
 * One UNIT_MICROHZ request, used synchronously (DoIO), so a sleep never
 * outlives its caller.  GetSysTime is the clock for every deadline.
 */
static struct MsgPort *timer_port;
static struct timerequest timer_req;

static uint64_t now_us(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (uint64_t)tv.tv_secs * 1000000ull + (uint64_t)tv.tv_micro;
}

static void sleep_us(uint64_t us)
{
    timer_req.tr_node.io_Command = TR_ADDREQUEST;
    timer_req.tr_time.tv_secs = (long)(us / 1000000ull);
    timer_req.tr_time.tv_micro = (long)(us % 1000000ull);
    DoIO(&timer_req.tr_node);
}

/* ---------------------------------------------------------------- sound ---
 * The PC speaker holds a note until told otherwise: io_sound records the
 * divisor (0 = silence) and sound_top_up keeps roughly two buffers of
 * square wave queued to audio.device, refilled from the present path the
 * way the SDL port refilled its stream.
 */
#define SND_RATE   22050
#define SND_BUFS   4
#define SND_LEN    1024              /* samples per buffer, ~46 ms */
#define SND_PERIOD 162               /* 3.579545 MHz / 22050 */

/* Paula's channels 0 and 3 come out of the left socket and 1 and 2 out of
 * the right. The PC speaker is mono, so every buffer is written to a left
 * channel and a right one - on one channel alone it would be silent
 * wherever only the other socket is wired. snd_open is the request the
 * channels were allocated with and closes them; snd_req[i][k] writes
 * buffer i to channel k. */
static struct MsgPort *snd_port;
static struct IOAudio snd_open;
static struct IOAudio snd_req[SND_BUFS][2];
static int32_t snd_nch;         /* 2, or 1 if only a single channel was free */
/* audio.device plays the buffers by DMA, and DMA reads Chip RAM only, so
 * they come from AllocVec(MEMF_CHIP) at init.  A `__chip` array is not
 * enough: the compiler puts it in .bsschip, but the linked executable's
 * hunk carries no Chip flag, so on a machine with Fast RAM the loader is
 * free to put the buffers where the DMA cannot read them. */
static uint8_t (*snd_buf)[SND_LEN];
static int32_t snd_busy[SND_BUFS];     /* writes of buffer i not yet back */
/* And which ones: shutdown may only WaitIO a request still out, since one
 * already taken off the port by GetMsg is no longer in any list. */
static int8_t  snd_out[SND_BUFS][2];
static int32_t snd_inflight_n;
static int32_t audio_ok;
static uint32_t snd_next;
static uint32_t tone_divisor;
static uint32_t tone_half;      /* samples per half period */
static uint32_t tone_cnt;       /* samples left in this half */
static int8_t   tone_sign;      /* current half of the square wave */

static int32_t snd_inflight(void) { return snd_inflight_n; }

static void snd_inflight_dec(struct Message *m)
{
    int32_t i, k;
    for (i = 0; i < SND_BUFS; i++)
        for (k = 0; k < snd_nch; k++)
            if (&snd_req[i][k].ioa_Request.io_Message == m)
                goto found;
    return;
found:
    if (!snd_out[i][k])
        return;
    snd_out[i][k] = 0;
    if (--snd_busy[i] == 0)             /* the buffer is free once both are */
        snd_inflight_n--;
}

/* Every write starts with BeginIO, never SendIO or DoIO: exec's two
 * overwrite io_Flags - SendIO with 0, DoIO with IOF_QUICK - and that
 * discards ADIOF_PERVOL. The write then plays at the channel's own period
 * and volume, which on a fresh channel is a leftover period and volume 0:
 * finished too soon and silent, which is exactly what the first runs on the
 * Amiga did. audio.device's own documentation says BeginIO. */
static void snd_write(struct IOAudio *r, uint8_t *data, uint32_t len,
                      UWORD cycles)
{
    r->ioa_Request.io_Command = CMD_WRITE;
    r->ioa_Request.io_Flags = ADIOF_PERVOL;
    r->ioa_Data = data;
    r->ioa_Length = len;
    r->ioa_Period = SND_PERIOD;
    r->ioa_Volume = 64;
    r->ioa_Cycles = cycles;
}

static void sound_top_up(void)
{
    struct Message *m;

    if (!audio_ok)
        return;
    while ((m = GetMsg(snd_port)) != NULL)
        snd_inflight_dec(m);

    if (!tone_divisor) {
        tone_cnt = 0;
        return;
    }
    for (;;) {
        int32_t i, n;
        if (snd_inflight() >= 2)
            return;
        i = -1;
        for (n = 0; n < SND_BUFS; n++) {
            int32_t cand = (snd_next + n) % SND_BUFS;
            if (!snd_busy[cand]) {
                i = cand;
                break;
            }
        }
        if (i < 0)
            return;
        snd_next = (i + 1) % SND_BUFS;

        /* PIT channel 2 counts the divisor down at 1.193182 MHz, so one
         * half period lasts 22050*divisor/1193182 samples.  Rounded, and
         * kept as a countdown so the inner loop is decrement-and-compare:
         * the 68k has no business doing floating point for a square wave. */
        if (tone_cnt == 0) {
            tone_cnt = tone_half;
            tone_sign = 64;
        }
        for (n = 0; n < SND_LEN; n++) {
            if (--tone_cnt == 0) {
                tone_sign = (int8_t)-tone_sign;
                tone_cnt = tone_half;
            }
            snd_buf[i][n] = (uint8_t)tone_sign;
        }

        /* A copyback data cache on a 68040 or 68060 can still be holding
         * what was just written when the DMA reads Chip RAM. */
        CacheClearU();
        snd_busy[i] = snd_nch;
        snd_inflight_n++;
        for (n = 0; n < snd_nch; n++) {
            snd_write(&snd_req[i][n], snd_buf[i], SND_LEN, 1);
            snd_out[i][n] = 1;
            BeginIO(&snd_req[i][n].ioa_Request);
        }
    }
}

void io_sound(uint32_t divisor)
{
    if (divisor != tone_divisor) {
        /* Half period in samples: the PIT runs at 1.193182 MHz and the
         * speaker sees a half wave each divisor counts, so
         * rate*divisor / (2 * 1193182) - rounded.  (22050*divisor /
         * 1193182 alone is the FULL period, which plays everything an
         * octave low.) */
        tone_half = (22050u * divisor + 1193182u) / 2386364u;
        if (tone_half < 1)
            tone_half = 1;
        tone_cnt = 0;
    }
    tone_divisor = divisor;
}

/* ------------------------------------------------------------ the frame ---
 * Decode the interlaced 2bpp CGA aperture into the chunky buffer, then one
 * p96WritePixelArray puts it on the screen.  Palette registers live in
 * cga_palette_update below; a change is applied immediately and the CLUT
 * path only needs LoadRGB32, the chunky pixels are already indices.
 */
static uint32_t cga_mode_reg = 0x0e, cga_colour_reg = 0x30;
static int32_t cga_rgbi;
static void cga_palette_update(void);

static void decode_frame(void)
{
    int32_t y, x;
    if (screen_format == RGBFB_CLUT) {
        uint8_t *dst = framebuf;
        for (y = 0; y < CGA_H; y++) {
            const uint8_t *row =
                g_vram + (y & 1 ? CGA_PLANE : 0) + (y >> 1) * CGA_STRIDE;
            for (x = 0; x < CGA_STRIDE; x++) {
                uint32_t b = row[x];
                *dst++ = (uint8_t)(b >> 6);
                *dst++ = (uint8_t)((b >> 4) & 3);
                *dst++ = (uint8_t)((b >> 2) & 3);
                *dst++ = (uint8_t)(b & 3);
            }
        }
    } else {
        /* No CLUT mode on this card: emit 16-bit RGB built from the same
         * four palette entries. */
        uint16_t lut[4];
        uint16_t *dst = (uint16_t *)framebuf;
        for (x = 0; x < 4; x++) {
            uint32_t p = g_palette[x];
            lut[x] = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0)
                                | ((p >> 3) & 0x001f));
        }
        for (y = 0; y < CGA_H; y++) {
            const uint8_t *row =
                g_vram + (y & 1 ? CGA_PLANE : 0) + (y >> 1) * CGA_STRIDE;
            for (x = 0; x < CGA_STRIDE; x++) {
                uint32_t b = row[x];
                *dst++ = lut[(b >> 6) & 3];
                *dst++ = lut[(b >> 4) & 3];
                *dst++ = lut[(b >> 2) & 3];
                *dst++ = lut[b & 3];
            }
        }
    }
}

static void palette_apply(void)
{
    int32_t i;
    if (screen_format != RGBFB_CLUT || !scr)
        return;
    for (i = 0; i < 4; i++) {
        uint32_t p = g_palette[i];
        SetRGB32(&scr->ViewPort, (uint32_t)i,
                 (p << 8) & 0xff000000, (p << 16) & 0xff000000,
                 (p << 24) & 0xff000000);
    }
}

static void present_now(void)
{
    decode_frame();
    p96WritePixelArray(&ri, 0, 0, win->RPort, 0, 0, CGA_W, CGA_H);
}

void io_present(void)
{
    uint64_t now;
    presented++;
    sound_top_up();
    now = now_us();
    if (now < next_present_us)
        return;
    next_present_us = now + 1000000ull / 60;
    present_now();
}

/* Show what has been drawn and answer the window manager.  Called from the
 * busy-waits as well as the retrace wait, for the same reason as in the SDL
 * port: the opening sequence lives inside game-side waits, and without this
 * the window would sit blank and deaf for its first half-minute.  Shares
 * io_present's budget so the two cannot double up. */
static void keep_alive(void)
{
    uint64_t now = now_us();
    if (now < next_present_us)
        return;
    next_present_us = now + 1000000ull / 60;
    present_now();
    io_pump();
}

uint64_t io_ms(void)
{
    return now_us() / 1000ull;
}

void io_wait_retrace(void)
{
    uint64_t now;
    retraces++;
    keep_alive();
    now = now_us();
    if (next_retrace_us > now)
        sleep_us(next_retrace_us - now);
    else
        next_retrace_us = now;      /* behind: do not build up debt */
    next_retrace_us += 1000000ull / 60;
}

/* One play-loop frame, paced against an absolute clock at g_play_hz.  The
 * 326 Hz tick is a rate, not a count per refresh; holding an absolute
 * deadline is what keeps a 50 Hz PAL display from mattering.  See the long
 * note in sdl_io.c - the reasoning carries over unchanged. */
void io_frame_pace(void)
{
    static uint64_t next_tick_us;
    uint64_t period, now;

    if (g_play_hz == 0)
        return;
    keep_alive();
    now = now_us();
    period = 1000000ull / g_play_hz;
    if (next_tick_us > now)
        sleep_us(next_tick_us - now);
    else if (now - next_tick_us > 1000000ull / 4)
        next_tick_us = now;         /* a quarter second behind: give up the
                                       debt rather than sprint */
    next_tick_us += period;
}

void io_delay_cycles(uint32_t cycles)
{
    delay_owed_ns += (uint64_t)cycles * 125u;      /* an 8 MHz 8086 */
    if (delay_owed_ns >= 1000000ull) {             /* a millisecond or more */
        sleep_us(delay_owed_ns / 1000ull);
        delay_owed_ns %= 1000ull;
    }
    keep_alive();
}

uint32_t io_ticks(void)
{
    return (uint32_t)(now_us() / 1000ull * 182 / 10000);  /* 18.2 Hz */
}

/* --------------------------------------------------------------- input ---
 * Keyboard: IDCMP_RAWKEY carries Amiga raw key codes, a row/column scheme
 * of its own.  The game speaks PC set-1 scan codes, so the codes are
 * remapped here before they reach the game's own int09_handler; what the
 * BIOS buffer would have held beside the scan code - the ASCII, case and
 * shift included - is worked out here too, in key_push.
 */
static uint8_t scancode_of_rawkey(uint8_t raw)
{
    /* Amiga raw code -> PC set-1 scan code, for the keys the game knows.
     * Letters and digits land on the same relative order on both maps, so
     * only the block offsets differ. */
    if (raw >= 0x01 && raw <= 0x0a)     /* 1..0 */
        return 0x02 + (raw - 0x01);
    if (raw >= 0x10 && raw <= 0x19)     /* Q..P */
        return 0x10 + (raw - 0x10);
    if (raw >= 0x20 && raw <= 0x28)     /* A..L */
        return 0x1e + (raw - 0x20);
    if (raw >= 0x31 && raw <= 0x37)     /* Z..M */
        return 0x2c + (raw - 0x31);
    if (raw >= 0x50 && raw <= 0x59)     /* F1..F10 */
        return 0x3b + (raw - 0x50);
    switch (raw) {
    case 0x45: return 0x01;             /* Esc */
    case 0x44:                          /* Return */
    case 0x43:                          /* keypad Enter - the PC BIOS */
        return 0x1c;                    /*   reports both as 0x1c too */
    case 0x40: return 0x39;             /* Space */
    case 0x4f: return 0x4b;             /* Left */
    case 0x4e: return 0x4d;             /* Right */
    case 0x41: return 0x0e;             /* Backspace */
    case 0x0b: return 0x0c;             /* - */
    default:   return 0;                /* dropped, as in the SDL port */
    }
}

/* What shift does to a symbol on the US layout the BIOS assumes - copied
 * from the SDL port. */
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

/* The BIOS keyboard buffer the menus read through INT 16h.  Sixteen
 * entries, as the real one had, each scan<<8 | ascii. */
#define KEYQ 16
static uint32_t key_q[KEYQ];
static int32_t key_head, key_tail;
static int32_t int09_installed;

static void key_push(uint32_t scan, uint32_t ascii)
{
    int32_t next = (key_tail + 1) % KEYQ;
    if (next == key_head)
        return;                          /* full: the real BIOS beeped */
    key_q[key_tail] = scan << 8 | ascii;
    key_tail = next;
}

int32_t io_key_ready(void)
{
    return key_head != key_tail;
}

uint16_t io_get_key(void)
{
    uint32_t k;
    if (key_head == key_tail)
        return 0;
    k = key_q[key_head];
    key_head = (key_head + 1) % KEYQ;
    return (uint16_t)k;
}

void io_flush_keys(void)
{
    key_head = key_tail = 0;
}

void io_set_int09_installed(int32_t on)
{
    int09_installed = on ? 1 : 0;
    if (int09_installed)
        io_flush_keys();        /* what the BIOS buffer held is unreachable */
}

/* Shift and caps state, tracked from the raw keys themselves: the ASCII
 * the BIOS would have buffered is case as typed, which the menus' name
 * field reads byte for byte. */
static int32_t shift_held, caps_lock;

static void key_event(uint8_t raw, int32_t down)
{
    uint32_t sc, ascii;

    if (raw == 0x60 || raw == 0x61) {   /* either shift key */
        shift_held = down;
        return;                         /* the game has no use for them */
    }
    if (raw == 0x62) {                  /* caps lock toggles on the make */
        if (down)
            caps_lock = !caps_lock;
        return;
    }
    sc = scancode_of_rawkey(raw);
    if (!sc)
        return;
    if (down)
        int09_handler(sc);
    else
        int09_handler(sc | 0x80);
    if (!down || int09_installed)
        return;

    ascii = ascii_of_scan(sc);
    if (ascii >= 'A' && ascii <= 'Z') {
        /* Letters fold to lower case unless exactly one of shift and caps
         * is on - the US BIOS behaviour the SDL port mirrors. */
        int32_t upper = (shift_held != 0) != (caps_lock != 0);
        if (upper)
            ascii += 32;
    } else if (ascii && (shift_held != 0) != (caps_lock != 0)) {
        /* ...and the same fold shifts the digits and symbols, so typing
         * with caps lock on reaches the shifted key, as in the SDL port. */
        ascii = shifted_symbol(ascii);
    } else if (sc == 0x1c) {
        ascii = 0x0d;
    } else if (sc == 0x01) {
        ascii = 0x1b;
    } else if (sc == 0x0e) {
        ascii = 0x08;
    }
    /* The BIOS buffered keys that have no ASCII too - F-keys, arrows - and
     * the menus dispatch on the scan code in the high byte, so this push
     * is unconditional.  Gating it on ascii is what killed the F-keys. */
    key_push(sc, ascii);
}

/* The pointer, in the 640-wide virtual screen INT 33h reports - the game
 * does shr cx,1 on it to get a 320-pixel x.
 *
 * Movement comes from an input.device handler, not IDCMP_MOUSEMOVE: the
 * IDCMP deltas are *pointer* pixels, after Prefs/Input scaling and
 * acceleration, which is what made the paddle jumpy - slow moves vanish
 * into the acceleration knee, fast ones leap, and the whole travel is
 * squeezed into the 320-pixel screen before the pointer hits the edge and
 * stops.  The handler sees the raw quadrature counts instead, one count
 * per mickey, before any of that, which is the 1:1 feel a ball-and-paddle
 * game wants.  It snoops: it keeps every event and touches one counter.
 * If the handler cannot be installed, the IDCMP path below is the
 * fallback. */
static struct MsgPort *input_port;
static struct IOStdReq *input_io;
static struct Interrupt input_handler;
static volatile int32_t raw_dx;
static int32_t raw_mouse_ok;
static uint32_t mouse_buttons;

static struct InputEvent *__attribute__((noinline))
input_handler_code(struct InputEvent *ie __asm("a0"),
                   struct IOStdReq *req __asm("a1"))
{
    struct InputEvent *head = ie;
    (void)req;
    while (ie) {
        if (ie->ie_Class == IECLASS_RAWMOUSE) {
            /* Relative mickeys in ie_X; absolute POINTERPOS events
             * (tablets) carry no mickeys and fall through untouched. */
            raw_dx += ie->ie_X;
        }
        ie = ie->ie_NextEvent;
    }
    /* The return is the chain passed to lower-priority handlers, and
     * NULL means "consumed everything" - returning it would starve
     * Intuition of the entire input stream.  Snoop-only: hand the chain
     * back unchanged. */
    return head;
}

static int32_t setup_raw_mouse(void)
{
    input_port = CreateMsgPort();
    if (!input_port)
        return 0;
    input_io = (struct IOStdReq *)
        CreateIORequest(input_port, sizeof(*input_io));
    if (!input_io) {
        DeleteMsgPort(input_port);
        input_port = NULL;
        return 0;
    }
    if (OpenDevice("input.device", 0, (struct IORequest *)input_io, 0)
            != 0) {
        DeleteIORequest(input_io);
        DeleteMsgPort(input_port);
        input_io = NULL;
        input_port = NULL;
        return 0;
    }
    input_handler.is_Node.ln_Type = NT_INTERRUPT;
    input_handler.is_Node.ln_Pri = 100;     /* ahead of Intuition's 50 */
    input_handler.is_Node.ln_Name = "popcorn raw mouse";
    input_handler.is_Data = NULL;
    input_handler.is_Code = (void (*)())input_handler_code;
    input_io->io_Command = IND_ADDHANDLER;
    input_io->io_Data = &input_handler;
    DoIO((struct IORequest *)input_io);
    return input_io->io_Error == 0;
}

static void teardown_raw_mouse(void)
{
    if (!input_io)
        return;
    input_io->io_Command = IND_REMHANDLER;
    input_io->io_Data = &input_handler;
    DoIO((struct IORequest *)input_io);
    CloseDevice((struct IORequest *)input_io);
    DeleteIORequest(input_io);
    DeleteMsgPort(input_port);
    input_io = NULL;
    input_port = NULL;
}

static int32_t mouse_acc_x = 320;

uint32_t io_mouse_x(void)
{
    /* Fold the counts the handler gathered into the virtual position.
     * Disable around the take so no mickey is lost to a handler running
     * mid-read; io_mouse_x is called a few hundred times a second and the
     * critical section is a handful of cycles. */
    int32_t dx;
    Disable();
    dx = raw_dx;
    raw_dx = 0;
    Enable();
    mouse_acc_x += dx;
    if (mouse_acc_x < 0)
        return 0;
    if (mouse_acc_x > 639)
        return 639;
    return (uint32_t)mouse_acc_x;
}

uint32_t io_mouse_buttons(void)
{
    return mouse_buttons;
}

void io_mouse_warp(uint16_t x, uint16_t y)
{
    /* The play loop centres the pointer before the serve; a warp is what
     * the next read returns (no real-pointer equivalent on Intuition's
     * public API, and none needed: the sprite is blank).  The y half is
     * accepted for the interface's sake; the game never reads y. */
    (void)y;
    mouse_acc_x = x;
}

int32_t io_pump(void)
{
    struct IntuiMessage *m;
    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
        switch (m->Class) {
        case IDCMP_CLOSEWINDOW:
            /* Closing the window has to work from inside the opening
             * animations too, for the same reason as the SDL port's
             * SDL_EVENT_QUIT: those are twenty seconds of busy-wait with
             * no loop that checks a return value. */
            io_shutdown();
            exit(0);
        case IDCMP_RAWKEY:
            /* The up-prefix travels in the code byte itself; the raw
             * lookup wants it masked off or every break code is an
             * unknown key and the game's key-state bytes never clear. */
            key_event((uint8_t)(m->Code & ~IECODE_UP_PREFIX),
                      (m->Code & IECODE_UP_PREFIX) ? 0 : 1);
            break;
        case IDCMP_MOUSEMOVE:
            /* Fallback only: the raw handler feeds io_mouse_x when it
             * is installed, and these pointer-pixel deltas would only
             * double-count. */
            if (!raw_mouse_ok)
                mouse_acc_x += m->MouseX;
            break;
        case IDCMP_MOUSEBUTTONS:
            if (m->Code == SELECTDOWN)
                mouse_buttons |= 1;
            else if (m->Code == SELECTUP)
                mouse_buttons &= ~1;
            else if (m->Code == MENUDOWN)
                mouse_buttons |= 2;
            else if (m->Code == MENUUP)
                mouse_buttons &= ~2;
            break;
        default:
            break;
        }
        ReplyMsg((struct Message *)m);
    }
    return 1;
}

/* The pointer grab is meaningless on Intuition - there is nothing to
 * leave - but the game calls these, so the state is kept.  The pointer
 * sprite is blanked at io_init and never shown again. */
static int32_t grabbed;

void io_set_grab(int32_t on)
{
    grabbed = on ? 1 : 0;
}

int32_t io_grabbed(void)
{
    return grabbed;
}

/* ------------------------------------------------------------- the CGA ---
 * The two registers F8 cycles, carried over from the SDL port: the port
 * keeps a palette rather than a register file.  See cga_palette_update in
 * sdl_io.c for the full story of the colour-burst quirk.
 */
static const uint32_t CGA16[16] = {
    0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa,
    0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
    0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff,
    0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff,
};

void io_set_rgbi(int32_t on)
{
    cga_rgbi = on != 0;
    cga_palette_update();
}

static void cga_palette_update(void)
{
    static const uint8_t sets[4][3] = {
        { 2, 4, 6 }, { 10, 12, 14 },        /* palette 0, dim and bright */
        { 3, 5, 7 }, { 11, 13, 15 },        /* palette 1 */
    };
    /* What a real CGA substitutes when the burst is off, whatever the
     * palette bit says: cyan, red and white.  Only with --rgbi. */
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
    palette_apply();
}

void io_cga_mode(uint32_t v)   { cga_mode_reg = v;   cga_palette_update(); }
void io_cga_colour(uint32_t v) { cga_colour_reg = v; cga_palette_update(); }

/* ------------------------------------------------------------ DOS text ---
 * Everything the program has to say is CP437; the SDL port translates to
 * UTF-8 for a terminal.  Copied verbatim from there - this layer writes
 * to stderr exactly the same way.
 */
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

uint16_t io_cp437_utf8(char *out, uint16_t n, uint16_t cap, uint8_t c)
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

void io_print_dos(const char *what, const uint8_t *dos, uint16_t n)
{
    char line[1024];
    uint16_t k = 0;
    for (uint16_t i = 0; i < n; i++)
        k = io_cp437_utf8(line, k, sizeof line, dos[i]);
    line[k] = 0;
    fprintf(stderr, "popcorn: [%s] %s\n", what, line);
}

/* ------------------------------------------------------------ init/exit ---
 */
/* An empty sprite for SetPointer: two control words, one row of two
 * words, two terminating words - six in all, and in Chip RAM for the same
 * reason the sound buffers are. */
#define BLANK_POINTER_WORDS 6
static UWORD *blank_pointer;

int32_t io_init(int32_t scale)
{
    ULONG mode;
    WORD pens[] = { ~0 };

    (void)scale;    /* the screen is mode 05h's own size: always 320x200 */

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!IntuitionBase || !GfxBase) {
        fprintf(stderr, "popcorn: needs Kickstart 3.0 or newer\n");
        return 0;
    }
    P96Base = OpenLibrary("Picasso96API.library", 2);
    if (!P96Base) {
        fprintf(stderr, "popcorn: Picasso96 not found - an RTG setup "
                        "with P96 is required\n");
        return 0;
    }

    mode = p96BestModeIDTags(
        P96BIDTAG_NominalWidth, CGA_W,
        P96BIDTAG_NominalHeight, CGA_H,
        P96BIDTAG_Depth, 8,
        P96BIDTAG_FormatsAllowed, RGBFF_CLUT,
        TAG_DONE);
    if (mode != (ULONG)INVALID_ID) {
        screen_format = RGBFB_CLUT;
    } else {
        /* Cards without a CLUT8 mode still get a game: 16-bit RGB. */
        mode = p96BestModeIDTags(
            P96BIDTAG_NominalWidth, CGA_W,
            P96BIDTAG_NominalHeight, CGA_H,
            P96BIDTAG_Depth, 16,
            P96BIDTAG_FormatsAllowed, RGBFF_R5G6B5,
            TAG_DONE);
        screen_format = RGBFB_R5G6B5;
    }
    if (mode == (ULONG)INVALID_ID) {
        fprintf(stderr, "popcorn: no 320x200 RTG mode on this card\n");
        return 0;
    }

    scr = p96OpenScreenTags(
        P96SA_DisplayID, mode,
        P96SA_Width, CGA_W,
        P96SA_Height, CGA_H,
        P96SA_Depth, screen_format == RGBFB_CLUT ? 8 : 16,
        P96SA_Quiet, TRUE,
        P96SA_ShowTitle, FALSE,
        P96SA_AutoScroll, FALSE,
        P96SA_Pens, (ULONG)pens,
        P96SA_Title, (ULONG)"Popcorn",
        TAG_DONE);
    if (!scr) {
        fprintf(stderr, "popcorn: could not open the RTG screen\n");
        return 0;
    }

    framebuf_len = (size_t)CGA_W * CGA_H
                   * (screen_format == RGBFB_CLUT ? 1 : 2);
    framebuf = AllocVec(framebuf_len, MEMF_ANY | MEMF_CLEAR);
    if (!framebuf) {
        fprintf(stderr, "popcorn: out of memory\n");
        return 0;
    }
    ri.Memory = framebuf;
    ri.BytesPerRow = CGA_W * (screen_format == RGBFB_CLUT ? 1 : 2);
    ri.RGBFormat = screen_format;

    /* Raw mickeys for the paddle; IDCMP pointer deltas are the fallback
     * if the handler cannot be installed. */
    raw_mouse_ok = setup_raw_mouse();

    win = OpenWindowTags(NULL,
        WA_CustomScreen, scr,
        WA_Left, 0, WA_Top, 0,
        WA_Width, CGA_W, WA_Height, CGA_H,
        WA_Backdrop, TRUE,
        WA_Borderless, TRUE,
        WA_Activate, TRUE,
        WA_RMBTrap, TRUE,
        WA_ReportMouse, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW
                  | (raw_mouse_ok ? 0 : IDCMP_MOUSEMOVE),
        TAG_DONE);
    if (!win) {
        fprintf(stderr, "popcorn: could not open the window\n");
        return 0;
    }
    blank_pointer = AllocVec(BLANK_POINTER_WORDS * sizeof(UWORD),
                             MEMF_CHIP | MEMF_CLEAR);
    if (blank_pointer)
        SetPointer(win, blank_pointer, 1, 16, 0, 0);

    timer_port = CreateMsgPort();
    if (!timer_port) {
        fprintf(stderr, "popcorn: could not create the timer port\n");
        return 0;
    }
    timer_req.tr_node.io_Message.mn_ReplyPort = timer_port;
    if (OpenDevice("timer.device", UNIT_MICROHZ, &timer_req.tr_node, 0)
            != 0) {
        fprintf(stderr, "popcorn: could not open timer.device\n");
        return 0;
    }
    TimerBase = timer_req.tr_node.io_Device;

    /* Sound is optional: any failure leaves the game silent rather than
     * dead.  OpenDevice allocates a channel only when it is handed a map
     * of acceptable ones in ioa_Data/ioa_Length - with ioa_Length 0 it
     * opens the device and allocates nothing, and every CMD_WRITE after
     * that is refused for want of a channel. */
    snd_buf = AllocVec(sizeof(uint8_t[SND_BUFS][SND_LEN]),
                       MEMF_CHIP | MEMF_CLEAR);
    snd_port = snd_buf ? CreateMsgPort() : NULL;
    if (!snd_buf)
        fprintf(stderr, "popcorn: no Chip RAM for the sound buffers - "
                        "playing silent\n");
    if (snd_port) {
        /* A left channel and a right one, in any of the four pairings;
         * failing that, one channel alone rather than none. */
        static UBYTE channels[] = { 3, 5, 10, 12, 1, 2, 4, 8 };
        snd_open.ioa_Request.io_Message.mn_ReplyPort = snd_port;
        snd_open.ioa_Request.io_Message.mn_Node.ln_Pri = 0;
        snd_open.ioa_Request.io_Flags = ADIOF_NOWAIT;
        snd_open.ioa_AllocKey = 0;
        snd_open.ioa_Data = channels;
        snd_open.ioa_Length = sizeof channels;
        if (OpenDevice("audio.device", 0, &snd_open.ioa_Request, 0) == 0) {
            ULONG mask = (ULONG)snd_open.ioa_Request.io_Unit;
            ULONG bit[2];
            int32_t i, k;
            bit[0] = mask & -mask;          /* the lowest channel */
            bit[1] = mask & ~bit[0];        /* and the other, if there is one */
            snd_nch = bit[1] ? 2 : 1;
            /* The whole IOAudio, not just its IORequest: the key that
             * proves we own the channels is ioa_AllocKey, which lives
             * outside ioa_Request. Each write then names one channel. */
            for (i = 0; i < SND_BUFS; i++)
                for (k = 0; k < snd_nch; k++) {
                    snd_req[i][k] = snd_open;
                    snd_req[i][k].ioa_Request.io_Unit = (struct Unit *)bit[k];
                }
            audio_ok = 1;
        } else {
            fprintf(stderr, "popcorn: no audio channel free - "
                            "playing silent\n");
        }
    }

    cga_palette_update();       /* registers -> g_palette -> CLUT */
    grabbed = 1;
    next_present_us = now_us();
    next_retrace_us = now_us();
    return 1;
}

void io_shutdown(void)
{
    teardown_raw_mouse();
    if (audio_ok) {
        int32_t i, k;
        for (i = 0; i < SND_BUFS; i++) {
            for (k = 0; k < snd_nch; k++) {
                if (snd_out[i][k]) {
                    AbortIO(&snd_req[i][k].ioa_Request);
                    WaitIO(&snd_req[i][k].ioa_Request);
                    snd_out[i][k] = 0;
                }
            }
            snd_busy[i] = 0;
        }
        CloseDevice(&snd_open.ioa_Request);
        audio_ok = 0;
    }
    if (snd_port) {
        struct Message *m;
        while ((m = GetMsg(snd_port)) != NULL)
            ;
        DeleteMsgPort(snd_port);
        snd_port = NULL;
    }
    if (snd_buf) {                      /* only once no write can be in flight */
        FreeVec(snd_buf);
        snd_buf = NULL;
    }
    if (TimerBase) {
        CloseDevice(&timer_req.tr_node);
        TimerBase = NULL;
    }
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
    if (win) {
        struct Message *m;
        Forbid();
        while ((m = GetMsg(win->UserPort)) != NULL)
            ReplyMsg(m);
        Permit();
        CloseWindow(win);
        win = NULL;
    }
    if (blank_pointer) {                /* the window that showed it is gone */
        FreeVec(blank_pointer);
        blank_pointer = NULL;
    }
    if (scr) {
        p96CloseScreen(scr);
        scr = NULL;
    }
    if (framebuf) {
        FreeVec(framebuf);
        framebuf = NULL;
    }
    if (P96Base) {
        CloseLibrary(P96Base);
        P96Base = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}
