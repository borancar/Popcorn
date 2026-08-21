#!/usr/bin/env python3
"""
Run Popcorn in the emulator with a real SDL window: CGA output, keyboard, mouse.

Extends the DOS shim in trace_dos.py with:
  * a CGA model - mode 04h/05h/06h, the mode-control and colour-select
    registers (0x3d8/0x3d9), and the interlaced 0xb8000 framebuffer decoded to
    a linear surface.  A VGA model is kept alongside it, unused by this game
    but the reason the file is worth having as a starting point
  * a hardware keyboard - Popcorn installs its own INT 09h and reads scan
    codes from port 0x60, so keys are delivered as IRQ 1 while that handler is
    installed and through the BIOS INT 16h buffer while it is not
  * wall-clock timing - the PIT counter and the 0x3da vertical-retrace bit are
    derived from real elapsed time, so the game paces itself correctly instead
    of spinning
  * live input - host keyboard fed through the BIOS INT 16h buffer, host mouse
    through INT 33h

The host filesystem stays READ-ONLY, exactly as in trace_dos.py: the game's
writes to settings.dat and save files are intercepted in memory. Nothing in the
game directory is modified.

Usage:
    python emulation.py                        # interactive window
    python emulation.py --scale 3
    python emulation.py --shots 6 --shot-every 2.0   # save PNGs and exit
    python emulation.py --blaster              # advertise a Sound Blaster
"""
import argparse
import os
import struct
import sys
import time
from collections import Counter, deque

import pygame
from unicorn import *
from unicorn.x86_const import *
from trace_dos import DosMachine, UNPACKED
from sb import SoundBlaster
from xms import XMS

# Where the XMS entry-point stub lives: low memory, above the BIOS data area
# and below the PSP, so it collides with nothing the program uses.
XMS_STUB_SEG = 0x0090
XMS_INT = 0x60              # spare vector the stub traps through

PIT_HZ = 1193182.0

# pygame key -> (BIOS scancode, ASCII)
KEYMAP = {
    pygame.K_ESCAPE: (0x01, 0x1B), pygame.K_RETURN: (0x1C, 0x0D),
    pygame.K_SPACE: (0x39, 0x20), pygame.K_BACKSPACE: (0x0E, 0x08),
    pygame.K_TAB: (0x0F, 0x09),
    pygame.K_UP: (0x48, 0x00), pygame.K_DOWN: (0x50, 0x00),
    pygame.K_LEFT: (0x4B, 0x00), pygame.K_RIGHT: (0x4D, 0x00),
    pygame.K_HOME: (0x47, 0x00), pygame.K_END: (0x4F, 0x00),
    pygame.K_PAGEUP: (0x49, 0x00), pygame.K_PAGEDOWN: (0x51, 0x00),
    pygame.K_LEFTBRACKET: (0x1A, 0x5B), pygame.K_RIGHTBRACKET: (0x1B, 0x5D),
    pygame.K_COMMA: (0x33, 0x2C), pygame.K_PERIOD: (0x34, 0x2E),
    pygame.K_MINUS: (0x0C, 0x2D), pygame.K_EQUALS: (0x0D, 0x3D),
    pygame.K_SEMICOLON: (0x27, 0x3B), pygame.K_SLASH: (0x35, 0x2F),
}
for i, k in enumerate("qwertyuiop"):
    KEYMAP[getattr(pygame, f"K_{k}")] = (0x10 + i, ord(k))
for i, k in enumerate("asdfghjkl"):
    KEYMAP[getattr(pygame, f"K_{k}")] = (0x1E + i, ord(k))
for i, k in enumerate("zxcvbnm"):
    KEYMAP[getattr(pygame, f"K_{k}")] = (0x2C + i, ord(k))
for i in range(1, 10):
    KEYMAP[getattr(pygame, f"K_{i}")] = (0x02 + i - 1, ord(str(i)))
KEYMAP[pygame.K_0] = (0x0B, ord("0"))
# F1 is scan code 0x3b, not 0x3a - 0x3a is Caps Lock. Popcorn's whole menu is
# function keys, so an off-by-one here makes every one of them do nothing.
for i in range(1, 11):
    KEYMAP[getattr(pygame, f"K_F{i}")] = (0x3A + i, 0x00)

def shift_ascii(mapped, text):
    """(scancode, ascii) with the ASCII replaced by what was actually typed.

    KEYMAP holds one ASCII per key and it is the unshifted one, so on its own the
    machine can only ever type lowercase. `text` is pygame's ev.unicode - the
    character the layout produced, with shift and caps lock already applied - and
    it wins whenever it is a single printable ASCII character. Anything else
    (dead keys, an empty string for the arrows, a non-ASCII layout) leaves the
    table's value alone rather than pushing something the guest cannot represent.
    """
    sc, asc = mapped
    if text and len(text) == 1 and 0x20 <= ord(text) < 0x7F:
        return (sc, ord(text))
    return mapped


TRACE_TEXT = False          # set by --text-trace
WATCH_DGROUP = []           # set by --watch-dgroup

MODE_GEOM = {0x13: (320, 200), 0x00: (320, 200), 0x01: (320, 200),
             0x04: (320, 200), 0x05: (320, 200), 0x06: (640, 200),
             0x0D: (320, 200),
             0x0E: (640, 200), 0x10: (640, 350), 0x12: (640, 480)}

VGA_A000 = 0xA0000
VGA_B800 = 0xB8000
CGA_MODES = (0x04, 0x05, 0x06)

# The sixteen colours a CGA can put on an RGB monitor.  Index order is the
# usual IRGB.
CGA16 = [
    (0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
    (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
    (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
    (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255),
]

# The four-colour palettes of 320x200 graphics, as attribute indices into
# CGA16 above.  Entry 0 is the background, which the colour-select register
# names separately; the three foreground entries are what the palette bits
# choose.  Keyed by (palette bit 5, intensity bit 4, mode-control bw bit 2).
#
# Mode 05h sets the bw bit, and on an RGB monitor that gives the third,
# often-forgotten palette - cyan / red / white - regardless of the palette
# bit.  Popcorn runs in mode 05h, so this is the row that matters; F8 in the
# menu cycles the colour-select register through the others.
CGA4 = {
    (0, 0, 0): (2, 4, 6),      (0, 1, 0): (10, 12, 14),
    (1, 0, 0): (3, 5, 7),      (1, 1, 0): (11, 13, 15),
    (0, 0, 1): (3, 4, 7),      (0, 1, 1): (11, 12, 15),
    (1, 0, 1): (3, 4, 7),      (1, 1, 1): (11, 12, 15),
}

# What each port is, so a port report reads as hardware rather than as numbers.
# Here rather than in a reporting script because _on_in/_on_out below are the
# authority on which of these the machine actually models, and native.py's
# port_report and trace_ports.py both read this one copy.
PORTS = {
    0x40: "PIT ch0 counter", 0x41: "PIT ch1", 0x42: "PIT ch2",
    0x43: "PIT mode/command",
    0x60: "keyboard data", 0x61: "keyboard control",
    0x201: "joystick",
    0x3C0: "attribute controller", 0x3C2: "misc output",
    0x3C4: "sequencer index", 0x3C5: "sequencer data (map mask at index 2)",
    0x3C6: "DAC pel mask", 0x3C7: "DAC read index",
    0x3C8: "DAC write index", 0x3C9: "DAC data",
    0x3CE: "graphics ctlr index", 0x3CF: "graphics ctlr data",
    0x3D4: "CRTC index", 0x3D5: "CRTC data",
    0x3D8: "CGA mode control", 0x3D9: "CGA colour select",
    0x3DA: "input status 1 (bit 0 display enable, bit 3 vertical retrace)",
    0x220: "SB DSP reset", 0x22C: "SB DSP write", 0x22A: "SB DSP read",
    0x22E: "SB DSP read-buffer status", 0x226: "SB reset",
    0x00A: "DMA mask", 0x00B: "DMA mode", 0x00C: "DMA flip-flop clear",
    0x002: "DMA ch1 address", 0x003: "DMA ch1 count", 0x083: "DMA ch1 page",
    0x020: "PIC 1 command (0x20 = end of interrupt)", 0x021: "PIC 1 mask",
    0x0A0: "PIC 2 command", 0x0A1: "PIC 2 mask",
}


class VgaDos(DosMachine):
    def __init__(self, exe, blaster=False, **kw):
        self.palette = [(0, 0, 0)] * 256
        self.dac_index = 0
        self.dac_phase = 0
        self.dac_latch = []
        self.seq_index = 0
        self.chain4 = True
        self.map_mask = 0x0F
        self.active_planes = (0, 1, 2, 3)
        self.planes = [bytearray(0x10000) for _ in range(4)]
        self.crtc = {}
        self.crtc_index = 0
        self.start_addr = 0
        self.crtc_offset = 0
        self.start_mult = 4
        self._warned_range = False
        self.mode = 0x03
        self.width, self.height = 320, 200
        self.key_buf = deque()
        self.pending_scan = None   # second half of a DOS extended-key read
        self.last_scancode = 0
        self.mouse_pos = (160, 100)
        self.mouse_btn = 0
        self.mouse_rel = [0.0, 0.0]
        self.mouse_sens = 1.0
        # Indexed the way INT 33h numbers buttons: 0=left, 1=right, 2=middle.
        self.press_count = [0, 0, 0]
        self.release_count = [0, 0, 0]
        self.press_pos = [(160, 100)] * 3
        self.release_pos = [(160, 100)] * 3
        # CGA: the two write-only registers, at the values the BIOS leaves
        # after a mode set.  0x3d8 bit 3 is video-enable, bit 2 the
        # colour-burst kill; 0x3d9 bit 4 is intensity, bit 5 the palette.
        # What INT 09h points at before the program touches it. Anything else
        # there means the program's own handler is live; see
        # guest_owns_keyboard().
        self.boot_int09 = (0, 0)
        self.cga_mode_ctrl = 0x0A
        self.cga_colour = 0x30
        # Scan codes waiting to reach the guest, as (code, ascii) with the
        # code already carrying bit 7 for a break.
        self.scan_queue = deque()
        self.pit_latch_toggle = {}
        self.pit_initial = 0xFFFF
        self.t0 = time.perf_counter()
        self.palette_writes = 0
        self.int10_fn = Counter()
        self.text_mode = True             # DOS hands us mode 03h
        self.cursor = [(0, 0)] * 8
        self.active_page = 0
        self._trun = None
        self.sb = SoundBlaster(base=0x220, irq=5, dma=1,
                              log=print, verbose=True) if blaster else None
        self.sb_last_tick = None
        self.sb_irqs = 0
        self._dma_hook = None
        self.xms = XMS(log=print, verbose=True)
        self.vidwrites = Counter()
        self.vidrange = {}
        super().__init__(exe, blaster=blaster, verbose=False, **kw)
        # Watch the video apertures so we can tell where the game actually
        # draws: 0xa0000 (graphics) vs 0xb8000 (colour text) vs 0xb0000 (mono).
        # Handle kept so a subclass can drop this one: it is diagnostics only,
        # but it fires on every single write to video memory.
        self._vidwrite_hook = self.uc.hook_add(
            UC_HOOK_MEM_WRITE, self._on_vidwrite, None, 0xA0000, 0xBFFFF)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_plane_write,
                         None, 0xA0000, 0xAFFFF)
        # The guest reaches XMS by far-calling this stub: INT 60h services the
        # request, then RETF returns to the caller. Written after the machine
        # exists, since it lives in emulated memory.
        self.uc.mem_write(XMS_STUB_SEG * 16, bytes([0xCD, XMS_INT, 0xCB]))
        self.boot_int09 = self._ivt(0x09)
        if TRACE_TEXT:
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_textwrite,
                             None, 0xB8000, 0xB8FA0)
        for off in WATCH_DGROUP:
            # DGROUP sits at image offset 0x18950; the image loads at
            # load_seg<<4. Watching a variable there shows exactly when and
            # from where the game changes it.
            lin = self.load_seg * 16 + 0x18950 + off
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self._make_watch(off),
                             None, lin, lin + 1)
            print(f"  [watch] DGROUP {off:#06x} -> linear {lin:#07x}")

    def _make_watch(self, off):
        def on_write(uc, access, address, size, value, user):
            print(f"  [watch] DGROUP {off:#06x} = {value:#06x} "
                  f"(size {size}) written from "
                  f"{uc.reg_read(UC_X86_REG_CS):04x}:"
                  f"{uc.reg_read(UC_X86_REG_IP):04x} "
                  f"at t={self._elapsed():.1f}s")
        return on_write

    def _flush_text_run(self):
        """Emit the pending run of characters poked straight into 0xb8000."""
        if not self._trun:
            return
        start, chars = self._trun
        row, col = divmod(start, 80)
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chars)
        print(f"  [txt] wrote {len(chars):>3} chars at row {row} col {col}: "
              f"{text[:72]!r}")
        self._trun = None

    def _on_textwrite(self, uc, access, address, size, value, user):
        """Coalesce direct text-buffer writes into runs, for --text-trace."""
        off = address - 0xB8000
        if off < 0 or off >= 80 * 25 * 2 or off & 1:
            return                              # attribute byte, or off-screen
        cell = off // 2
        ch = value & 0xFF
        if self._trun and self._trun[0] + len(self._trun[1]) == cell:
            self._trun[1].append(ch)
        else:
            self._flush_text_run()
            self._trun = (cell, [ch])

    def _on_vidwrite(self, uc, access, address, size, value, user):
        if address >= 0xB8000:
            k = "b800(text)"
        elif address >= 0xB0000:
            k = "b000(mono)"
        else:
            k = "a000(gfx)"
        self.vidwrites[k] += size
        lo, hi = self.vidrange.get(k, (1 << 30, 0))
        self.vidrange[k] = (min(lo, address), max(hi, address + size))

    # ------------------------------------------------------------ timing
    def _elapsed(self):
        return time.perf_counter() - self.t0

    # ------------------------------------------------------------- ports
    def _on_out(self, uc, port, size, value, user):
        self.port_out[port] += 1
        v = value & 0xFF
        # Sound card and its DMA channel take priority over the VGA decoding
        # below; note 0x20/0x21 (PIC) are shared, so the SB only observes them.
        if self.sb is not None and self.sb.owns(port):
            self.sb.write(port, v)
            if port not in (0x20, 0x21):
                return
        if port == 0x3C8:                     # DAC write index
            self.dac_index = v
            self.dac_phase = 0
            self.dac_latch = []
        elif port == 0x3C9:                   # DAC data: R, G, B (6-bit)
            self.dac_latch.append(v & 0x3F)
            if len(self.dac_latch) == 3:
                r, g, b = (c * 255 // 63 for c in self.dac_latch)
                self.palette[self.dac_index & 0xFF] = (r, g, b)
                self.dac_index = (self.dac_index + 1) & 0xFF
                self.dac_latch = []
                self.palette_writes += 1
        elif port == 0x3C4:
            self.seq_index = v
            if size == 2:                     # OUT dx,ax -> index+data in one
                self._seq_write(v, (value >> 8) & 0xFF)
        elif port == 0x3C5:
            self._seq_write(self.seq_index, v)
        elif port == 0x3D4:
            self.crtc_index = v
            if size == 2:
                self._crtc_write(v, (value >> 8) & 0xFF)
        elif port == 0x3D5:
            self._crtc_write(self.crtc_index, v)
        elif port == 0x3D8:                   # CGA mode control
            self.cga_mode_ctrl = v
        elif port == 0x3D9:                   # CGA colour select
            self.cga_colour = v
        elif port == 0x43:
            self.pit_latch_toggle[(v >> 6) & 3] = 0
        elif port == 0x40:
            self.pit_initial = v | (self.pit_initial & 0xFF00)

    def _seq_write(self, index, v):
        if index == 0x02:                     # map mask: which planes to write
            self.map_mask = v & 0x0F
            self.active_planes = tuple(p for p in range(4) if v & (1 << p))
        elif index == 0x04:                   # memory mode
            new_chain4 = bool(v & 0x08)
            if new_chain4 != self.chain4:
                print(f"  [vga] chain-4 "
                      f"{'ON (linear mode 13h)' if new_chain4 else 'OFF (Mode X planar)'}")
            self.chain4 = new_chain4

    def _crtc_write(self, index, v):
        self.crtc[index] = v
        if index in (0x0C, 0x0D):             # display start address
            self.start_addr = (self.crtc.get(0x0C, 0) << 8) | \
                self.crtc.get(0x0D, 0)
        elif index == 0x13:                   # logical line width
            self.crtc_offset = v
        elif index in (0x14, 0x17):
            self._update_addr_mode()

    def _update_addr_mode(self):
        """Determine what unit the CRTC start address is counted in.

        The start address is not necessarily a byte offset. Underline Location
        (0x14) bit 6 selects doubleword addressing; failing that, Mode Control
        (0x17) bit 6 picks byte (1) or word (0). Ducks sets 0x17=0xe3, 0x14=0x00
        -> byte addressing, so its page-flip value 0x7d00 means offset 32000,
        not 128000. Assuming doubleword puts page 1 past the end of the plane,
        which renders black and looks like flicker as the game flips pages.
        """
        old = self.start_mult
        if self.crtc.get(0x14, 0) & 0x40:
            self.start_mult = 4
        elif self.crtc.get(0x17, 0) & 0x40:
            self.start_mult = 1
        else:
            self.start_mult = 2
        if self.start_mult != old:
            unit = {1: "bytes", 2: "words", 4: "doublewords"}[self.start_mult]
            print(f"  [vga] CRTC start address counted in {unit} "
                  f"(0x14={self.crtc.get(0x14, 0):#04x} "
                  f"0x17={self.crtc.get(0x17, 0):#04x})")

    def _on_in(self, uc, port, size, user):
        self.port_in[port] += 1
        n = self.port_in[port]
        el = self._elapsed()
        if port == 0x3DA:
            # Bit 3 = vertical retrace, at the ~70 Hz frame rate (wall clock, so
            # the game paces its frames correctly).
            # Bit 0 = display enable, which runs at the ~31.5 kHz HORIZONTAL
            # rate. The snow-avoidance blit at 0x1ddf waits for a full 0->1
            # transition of bit 0 for every single word it copies, so this bit
            # must flip far faster than the emulator can be clocked from wall
            # time; toggle it per read instead.
            vsync = 0x08 if (el * 70.0) % 1.0 > 0.92 else 0x00
            return vsync | (0x01 if (n & 1) else 0x00)
        if port in (0x40, 0x41, 0x42):
            ch = port - 0x40
            counter = int(PIT_HZ * el) & 0xFFFF
            counter = (0x10000 - counter) & 0xFFFF
            t = self.pit_latch_toggle.get(ch, 0)
            self.pit_latch_toggle[ch] = 1 - t
            return counter & 0xFF if t == 0 else (counter >> 8) & 0xFF
        if port == 0x60:
            return self.last_scancode
        if port == 0x61:
            return 0x20
        if port == 0x201:
            return 0xFF
        if self.sb is not None:
            r = self.sb.read(port)
            if r is not None:
                return r
        if port == 0x22A:
            return 0xAA
        if port == 0x22C:
            return 0x00
        if port == 0x22E:
            return 0x80
        return 0x00

    def _watch_dma_buffer(self):
        """Hook guest writes to the DMA buffer once the card tells us where it is.

        This distinguishes "the game mixed silence because nothing is playing"
        from "the game never wrote any samples at all" - which look identical in
        the captured PCM.
        """
        sb = self.sb
        if sb is None or self._dma_hook is not None or not sb.dma_active:
            return
        lo = (sb.dma_page << 16) | sb.dma_addr
        hi = lo + max(512, sb.dma_len) - 1

        def on_write(uc, access, address, size, value, user):
            sb.buf_writes += size
            for i in range(size):
                b = (value >> (8 * i)) & 0xFF
                sb.buf_write_values[b] += 1
                # Announce the instant real audio first appears. Without this,
                # confirming "the game mixed an actual sound" means trawling a
                # megabyte of capture for the one moment it happened.
                if not sb.saw_signal and b != 0x80:
                    sb.saw_signal = True
                    print(f"  [sb] *** FIRST NON-SILENT SAMPLE {b:#04x} at "
                          f"t={self._elapsed():.1f}s - the game is mixing "
                          f"real audio ***")

        self._dma_hook = self.uc.hook_add(UC_HOOK_MEM_WRITE, on_write,
                                         None, lo, hi)
        print(f"  [sb] watching guest writes to DMA buffer "
              f"{lo:#07x}..{hi:#07x}")

    # ------------------------------------------------------------- sound IRQ
    def service_sound(self):
        """Advance DMA playback and deliver IRQ5 to the game's handler."""
        if self.sb is None:
            return
        now = self._elapsed()
        if self.sb_last_tick is None:
            self.sb_last_tick = now
            return
        dt = now - self.sb_last_tick
        self.sb_last_tick = now
        if dt <= 0:
            return
        self._watch_dma_buffer()
        self.sb.tick(self.uc, min(dt, 0.25))
        if not self.sb.irq_pending:
            return
        if not self.sb.irq_enabled():
            return
        # Only deliver when the guest has interrupts enabled and has installed a
        # handler; IRQ5 is INT 0dh on the master PIC.
        if not (self.uc.reg_read(UC_X86_REG_EFLAGS) & 0x200):
            return
        if self._dispatch_to_guest(0x0D):
            self.sb_irqs += 1
            self.sb.irq_pending = False

    # ------------------------------------------------------------- input
    def guest_owns_keyboard(self):
        """True while the program's own INT 09h handler is installed.

        Popcorn installs one for the game and the demo and takes it out again
        for the menus, so which path a key should take changes during a
        session and cannot be decided once at startup.  It has to be read out
        of the live vector rather than from the set-vector calls we saw: the
        restore is itself a set-vector, so "has it ever hooked INT 09h" is
        true from the first second onwards and would send every menu key into
        a handler that is no longer installed.
        """
        return self._ivt(0x09) != self.boot_int09

    def press_key(self, scancode, ascii_=0x00, down=True):
        """Queue one key transition for whichever path the guest is using."""
        code = scancode if down else (scancode | 0x80)
        if self.guest_owns_keyboard():
            self.scan_queue.append((code, ascii_))
        elif down:
            self.key_buf.append((scancode, ascii_))
            self.last_scancode = code

    def service_keyboard(self):
        """Deliver one queued scan code as IRQ 1, if the guest can take it.

        Only one per call: the handler must run to its IRET before the next
        code appears at port 0x60, exactly as the hardware would sequence
        them, and it must not be re-entered.  The interrupt flag is checked
        because the game runs CLI sections around its own screen blits.
        """
        if not self.scan_queue or not self.guest_owns_keyboard():
            return False
        if not (self.uc.reg_read(UC_X86_REG_EFLAGS) & 0x200):
            return False
        code, _ = self.scan_queue.popleft()
        self.last_scancode = code
        return self._dispatch_to_guest(0x09)

    def _bios_kbd(self):
        ah = self._reg(UC_X86_REG_AX) >> 8
        f = self.uc.reg_read(UC_X86_REG_EFLAGS)
        if ah in (0x01, 0x11):
            if self.key_buf:
                sc, asc = self.key_buf[0]
                self._set(UC_X86_REG_AX, (sc << 8) | asc)
                self.uc.reg_write(UC_X86_REG_EFLAGS, f & ~0x40)   # ZF=0
            else:
                self.uc.reg_write(UC_X86_REG_EFLAGS, f | 0x40)    # ZF=1
            return
        if ah in (0x00, 0x10):
            if self.key_buf:
                sc, asc = self.key_buf.popleft()
                self._set(UC_X86_REG_AX, (sc << 8) | asc)
            else:
                self._set(UC_X86_REG_AX, 0)
            return
        if ah == 0x02:
            self._set(UC_X86_REG_AX, 0)
            return

    def _on_intr(self, uc, intno, user):
        if intno == 0x2F:
            ax = self._reg(UC_X86_REG_AX)
            if ax == 0x4300:                  # XMS installation check
                self.int_counts[intno] += 1
                self._set(UC_X86_REG_AX, (ax & 0xFF00) | 0x80)
                return
            if ax == 0x4310:                  # get XMS driver entry point
                self.int_counts[intno] += 1
                self.uc.reg_write(UC_X86_REG_ES, XMS_STUB_SEG)
                self._set(UC_X86_REG_BX, 0)
                print(f"  [xms] driver entry handed to the game at "
                      f"{XMS_STUB_SEG:04x}:0000")
                return
        if intno == XMS_INT:
            self.int_counts[intno] += 1
            return self._xms_call()
        if intno == 0x29:
            # DOS fast console output: write AL at the cursor and advance it.
            # Real DOS always provides this vector; dropping it silently loses
            # every character and newline emitted through it.
            self.int_counts[intno] += 1
            self._tty(self._reg(UC_X86_REG_AX) & 0xFF)
            return
        return super()._on_intr(uc, intno, user)

    def _xms_call(self):
        """Service an XMS request made through the entry-point stub."""
        R = {"ax": UC_X86_REG_AX, "bx": UC_X86_REG_BX, "cx": UC_X86_REG_CX,
             "dx": UC_X86_REG_DX, "si": UC_X86_REG_SI, "di": UC_X86_REG_DI,
             "ds": UC_X86_REG_DS, "es": UC_X86_REG_ES}
        regs = {k: self.uc.reg_read(v) for k, v in R.items()}
        ah = (regs["ax"] >> 8) & 0xFF

        class Mem:
            def __init__(self, uc):
                self.uc = uc

            def read(self, addr, n):
                return bytes(self.uc.mem_read(addr, n))

            def write(self, addr, data):
                self.uc.mem_write(addr, bytes(data))

        out = self.xms.dispatch(ah, regs, Mem(self.uc))
        for name, val in out.items():
            self.uc.reg_write(R[name], val & 0xFFFF)

    def _dos(self):
        """Feed real keystrokes to the DOS console-input calls too.

        The README screen polls INT 21h AH=0Bh tens of thousands of times
        waiting for a key; without this it never advances.
        """
        ax = self._reg(UC_X86_REG_AX)
        ah = ax >> 8
        if ah == 0x0B:
            self.dos_counts[ah] += 1
            ready = bool(self.key_buf) or self.pending_scan is not None
            self._set(UC_X86_REG_AX, (ax & 0xFF00) | (0xFF if ready else 0x00))
            self._cf(False)
            return
        if ah in (0x01, 0x06, 0x07, 0x08):
            self.dos_counts[ah] += 1
            # AH=01, 07 and 08 BLOCK on real DOS - they do not return until a
            # key is there. Returning AL=0 instead is invisible while every
            # caller is Borland's getch behind a kbhit, and wrong for the one
            # that is not: pause_screen calls getch with nothing pending to hold
            # the COLOURMAP chart on screen, and a non-blocking read dismissed it
            # in the frame that drew it.
            #
            # Waiting here would deadlock - the event pump that delivers keys is
            # in the outer loop - so wind IP back over the two-byte INT and stop
            # the slice. main() pumps pygame, paces on clock.tick(60) and comes
            # back to the same instruction. AH=06 is left alone: with DL=0xFF it
            # is a status poll and must answer 0 rather than wait.
            if (ah != 0x06 and self.pending_scan is None
                    and not self.key_buf):
                self._set(UC_X86_REG_IP,
                          (self._reg(UC_X86_REG_IP) - 2) & 0xFFFF)
                self.uc.emu_stop()
                return
            # DOS delivers extended keys (arrows, function keys) as TWO reads:
            # a 0x00 prefix, then the scancode. Returning only the prefix and
            # dropping the key makes every extended key look like a null
            # character, which is why arrow keys did nothing.
            if self.pending_scan is not None:
                sc, self.pending_scan = self.pending_scan, None
                self._set(UC_X86_REG_AX, (ax & 0xFF00) | sc)
            elif self.key_buf:
                sc, asc = self.key_buf.popleft()
                if asc == 0:
                    self.pending_scan = sc
                    self._set(UC_X86_REG_AX, ax & 0xFF00)
                else:
                    self._set(UC_X86_REG_AX, (ax & 0xFF00) | asc)
            else:
                self._set(UC_X86_REG_AX, ax & 0xFF00)
            self._cf(False)
            return

        # DOS console output: render it to the screen and advance the cursor,
        # rather than only capturing the text.
        ds = self.uc.reg_read(UC_X86_REG_DS)
        dx = self._reg(UC_X86_REG_DX)
        if ah == 0x02:
            self.dos_counts[ah] += 1
            ch = dx & 0xFF
            self.stdout.append(ch)
            self._tty(ch)
            self._cf(False)
            return
        if ah == 0x09:
            self.dos_counts[ah] += 1
            s = self._rd(ds, dx, 256).split(b"$")[0]
            self.stdout += s
            for ch in s:
                self._tty(ch)
            self._cf(False)
            return
        if ah == 0x40 and self._reg(UC_X86_REG_BX) in (1, 2):
            self.dos_counts[ah] += 1
            cx = self._reg(UC_X86_REG_CX)
            s = self._rd(ds, dx, cx)
            self.stdout += s
            for ch in s:
                self._tty(ch)
            self._set(UC_X86_REG_AX, cx)
            self._cf(False)
            return
        return super()._dos()

    def _mouse(self):
        ax = self._reg(UC_X86_REG_AX)
        self.mouse_calls[ax] += 1
        if ax == 0x0000:
            self._set(UC_X86_REG_AX, 0xFFFF)
            self._set(UC_X86_REG_BX, 3)
            self.press_count = [0, 0, 0]
            self.release_count = [0, 0, 0]
            return
        if ax == 0x0003:
            x, y = self.mouse_pos
            self._set(UC_X86_REG_CX, x)
            self._set(UC_X86_REG_DX, y)
            self._set(UC_X86_REG_BX, self.mouse_btn)
            return
        if ax in (0x0005, 0x0006):
            # BX selects WHICH button is being asked about (0=left, 1=right,
            # 2=middle). The reply is that button's press/release count since
            # the last query, which must then be cleared, plus the cursor
            # position at that event. Ignoring BX makes every button look like
            # the same button, so per-button actions - Ducks assigns walk / use
            # tool / cycle tool to separate buttons - never fire correctly.
            idx = min(self._reg(UC_X86_REG_BX) & 0xFFFF, 2)
            counts = self.press_count if ax == 0x0005 else self.release_count
            positions = self.press_pos if ax == 0x0005 else self.release_pos
            self._set(UC_X86_REG_AX, self.mouse_btn)
            self._set(UC_X86_REG_BX, counts[idx])
            counts[idx] = 0
            px, py = positions[idx]
            self._set(UC_X86_REG_CX, px)
            self._set(UC_X86_REG_DX, py)
            return
        if ax == 0x000B:
            # Report whole mickeys and carry the remainder. Ducks never calls
            # 03h, so this is the only thing steering its cursor; quantising
            # small movements to zero would lose fine control entirely.
            dx, dy = int(self.mouse_rel[0]), int(self.mouse_rel[1])
            self.mouse_rel[0] -= dx
            self.mouse_rel[1] -= dy
            self._set(UC_X86_REG_CX, dx & 0xFFFF)
            self._set(UC_X86_REG_DX, dy & 0xFFFF)
            return
        if ax == 0x0004:
            self.mouse_pos = (self._reg(UC_X86_REG_CX),
                              self._reg(UC_X86_REG_DX))
            return
        return

    def _scroll(self, r1, c1, r2, c2, lines, attr):
        """Scroll a text window up, filling the vacated rows with `attr`."""
        base = 0xB8000
        blank = bytes([0x20, attr]) * max(0, c2 - c1 + 1)
        if lines == 0 or lines > (r2 - r1):
            for r in range(r1, r2 + 1):
                self.uc.mem_write(base + (r * 80 + c1) * 2, blank)
            return
        for r in range(r1, r2 + 1 - lines):
            src = base + ((r + lines) * 80 + c1) * 2
            row = bytes(self.uc.mem_read(src, (c2 - c1 + 1) * 2))
            self.uc.mem_write(base + (r * 80 + c1) * 2, row)
        for r in range(r2 + 1 - lines, r2 + 1):
            self.uc.mem_write(base + (r * 80 + c1) * 2, blank)

    def _tty(self, ch, page=0, attr=0x07):
        """Write one character at the cursor and advance it, like the BIOS.

        Used for both INT 10h 0eh and DOS console output. DOS output MUST move
        the cursor: Ducks mixes DOS writes with glyphs it pokes into 0xb8000
        itself, positioning those by asking INT 10h 03h where the cursor is. If
        console output leaves the cursor behind, the game's own text lands at
        stale columns - which is what makes its 80-column rules start mid-line
        and wrap.
        """
        row, col = self.cursor[page & 7]
        if ch == 0x0D:
            col = 0
        elif ch == 0x0A:
            row += 1
        elif ch == 0x08:
            col = max(0, col - 1)
        elif ch == 0x09:
            col = (col + 8) & ~7
        elif ch == 0x07:
            pass                              # bell: nothing to draw
        else:
            self.uc.mem_write(0xB8000 + (row * 80 + col) * 2,
                              bytes([ch, attr]))
            col += 1
        if col >= 80:
            col, row = 0, row + 1
        if row > 24:
            self._scroll(0, 0, 24, 79, 1, attr)
            row = 24
        self._set_cursor(page & 7, row, col)

    def _set_cursor(self, page, row, col):
        self.cursor[page & 7] = (row, col)
        # The BIOS keeps the cursor position in the BDA at 0x450 (col, row per
        # page); programs read it directly as often as they call INT 10h.
        self.uc.mem_write(0x450 + (page & 7) * 2,
                          bytes([col & 0xFF, row & 0xFF]))

    def _bios_video(self):
        ax = self._reg(UC_X86_REG_AX)
        ah, al = ax >> 8, ax & 0xFF
        bx = self._reg(UC_X86_REG_BX)
        page = (bx >> 8) & 7
        self.int10_fn[ah] += 1

        # Cursor position must be real state: Ducks calls 03h to find out where
        # to write, then pokes 0xb8000 itself. Returning nothing made every
        # message compute row 0 and overwrite the previous one.
        if ah == 0x02:
            dx = self._reg(UC_X86_REG_DX)
            if TRACE_TEXT:
                self._flush_text_run()
                print(f"  [txt] set cursor -> row {(dx >> 8) & 0xFF} "
                      f"col {dx & 0xFF}")
            self._set_cursor(page, (dx >> 8) & 0xFF, dx & 0xFF)
            return
        if ah == 0x03:
            row, col = self.cursor[page]
            if TRACE_TEXT:
                self._flush_text_run()
                print(f"  [txt] get cursor -> row {row} col {col}")
            self._set(UC_X86_REG_DX, ((row & 0xFF) << 8) | (col & 0xFF))
            self._set(UC_X86_REG_CX, 0x0607)
            return
        if ah == 0x05:
            self.active_page = al & 7
            return
        if ah in (0x06, 0x07):
            cx, dx = self._reg(UC_X86_REG_CX), self._reg(UC_X86_REG_DX)
            self._scroll((cx >> 8) & 0xFF, cx & 0xFF,
                         min((dx >> 8) & 0xFF, 24), min(dx & 0xFF, 79),
                         al, (bx >> 8) & 0xFF or 0x07)
            return
        if ah == 0x08:
            row, col = self.cursor[page]
            ch, at = bytes(self.uc.mem_read(0xB8000 + (row * 80 + col) * 2, 2))
            self._set(UC_X86_REG_AX, (at << 8) | ch)
            return
        if ah in (0x09, 0x0A):
            row, col = self.cursor[page]
            cnt = max(1, self._reg(UC_X86_REG_CX))
            attr = bx & 0xFF
            off = 0xB8000 + (row * 80 + col) * 2
            for i in range(min(cnt, 80 * 25)):
                self.uc.mem_write(off + i * 2,
                                  bytes([al, attr]) if ah == 0x09
                                  else bytes([al]))
            return
        if ah == 0x0E:
            self._tty(al, page)
            return
        if ah == 0x00:
            self.mode = al & 0x7F
            self.video_modes.append(self.mode)
            self.width, self.height = MODE_GEOM.get(self.mode, (320, 200))
            self.text_mode = self.mode in (0x00, 0x01, 0x02, 0x03, 0x07)
            if self.mode in CGA_MODES:
                # What the BIOS leaves in the two CGA registers for each mode.
                # Popcorn never writes 0x3d8 itself, so getting this wrong
                # loses the colour set the whole game is drawn in.
                self.cga_mode_ctrl = {0x04: 0x0A, 0x05: 0x0E, 0x06: 0x1E}[self.mode]
                self.cga_colour = 0x30
                self.uc.mem_write(VGA_B800, bytes(0x4000))
            print(f"  [vga] set mode {self.mode:#04x} -> "
                  f"{self.width}x{self.height} "
                  f"{'text' if self.text_mode else 'graphics'}")
        elif ah == 0x0F:                      # get current video mode
            self._set(UC_X86_REG_AX, (80 << 8) | self.mode)
            self._set(UC_X86_REG_BX, 0)
        elif ah == 0x10 and al == 0x12:       # set block of DAC registers
            first = self._reg(UC_X86_REG_BX)
            count = self._reg(UC_X86_REG_CX)
            es = self.uc.reg_read(UC_X86_REG_ES)
            dx = self._reg(UC_X86_REG_DX)
            blob = bytes(self.uc.mem_read(es * 16 + dx, count * 3))
            for i in range(count):
                r, g, b = blob[i * 3:i * 3 + 3]
                idx = (first + i) & 0xFF
                self.palette[idx] = (r * 255 // 63, g * 255 // 63,
                                     b * 255 // 63)
            self.palette_writes += count
        elif ah == 0x10 and al == 0x10:       # set single DAC register
            idx = self._reg(UC_X86_REG_BX) & 0xFF
            self.palette[idx] = (
                ((self._reg(UC_X86_REG_DX) >> 8) & 0x3F) * 255 // 63,
                (self._reg(UC_X86_REG_CX) >> 8 & 0x3F) * 255 // 63,
                (self._reg(UC_X86_REG_CX) & 0x3F) * 255 // 63)
            self.palette_writes += 1
        return

    # ------------------------------------------------------------ framebuffer
    def _on_plane_write(self, uc, access, address, size, value, user):
        """Shadow writes to the 0xa0000 aperture into four separate planes.

        In Mode X the CPU address selects a byte OFFSET and the sequencer map
        mask selects which of the four planes receive it, so several distinct
        pixels share one linear address. Unicorn's memory is flat and would let
        them overwrite each other, hence this shadow copy.
        """
        off = address - VGA_A000
        if off < 0 or off >= 0x10000:
            return
        planes, active = self.planes, self.active_planes
        if size == 1:
            b = value & 0xFF
            for p in active:
                planes[p][off] = b
        else:
            for i in range(size):
                b = (value >> (8 * i)) & 0xFF
                o = off + i
                if o < 0x10000:
                    for p in active:
                        planes[p][o] = b

    def cga_palette(self):
        """The four (or two) colours currently displayed, as RGB triples."""
        if self.mode == 0x06:
            return [CGA16[0], CGA16[self.cga_colour & 0x0F]]
        key = ((self.cga_colour >> 5) & 1, (self.cga_colour >> 4) & 1,
               (self.cga_mode_ctrl >> 2) & 1)
        return [CGA16[self.cga_colour & 0x0F]] + [CGA16[i] for i in CGA4[key]]

    def cga_framebuffer(self):
        """Decode the 0xb8000 aperture into one byte per pixel.

        CGA graphics memory is interlaced: even scan lines live at offset 0 and
        odd ones at 0x2000, 80 bytes to a row either way.  Mode 04h/05h packs
        four 2-bit pixels into each byte, most significant pair leftmost; mode
        06h packs eight 1-bit pixels.  Bit 3 of the mode-control register is
        video-enable, and the game clears it while it reprograms - a black
        frame there is correct, not a decode failure.
        """
        w, h = self.width, self.height
        if not (self.cga_mode_ctrl & 0x08):
            return bytes(w * h)
        vram = bytes(self.uc.mem_read(VGA_B800, 0x4000))
        img = bytearray(w * h)
        bpp = 1 if self.mode == 0x06 else 2
        ppb = 8 // bpp
        mask = (1 << bpp) - 1
        row_bytes = w // ppb
        for y in range(h):
            src = (0x2000 if y & 1 else 0) + (y >> 1) * row_bytes
            row = vram[src:src + row_bytes]
            out = y * w
            for x, byte in enumerate(row):
                base = out + x * ppb
                for k in range(ppb):
                    img[base + k] = (byte >> (8 - bpp * (k + 1))) & mask
        return bytes(img)

    def framebuffer(self):
        w, h = self.width, self.height
        if self.mode in CGA_MODES:
            pal = self.cga_palette()
            self.palette = pal + [(0, 0, 0)] * (256 - len(pal))
            return self.cga_framebuffer()
        if self.chain4:
            return bytes(self.uc.mem_read(VGA_A000, w * h))
        # Mode X: interleave the four planes back into linear pixels.
        row_bytes = self.crtc_offset * 2 if self.crtc_offset else w // 4
        base = self.start_addr * 4 if self.start_mult == 4 else self.start_addr
        img = bytearray(w * h)
        span = w // 4
        for p in range(4):
            plane = self.planes[p]
            for y in range(h):
                src = base + y * row_bytes
                chunk = plane[src:src + span]
                if len(chunk) < span:          # ran off the end of the plane
                    if not self._warned_range:
                        self._warned_range = True
                        print(f"  [vga] !! start address {self.start_addr:#x} "
                              f"x{self.start_mult} = {base} puts row {y} at "
                              f"{src}, past the {len(plane)}-byte plane; "
                              f"frame would render black. Wrong addressing "
                              f"unit? (0x14={self.crtc.get(0x14, 0):#04x} "
                              f"0x17={self.crtc.get(0x17, 0):#04x})")
                    chunk = chunk + bytes(span - len(chunk))
                img[y * w + p:y * w + w:4] = chunk
        if len(img) != w * h:                  # never expected; keep the caller safe
            print(f"  [vga] !! framebuffer {len(img)} != {w * h} "
                  f"(w={w} h={h} row_bytes={row_bytes} base={base:#x})")
            img = (bytes(img) + bytes(w * h))[:w * h]
        return bytes(img)

    def vga_state(self):
        return {
            "mode": f"{self.mode:#04x}",
            "geometry": f"{self.width}x{self.height}",
            "chain4": self.chain4,
            "map_mask": f"{self.map_mask:#03x}",
            "start_addr": f"{self.start_addr:#x}",
            "start_mult": self.start_mult,
            "crtc_offset": self.crtc_offset,
            "row_bytes": self.crtc_offset * 2 if self.crtc_offset
                         else self.width // 4,
            "crtc_regs": {f"{k:#02x}": f"{v:#02x}"
                          for k, v in sorted(self.crtc.items())},
            "dac_writes": self.palette_writes,
            "nonblack_palette": sum(1 for c in self.palette if c != (0, 0, 0)),
            "plane_nonzero": [sum(1 for b in pl[:16000] if b)
                              for pl in self.planes],
            "aperture_nonzero": sum(
                1 for b in bytes(self.uc.mem_read(VGA_A000, 16000)) if b),
        }

    def textbuffer(self):
        """80x25 character/attribute pairs from the text-mode framebuffer."""
        base = VGA_B000 if self.mode == 0x07 else VGA_B800
        return bytes(self.uc.mem_read(base, 80 * 25 * 2))



def render_text(m, font, cell_w, cell_h):
    """Draw the text-mode screen. Ducks shows its README here before the game."""
    surf = pygame.Surface((80 * cell_w, 25 * cell_h))
    surf.fill(CGA16[0])
    buf = m.textbuffer()
    for row in range(25):
        for col in range(80):
            i = (row * 80 + col) * 2
            ch, attr = buf[i], buf[i + 1]
            bg = CGA16[(attr >> 4) & 0x07]
            fg = CGA16[attr & 0x0F]
            rect = pygame.Rect(col * cell_w, row * cell_h, cell_w, cell_h)
            if bg != CGA16[0]:
                surf.fill(bg, rect)
            if ch not in (0, 32, 255):
                glyph = font.render(CP437[ch], False, fg, bg)
                surf.blit(glyph, rect.topleft)
    return surf


# CP437 -> unicode for the printable range plus the box-drawing glyphs DOS
# programs use for framing. Anything unmapped renders as a space.
CP437 = [" "] * 256
for _i in range(32, 127):
    CP437[_i] = chr(_i)
for _i, _c in zip(
        range(176, 224),
        "░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀"):
    CP437[_i] = _c
CP437[249] = "·"
CP437[250] = "·"
CP437[254] = "■"
CP437[7] = "•"
CP437[15] = "☼"
CP437[16] = "►"
CP437[17] = "◄"
CP437[24] = "↑"
CP437[25] = "↓"
CP437[26] = "→"
CP437[27] = "←"

VGA_B000 = 0xB0000


def make_surface(m, font=None, cell=(8, 16)):
    if m.text_mode and font is not None:
        return render_text(m, font, *cell)
    buf = m.framebuffer()
    w, h = m.width, m.height
    if len(buf) != w * h:
        print(f"  [vga] !! buffer {len(buf)} bytes but {w}x{h} needs {w * h}")
        buf = (buf + bytes(w * h))[:w * h]
    surf = pygame.image.frombuffer(buf, (w, h), "P")
    surf.set_palette(m.palette)
    return surf


class AudioSink:
    """Stream the card's PCM to the host speakers via SDL.

    The Sound Blaster produces unsigned 8-bit mono at whatever rate the game
    programmed. We consume whatever has accumulated since the last call and
    queue it on a dedicated mixer channel.
    """

    def __init__(self, verbose=True):
        self.pos = 0
        self.rate = None
        self.ok = False
        self.queued = 0
        self.dropped = 0
        self.pending = deque()
        self.chan = None
        self.verbose = verbose

    def ensure_rate(self, rate):
        """Open (or reopen) the mixer at the rate the game actually programmed.

        Raw bytes handed to pygame are interpreted at the mixer's frequency, so
        a mismatch plays the sample at the wrong speed and pitch. Ducks selects
        22222 Hz via the DSP time constant, which we only learn at runtime.
        """
        if self.ok and self.rate == rate:
            return
        try:
            if pygame.mixer.get_init():
                pygame.mixer.quit()
            pygame.mixer.init(frequency=rate, size=8, channels=1, buffer=4096)
            pygame.mixer.set_num_channels(4)
            self.chan = pygame.mixer.Channel(0)
            self.rate, self.ok = rate, True
            print(f"  [audio] mixer at {rate} Hz: {pygame.mixer.get_init()}")
        except Exception as e:
            self.ok = False
            print(f"  [audio] mixer unavailable ({e}); "
                  f"PCM still captured to WAV")

    def push(self, sb, chunk=8192):
        if sb is None:
            return
        if sb.sample_rate and sb.sample_rate != self.rate:
            self.ensure_rate(sb.sample_rate)
        if not self.ok:
            return
        # Slice off whole chunks; never advance past data we failed to queue.
        while len(sb.pcm) - self.pos >= chunk:
            self.pending.append(bytes(sb.pcm[self.pos:self.pos + chunk]))
            self.pos += chunk
        try:
            # A mixer channel holds one playing plus one queued sound, so keep
            # both slots fed every iteration rather than dropping the overflow.
            while self.pending:
                if not self.chan.get_busy():
                    self.chan.play(
                        pygame.mixer.Sound(buffer=self.pending.popleft()))
                elif self.chan.get_queue() is None:
                    self.chan.queue(
                        pygame.mixer.Sound(buffer=self.pending.popleft()))
                else:
                    break
                self.queued += 1
        except Exception as e:
            if self.verbose:
                print(f"  [audio] push failed: {e}")
                self.verbose = False
        # If we fall a long way behind realtime, drop the backlog rather than
        # growing without bound - and say so instead of hiding it.
        if len(self.pending) > 120:
            self.dropped += len(self.pending) - 40
            while len(self.pending) > 40:
                self.pending.popleft()


def capture(m, screen, tag, outdir="debug"):
    """Dump everything needed to debug the display off-line."""
    import json
    os.makedirs(outdir, exist_ok=True)
    base = os.path.join(outdir, tag)

    pygame.image.save(screen, f"{base}_window.png")
    if not m.text_mode:
        raw = make_surface(m)
        pygame.image.save(raw.convert(24), f"{base}_raw.png")
        # Raw planes + palette, so alternative interpretations can be tried
        # without having to reach this point in the game again.
        with open(f"{base}_planes.bin", "wb") as f:
            for pl in m.planes:
                f.write(pl)
        with open(f"{base}_aperture.bin", "wb") as f:
            f.write(bytes(m.uc.mem_read(VGA_A000, 0x10000)))
    else:
        with open(f"{base}_text.txt", "w") as f:
            buf = m.textbuffer()
            for row in range(25):
                f.write("".join(CP437[buf[(row * 80 + c) * 2]]
                                for c in range(80)).rstrip() + "\n")
    with open(f"{base}_palette.bin", "wb") as f:
        for c in m.palette:
            f.write(bytes(c))
    state = m.vga_state()
    state["cs:ip"] = (f"{m._reg(UC_X86_REG_CS):04x}:"
                      f"{m._reg(UC_X86_REG_IP):04x}")
    state["elapsed"] = round(m._elapsed(), 2)
    state["files_read"] = m.files_read
    with open(f"{base}_state.json", "w") as f:
        json.dump(state, f, indent=2)
    print(f"  [capture] {base}_*  state={json.dumps(state)}")
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--cmdline", default="",
                    help="DOS command tail: the level file to load, e.g. poptab")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--blaster", action="store_true")
    ap.add_argument("--chunk", type=int, default=400_000,
                    help="instructions to run between display updates")
    ap.add_argument("--shots", type=int, default=0,
                    help="save this many PNG frames then exit (headless)")
    ap.add_argument("--shot-every", type=float, default=1.5,
                    help="seconds between saved frames")
    ap.add_argument("--shot-dir", default="shots")
    ap.add_argument("--status-every", type=float, default=5.0)
    ap.add_argument("--wav", default="popcorn_audio.wav",
                    help="dump captured PCM here on exit")
    ap.add_argument("--no-audio", action="store_true",
                    help="emulate the card but do not open the host mixer")
    ap.add_argument("--sound-slices", type=int, default=32,
                    help="how many times per display update to service the "
                         "sound card and deliver its IRQ")
    ap.add_argument("--watch-dgroup", default="",
                    help="comma-separated DGROUP offsets to watch for writes, "
                         "e.g. 0x2104,0x18f6")
    ap.add_argument("--text-trace", action="store_true",
                    help="log cursor moves and direct text-buffer writes")
    ap.add_argument("--keys", default="",
                    help="scripted input for an unattended run: a comma-"
                         "separated list of TIME:KEY, e.g. 4:f1,9:right,"
                         "9.5:-right. TIME is seconds of wall clock, KEY is a "
                         "pygame key name; a leading '-' makes it a release "
                         "rather than a press-and-release")
    ap.add_argument("--run-seconds", type=float, default=0.0,
                    help="stop after this many seconds (0 = run until quit)")
    ap.add_argument("--mouse-debug", action="store_true",
                    help="log every mouse button event and INT 33h query")
    args = ap.parse_args()

    global TRACE_TEXT, WATCH_DGROUP
    TRACE_TEXT = args.text_trace
    WATCH_DGROUP = [int(x, 0) for x in args.watch_dgroup.split(",") if x.strip()]

    headless = args.shots > 0
    if headless:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
        os.makedirs(args.shot_dir, exist_ok=True)

    pygame.init()
    m = VgaDos(args.exe, blaster=args.blaster, max_insns=1 << 62,
               cmdline=args.cmdline)
    audio = None
    if args.blaster and not args.no_audio and not headless:
        audio = AudioSink()
    print(f"=== running {args.exe} "
          f"(BLASTER {'set' if args.blaster else 'unset'}) ===")
    print("    host filesystem READ-ONLY; writes intercepted in memory")

    pygame.font.init()
    CELL = (8, 16)
    fpath = pygame.font.match_font("dejavusansmono,liberationmono,monospace")
    font = pygame.font.Font(fpath, 13) if fpath \
        else pygame.font.SysFont(None, 16)

    def base_size():
        return (80 * CELL[0], 25 * CELL[1]) if m.text_mode \
            else (m.width, m.height)

    bw, bh = base_size()
    win_w, win_h = bw * args.scale, bh * args.scale
    screen = pygame.display.set_mode((win_w, win_h))
    pygame.display.set_caption("Popcorn (1988) - unicorn/SDL")
    clock = pygame.time.Clock()

    # Scripted input, so a screen several menus deep can be reached without a
    # person at the keyboard. Parsed up front: a typo in --keys should fail
    # before the machine starts, not four seconds into a run.
    script = []
    for item in (x.strip() for x in args.keys.split(",") if x.strip()):
        when, _, name = item.partition(":")
        release = name.startswith("-")
        name = name.lstrip("-")
        key = next((k for k in (getattr(pygame, f"K_{n}", None)
                                for n in (name.lower(), name.upper()))
                    if k is not None), None)
        if key is None or key not in KEYMAP:
            raise SystemExit(f"--keys: no scan code for {name!r}")
        script.append((float(when), key, release))
    script.sort()
    script.reverse()          # popped from the end, earliest first

    cs = m._reg(UC_X86_REG_CS)
    ip = m._reg(UC_X86_REG_IP)
    addr = cs * 16 + ip
    running = True
    shots_taken = 0
    next_shot = args.shot_every
    next_status = args.status_every
    frames = 0
    paused = False
    cap_n = 0
    print("    controls: shift+F9 pause/resume, shift+F10 capture, "
          "shift+F12 quit; every other key goes to the game")
    print("    or from a shell: touch capture.request / touch pause.request")

    while running:
        if not paused:
            # Run the chunk in slices, servicing sound between each. A whole
            # chunk between IRQ deliveries is an enormous latency by the
            # guest's standards - real hardware interrupts within microseconds
            # of a block completing - and anything that waits on an interrupt
            # with a retry counter rather than a clock gives up long before it.
            slices = max(1, args.sound_slices if m.sb is not None else 1)
            step = max(1000, args.chunk // slices)
            for _ in range(slices):
                try:
                    m.uc.emu_start(addr, 0, count=step)
                except UcError as e:
                    print(f"  [cpu] {e} at {m._reg(UC_X86_REG_CS):04x}:"
                          f"{m._reg(UC_X86_REG_IP):04x}")
                    running = False
                    break
                if m.finished:
                    print(f"  [dos] program exited: {m.finished}")
                    running = False
                    break
                addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
                m.service_sound()
                m.service_keyboard()
                addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
            if audio is not None:
                audio.push(m.sb)

        while script and script[-1][0] <= m._elapsed():
            when, key, release = script.pop()
            sc, asc = KEYMAP[key]
            if release:
                m.press_key(sc, asc, down=False)
            else:
                m.press_key(sc, asc, down=True)
                m.press_key(sc, asc, down=False)
            print(f"  [keys] t={when:.1f}s "
                  f"{'release' if release else 'press'} scan {sc:#04x}")
        if args.run_seconds and m._elapsed() >= args.run_seconds:
            print(f"  [ctl] --run-seconds {args.run_seconds} reached")
            running = False

        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False
            elif ev.type == pygame.KEYDOWN:
                # The game owns F1-F10 - they are its whole menu - so the
                # emulator's controls sit behind Shift and everything else,
                # shifted or not, is passed straight through.
                shift = bool(ev.mod & pygame.KMOD_SHIFT)
                if shift and ev.key == pygame.K_F12:
                    running = False
                elif shift and ev.key == pygame.K_F9:
                    paused = not paused
                    print(f"  [ctl] {'PAUSED' if paused else 'resumed'} "
                          f"at {m._reg(UC_X86_REG_CS):04x}:"
                          f"{m._reg(UC_X86_REG_IP):04x}")
                elif shift and ev.key == pygame.K_F10:
                    cap_n += 1
                    capture(m, screen, f"cap{cap_n:02d}")
                elif shift and ev.key in (pygame.K_F7, pygame.K_F8):
                    m.mouse_sens *= 0.8 if ev.key == pygame.K_F7 else 1.25
                    print(f"  [ctl] mouse sensitivity {m.mouse_sens:.3f} "
                          f"(effective {m.mouse_sens / args.scale:.3f} "
                          f"mickeys per window pixel)")
                else:
                    mapped = KEYMAP.get(ev.key)
                    if mapped:
                        m.press_key(mapped[0], mapped[1], down=True)
            elif ev.type == pygame.KEYUP:
                mapped = KEYMAP.get(ev.key)
                if mapped:
                    m.press_key(mapped[0], mapped[1], down=False)
            elif ev.type == pygame.MOUSEMOTION:
                mx, my = ev.pos
                # INT 33h reports in a virtual 640x200 space for mode 13h.
                # Against the window's real size, for the same reason the blit
                # below uses it: win_w/win_h are what was asked for, and the
                # pointer arrives in the window that actually exists.
                sw, sh = screen.get_size()
                m.mouse_pos = (int(mx / sw * 640), int(my / sh * 200))
                # ev.rel is in window pixels, so at --scale 3 every movement is
                # reported 3x too large. Divide it back out so one game pixel of
                # cursor travel matches one game pixel of pointer travel, and
                # let mouse_sens trim the rest at runtime (F7/F8).
                k = m.mouse_sens / args.scale
                m.mouse_rel[0] += ev.rel[0] * k
                m.mouse_rel[1] += ev.rel[1] * k
            elif ev.type in (pygame.MOUSEBUTTONDOWN, pygame.MOUSEBUTTONUP):
                # pygame numbers buttons 1=left, 2=middle, 3=right; INT 33h
                # numbers them 0=left, 1=right, 2=middle. Wheel and extra
                # buttons are not part of the mouse driver interface.
                idx = {1: 0, 3: 1, 2: 2}.get(ev.button)
                if idx is not None:
                    # Track the mask from the events themselves. Reading
                    # pygame.mouse.get_pressed() here returns state that has not
                    # caught up with the event being handled, so the mask lags
                    # by one press - and INT 33h 05h/06h report it in AX.
                    # Bits are INT 33h order: 0=left, 1=right, 2=middle.
                    bit = 1 << idx
                    if ev.type == pygame.MOUSEBUTTONDOWN:
                        m.mouse_btn |= bit
                        m.press_count[idx] += 1
                        m.press_pos[idx] = m.mouse_pos
                    else:
                        m.mouse_btn &= ~bit
                        m.release_count[idx] += 1
                        m.release_pos[idx] = m.mouse_pos
                    if args.mouse_debug:
                        names = ("left", "right", "middle")
                        print(f"  [mouse] {names[idx]} "
                              f"{'down' if ev.type == pygame.MOUSEBUTTONDOWN else 'up'}"
                              f" at {m.mouse_pos} mask={m.mouse_btn:#03x}")

        # File-based control, so a capture can be requested from outside the
        # window: `touch capture.request` / `touch pause.request`.
        if os.path.exists("capture.request"):
            os.remove("capture.request")
            cap_n += 1
            capture(m, screen, f"cap{cap_n:02d}")
        if os.path.exists("pause.request"):
            os.remove("pause.request")
            paused = not paused
            print(f"  [ctl] {'PAUSED' if paused else 'resumed'} by request "
                  f"at {m._reg(UC_X86_REG_CS):04x}:"
                  f"{m._reg(UC_X86_REG_IP):04x}")

        nb = base_size()
        if nb != (bw, bh):
            bw, bh = nb
            win_w, win_h = bw * args.scale, bh * args.scale
            screen = pygame.display.set_mode((win_w, win_h))

        # Convert the 8-bit palettised surface to the display format before
        # scaling: transform.scale needs matching source/destination formats.
        surf = make_surface(m, font, CELL).convert(screen)
        # The display surface's own size, not the size that was asked for. A
        # window manager can hand back a smaller window than set_mode requested
        # - and can resize it later - and transform.scale requires the
        # destination to be exactly the size given, so anything else is a crash
        # on the first frame. native.py has always done it this way.
        pygame.transform.scale(surf, screen.get_size(), screen)
        pygame.display.flip()
        frames += 1

        el = m._elapsed()
        if True:
            if el >= next_status:
                fb = m.framebuffer()
                print(f"  [stat] t={el:6.1f}s  "
                      f"cs:ip={m._reg(UC_X86_REG_CS):04x}:"
                      f"{m._reg(UC_X86_REG_IP):04x}  "
                      f"mode={m.mode:#04x} dac={m.palette_writes} "
                      f"int10={m.int_counts[0x10]} int33={m.int_counts[0x33]} "
                      f"int21={m.int_counts[0x21]} "
                      f"fb_nonzero={sum(1 for b in fb[:8000] if b)} "
                      f"out3c9={m.port_out.get(0x3C9, 0)} "
                      f"in3da={m.port_in.get(0x3DA, 0)} "
                      f"chain4={m.chain4} start={m.start_addr:#x} "
                      f"crtc_off={m.crtc_offset} "
                      f"vde={m.crtc.get(0x12, 0):#x}")
                next_status += args.status_every
                if m.text_mode:
                    buf = m.textbuffer()
                    chars = sum(1 for i in range(0, len(buf), 2)
                                if buf[i] not in (0, 32))
                    print(f"  [text] {chars} non-blank cells at "
                          f"{'0xb8000' if m.mode != 7 else '0xb0000'}")
                    for row in range(25):
                        line = "".join(
                            CP437[buf[(row * 80 + c) * 2]] for c in range(80))
                        if line.strip():
                            print(f"    |{line.rstrip()}")
            if args.shots and el >= next_shot:
                path = os.path.join(args.shot_dir, f"frame{shots_taken:02d}.png")
                pygame.image.save(screen, path)
                nz = sum(1 for c in m.palette if c != (0, 0, 0))
                print(f"  [shot] {path}  t={el:5.1f}s  "
                      f"mode={m.mode:#04x} palette_entries={nz} "
                      f"dac_writes={m.palette_writes}")
                shots_taken += 1
                next_shot += args.shot_every
                if shots_taken >= args.shots:
                    running = False
        if not headless:
            clock.tick(60)

    print(f"\n=== finished after {frames} display updates, "
          f"{m._elapsed():.1f}s ===")
    print(f"  video modes set : {[hex(v) for v in m.video_modes]}")
    print(f"  DAC palette sets: {m.palette_writes}")
    print(f"  interrupts used : "
          f"{{{', '.join(f'{n:02x}h:{c}' for n, c in sorted(m.int_counts.items()))}}}")
    print(f"  INT 10h funcs   : "
          f"{{{', '.join(f'AH={a:02x}h:{c}' for a, c in m.int10_fn.most_common(12))}}}")
    print(f"  INT 21h funcs   : "
          f"{{{', '.join(f'AH={a:02x}h:{c}' for a, c in m.dos_counts.most_common(12))}}}")
    print(f"  INT 33h funcs   : "
          f"{{{', '.join(f'AX={a:04x}:{c}' for a, c in m.mouse_calls.most_common(12))}}}")
    print(f"  video-mem writes: "
          f"{{{', '.join(f'{k}:{v}' for k, v in sorted(m.vidwrites.items()))}}}")
    for k, (lo, hi) in sorted(m.vidrange.items()):
        print(f"    {k} address range {lo:#07x}..{hi:#07x}")
    # Where in the whole video aperture is there actually non-zero content?
    ap = bytes(m.uc.mem_read(0xA0000, 0x20000))
    runs, inrun = [], None
    for i in range(0, len(ap), 512):
        blk = any(ap[i:i + 512])
        if blk and inrun is None:
            inrun = i
        elif not blk and inrun is not None:
            runs.append((0xA0000 + inrun, 0xA0000 + i))
            inrun = None
    if inrun is not None:
        runs.append((0xA0000 + inrun, 0xA0000 + len(ap)))
    print(f"  non-zero video regions: "
          f"{[f'{a:#07x}..{b:#07x}' for a, b in runs] or 'none'}")
    print(f"  OUT ports       : "
          f"{{{', '.join(f'{p:#05x}:{c}' for p, c in m.port_out.most_common(14))}}}")
    import json
    print(f"  XMS             : {json.dumps(m.xms.summary(), indent=2)}")
    if audio is not None:
        # Distinguish "the emulator could not keep up" from "the card model is
        # wrong": queued vs dropped says whether the host starved, while the
        # WAV says whether the samples themselves were good.
        print(f"  audio streaming : {audio.queued} chunks queued, "
              f"{audio.dropped} dropped, {len(audio.pending)} still pending "
              f"at exit")
    if m.sb is not None:
        print(f"  sound blaster   : {json.dumps(m.sb.summary(), indent=2)}")
        print(f"  IRQ5 delivered  : {m.sb_irqs}")
        path = m.sb.write_wav(args.wav)
        print(f"  audio written   : {path or 'nothing - no PCM produced'}")
    print(f"  files read      : {m.files_read}")
    print(f"  files written   : {m.files_written} (intercepted)")
    print(f"  files missing   : {m.files_missing}")
    if m.stdout:
        print("  console output  :")
        for line in m.stdout.decode('latin1').replace('\r', '').split('\n'):
            print(f"    | {line}")
    pygame.quit()


if __name__ == "__main__":
    sys.exit(main())
