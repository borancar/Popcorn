#!/usr/bin/env python3
"""
Run the game under an emulated DOS and log every service call it makes.

Static disassembly desynchronises constantly - Popcorn is hand-written assembly
with data tables sitting in the middle of the code segment - so it cannot answer
"which files does this open" reliably. Executing the real binary can. This
provides just enough DOS/BIOS for the game to start up, and records every
interrupt, file access and port write it performs.

SAFETY: the host filesystem is opened READ-ONLY. Writes the program attempts are
satisfied from an in-memory overlay and logged, never applied to real files.
The high-score file is the one the game writes, and it is not touched here.

Usage:
    python trace_dos.py                 # trace popcorn.unpacked.exe
    python trace_dos.py --exe popcorn/popcorn.exe
"""
import argparse
import os
import struct
import sys
from collections import Counter
from unicorn import *
from unicorn.x86_const import *

# The game lives under the repository, in popcorn/, and is never part of it.
# Anchor on this file rather than on the working directory, so the tools can be
# run from anywhere; POPCORN_GAME_DIR overrides it for a different layout.
HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.abspath(os.environ.get(
    "POPCORN_GAME_DIR", os.path.join(HERE, "popcorn")))
UNPACKED = os.path.join(HERE, "popcorn.unpacked.exe")
MEM_SIZE = 0x200000
PSP_SEG = 0x0100
ENV_SEG = 0x00F0

DOS_FN = {
    0x00: "terminate", 0x02: "write char", 0x06: "direct console I/O",
    0x09: "write string", 0x0B: "check stdin", 0x0C: "flush+read",
    0x19: "get current disk", 0x1A: "set DTA", 0x25: "set int vector",
    0x2A: "get date", 0x2C: "get time", 0x2F: "get DTA",
    0x30: "get DOS version", 0x33: "get/set break", 0x35: "get int vector",
    0x36: "get free disk space", 0x38: "get country", 0x3B: "chdir",
    0x3C: "CREATE", 0x3D: "OPEN", 0x3E: "close", 0x3F: "READ",
    0x40: "WRITE", 0x41: "DELETE", 0x42: "seek", 0x43: "get/set attr",
    0x44: "ioctl", 0x47: "get cwd", 0x48: "alloc", 0x49: "free",
    0x4A: "resize", 0x4B: "EXEC", 0x4C: "exit", 0x4E: "find first",
    0x4F: "find next", 0x56: "rename", 0x57: "file date", 0x62: "get PSP",
}


def host_path(dos_path):
    """Resolve a DOS path to a real path, case-insensitively, inside GAME_DIR."""
    p = dos_path.replace("\\", "/").lstrip("/")
    if len(p) > 1 and p[1] == ":":
        p = p[2:].lstrip("/")
    cur = GAME_DIR
    if not p:
        return cur
    for part in p.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            continue
        try:
            entries = os.listdir(cur)
        except OSError:
            return os.path.join(cur, part)
        match = next((e for e in entries if e.lower() == part.lower()), part)
        cur = os.path.join(cur, match)
    return cur


class Handle:
    def __init__(self, path, data, writable):
        self.path = path
        self.data = bytearray(data)
        self.pos = 0
        self.writable = writable
        self.written = 0


class DosMachine:
    def __init__(self, exe_path, blaster=False, verbose=True,
                 max_insns=80_000_000, cmdline=""):
        self.verbose = verbose
        self.cmdline = cmdline
        self.max_insns = max_insns
        self.log = []
        self.int_counts = Counter()
        self.dos_counts = Counter()
        self.files_read = {}
        self.files_written = {}
        self.files_missing = []
        self.port_out = Counter()
        self.port_in = Counter()
        self.stdout = bytearray()
        self.handles = {}
        self.overlay = {}   # DOS path -> bytes, for files the game creates
        self.file_ops = []  # always recorded, regardless of verbosity
        self.next_handle = 5
        self.dta = (PSP_SEG, 0x80)
        self.finished = None
        self.blocks = 0
        self.video_modes = []
        self.hooked_vectors = {}
        self.guest_dispatch = Counter()
        self.mouse_calls = Counter()
        self.mouse_x = 160
        self.mouse_y = 100

        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, MEM_SIZE)
        self._load(exe_path, blaster)
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_INSN, self._on_in, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, self._on_out, None, 1, 0, UC_X86_INS_OUT)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)

    # ------------------------------------------------------------------ load
    def _load(self, path, blaster):
        data = open(path, "rb").read()
        (cblp, cp, crlc, cparhdr, minalloc, maxalloc, ss, sp, csum, ip, cs,
         lfarlc, ovno) = struct.unpack_from("<13H", data, 2)
        hdr = cparhdr * 16
        size = (cp - 1) * 512 + cblp - hdr if cblp else cp * 512 - hdr
        image = data[hdr:hdr + size]

        # Minimal BIOS data area: equipment word, memory size, video mode,
        # timer tick. Some DOS games read these directly instead of via BIOS.
        self.uc.mem_write(0x410, struct.pack("<H", 0x0021))   # equipment
        self.uc.mem_write(0x413, struct.pack("<H", 640))      # KB of RAM
        self.uc.mem_write(0x449, bytes([0x03]))               # video mode
        self.uc.mem_write(0x44A, struct.pack("<H", 80))       # columns
        self.uc.mem_write(0x46C, struct.pack("<I", 0x00010000))  # tick count

        env = b"COMSPEC=C:\\COMMAND.COM\x00PATH=C:\\\x00"
        if blaster:
            env += b"BLASTER=A220 I5 D1\x00"
        env += b"\x00\x01\x00C:\\POPCORN.EXE\x00"
        self.uc.mem_write(ENV_SEG * 16, env)

        psp = bytearray(0x100)
        psp[0:2] = b"\xcd\x20"
        struct.pack_into("<H", psp, 0x02, 0x9000)
        struct.pack_into("<H", psp, 0x2C, ENV_SEG)
        psp[0x50:0x53] = b"\xcd\x21\xcb"
        tail = (" " + self.cmdline).encode("latin1") if self.cmdline else b""
        psp[0x80] = len(tail)
        psp[0x81:0x81 + len(tail)] = tail
        psp[0x81 + len(tail)] = 0x0D
        self.uc.mem_write(PSP_SEG * 16, bytes(psp))

        self.load_seg = PSP_SEG + 0x10
        base = self.load_seg * 16
        self.uc.mem_write(base, image)
        for i in range(crlc):
            o, s = struct.unpack_from("<HH", data, lfarlc + i * 4)
            a = base + s * 16 + o
            v = struct.unpack("<H", self.uc.mem_read(a, 2))[0]
            self.uc.mem_write(a, struct.pack("<H", (v + self.load_seg) & 0xFFFF))

        self.uc.reg_write(UC_X86_REG_CS, (self.load_seg + cs) & 0xFFFF)
        self.uc.reg_write(UC_X86_REG_IP, ip)
        self.uc.reg_write(UC_X86_REG_SS, (self.load_seg + ss) & 0xFFFF)
        self.uc.reg_write(UC_X86_REG_SP, sp)
        self.uc.reg_write(UC_X86_REG_DS, PSP_SEG)
        self.uc.reg_write(UC_X86_REG_ES, PSP_SEG)
        self.uc.reg_write(UC_X86_REG_AX, 0)
        self.uc.reg_write(UC_X86_REG_CX, 0xFF)
        self.uc.reg_write(UC_X86_REG_DX, PSP_SEG)
        self.start = (self.load_seg + cs) * 16 + ip

    # ----------------------------------------------------------------- utils
    @staticmethod
    def device_info(handle):
        """The device-information word DOS returns for AH=44h AL=00h.

        Bit 7 marks a character device. The three standard handles are the
        console and everything this machine opens is a real file, so the answer
        is that simple - and it is deterministic, which reading an untouched DX
        was not. Lives here rather than in a caller because native.py serves
        isatty()/ioctl() at the function level and the two answers must agree;
        it delegates to this one.
        """
        return 0x80 if handle in (0, 1, 2) else 0x00

    def _rd(self, seg, off, n):
        return bytes(self.uc.mem_read(seg * 16 + off, n))

    def _str(self, seg, off, maxlen=128):
        b = self._rd(seg, off, maxlen)
        return b.split(b"\x00")[0].decode("latin1")

    def _reg(self, r):
        return self.uc.reg_read(r)

    def _set(self, r, v):
        self.uc.reg_write(r, v & 0xFFFF)

    def _cf(self, on):
        f = self.uc.reg_read(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (f | 1) if on else (f & ~1))

    def _fop(self, msg):
        """Record a file operation unconditionally.

        _note() is silenced when the machine is constructed with verbose=False,
        which is how the native port runs - so file activity was invisible in
        exactly the situation where it needed diagnosing.
        """
        self.file_ops.append(msg)
        print(f"    [file] {msg}")      # always: needed for live diagnosis

    def _note(self, msg):
        self.log.append(msg)
        if self.verbose:
            print(f"    {msg}")

    # ----------------------------------------------------------------- ports
    def _on_in(self, uc, port, size, user):
        self.port_in[port] += 1
        n = self.port_in[port]

        # VGA input status 1. Bit 3 = vertical retrace, bit 0 = display enable.
        # Must toggle: the game polls both for retrace-start and retrace-end, so
        # a constant value deadlocks whichever loop is waiting for the change.
        if port == 0x3DA:
            return 0x09 if (n & 1) else 0x00
        if port in (0x40, 0x41, 0x42):        # PIT counter latch, running down
            return (0xFFFF - n * 37) & 0xFF
        if port == 0x60:                      # keyboard data
            return 0x00
        if port == 0x61:                      # PC speaker / port B
            return (n & 0x10) | 0x20
        if port == 0x201:                     # joystick: none attached
            return 0xFF
        if port == 0x3C2:                     # VGA input status 0
            return 0x10
        # Sound Blaster DSP.
        if port == 0x22A:                     # DSP read data
            return 0xAA                       # reset acknowledgement
        if port == 0x22C:                     # DSP write status: bit7=busy
            return 0x00                       # always ready
        if port == 0x22E:                     # DSP read status: bit7=data ready
            return 0x80
        if port == 0x388 or port == 0x389:    # OPL FM status
            return 0x00
        return 0x00

    def _on_out(self, uc, port, size, value, user):
        self.port_out[port] += 1

    def _on_unmapped(self, uc, access, address, size, value, user):
        self._note(f"! unmapped access {address:#x} size={size} "
                   f"at {self._reg(UC_X86_REG_CS):04x}:"
                   f"{self._reg(UC_X86_REG_IP):04x}")
        return False

    # ------------------------------------------------------------------ ints
    def _ivt(self, intno):
        off, seg = struct.unpack("<HH", self.uc.mem_read(intno * 4, 4))
        return seg, off

    def _dispatch_to_guest(self, intno):
        """Vector a software interrupt to a handler the program installed.

        Borland's runtime hooks INT 34h-3Eh for 80x87 emulation and the game
        hooks timer/keyboard vectors. Those interrupts must reach the guest's own
        code, not our shim, or floating point silently does nothing.
        """
        seg, off = self._ivt(intno)
        if (seg, off) == (0, 0):
            return False
        sp = self._reg(UC_X86_REG_SP)
        ss = self._reg(UC_X86_REG_SS)
        flags = self.uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF
        for val in (flags, self._reg(UC_X86_REG_CS), self._reg(UC_X86_REG_IP)):
            sp = (sp - 2) & 0xFFFF
            self.uc.mem_write(ss * 16 + sp, struct.pack("<H", val))
        self._set(UC_X86_REG_SP, sp)
        self.uc.reg_write(UC_X86_REG_CS, seg)
        self.uc.reg_write(UC_X86_REG_IP, off)
        self.guest_dispatch[intno] += 1
        return True

    def _on_intr(self, uc, intno, user):
        self.int_counts[intno] += 1
        # A handler the program installed itself always wins.
        if intno not in (0x21, 0x20) and self._dispatch_to_guest(intno):
            return
        if intno == 0x21:
            return self._dos()
        if intno == 0x20:
            self.finished = "INT 20h terminate"
            uc.emu_stop()
            return
        if intno == 0x10:
            return self._bios_video()
        if intno == 0x16:
            return self._bios_kbd()
        if intno == 0x1A:
            self._set(UC_X86_REG_CX, 0)
            self._set(UC_X86_REG_DX, self.int_counts[0x1A] * 3)
            self._set(UC_X86_REG_AX, 0)
            return
        if intno == 0x11:
            self._set(UC_X86_REG_AX, 0x0021)
            return
        if intno == 0x12:
            self._set(UC_X86_REG_AX, 640)
            return
        if intno == 0x33:
            return self._mouse()
        self._note(f"unhandled INT {intno:02x}h AX={self._reg(UC_X86_REG_AX):04x}")

    def _bios_video(self):
        ax = self._reg(UC_X86_REG_AX)
        ah, al = ax >> 8, ax & 0xFF
        if ah == 0x00:
            self.video_modes.append(al)
            self._note(f"INT 10h set video mode {al:#04x}")
            return
        if ah in (0x0C, 0x0D):
            return self._bios_pixel(ah, al)
        return

    def _bios_pixel(self, ah, al):
        """INT 10h AH=0Ch/0Dh - one pixel, CX=x, DX=y.

        Popcorn's menu draws its bouncing kernels a pixel at a time through
        the BIOS: six hundred thousand of these calls in a minute of menu.
        Bit 7 of AL means XOR, which is how a kernel erases itself without
        knowing what it was covering.
        """
        x = self._reg(UC_X86_REG_CX)
        y = self._reg(UC_X86_REG_DX)
        mode = self.video_modes[-1] if self.video_modes else 0x05
        if mode in (0x04, 0x05):
            w, bpp = 320, 2
        elif mode == 0x06:
            w, bpp = 640, 1
        else:
            return                          # text mode: nothing to plot
        if x >= w or y >= 200:
            return
        off = (0x2000 if y & 1 else 0) + (y >> 1) * 80 + (x * bpp) // 8
        addr = 0xB8000 + off
        cur = self.uc.mem_read(addr, 1)[0]
        per = 8 // bpp
        shift = (per - 1 - (x % per)) * bpp
        mask = ((1 << bpp) - 1) << shift
        if ah == 0x0D:
            self._set(UC_X86_REG_AX, ((self._reg(UC_X86_REG_AX) & 0xFF00)
                                      | ((cur & mask) >> shift)))
            return
        val = (al & ((1 << bpp) - 1)) << shift
        new = (cur ^ val) if (al & 0x80) else ((cur & ~mask) | val)
        self.uc.mem_write(addr, bytes([new & 0xFF]))
        return

    def _mouse(self):
        """Minimal INT 33h driver. Ducks refuses to start without one."""
        ax = self._reg(UC_X86_REG_AX)
        self.mouse_calls[ax] += 1
        n = sum(self.mouse_calls.values())
        if ax == 0x0000:                      # reset / detect
            self._set(UC_X86_REG_AX, 0xFFFF)  # driver installed
            self._set(UC_X86_REG_BX, 3)       # three buttons (v1.2 supports mid)
            return
        if ax == 0x0003:                      # get position and button state
            # Drift the pointer and hold the left button down so menus advance.
            self.mouse_x = (self.mouse_x + 8) % 640
            self.mouse_y = (self.mouse_y + 3) % 200
            self._set(UC_X86_REG_CX, self.mouse_x)
            self._set(UC_X86_REG_DX, self.mouse_y)
            self._set(UC_X86_REG_BX, 1 if (n // 8) % 2 else 0)
            return
        if ax in (0x0005, 0x0006):            # button press/release counts
            self._set(UC_X86_REG_AX, 1)
            self._set(UC_X86_REG_BX, 1)
            self._set(UC_X86_REG_CX, self.mouse_x)
            self._set(UC_X86_REG_DX, self.mouse_y)
            return
        if ax == 0x000B:                      # read relative motion
            self._set(UC_X86_REG_CX, 4)
            self._set(UC_X86_REG_DX, 2)
            return
        # show/hide cursor, set range, event handler, etc: accept silently.
        return

    def _bios_kbd(self):
        ah = self._reg(UC_X86_REG_AX) >> 8
        f = self.uc.reg_read(UC_X86_REG_EFLAGS)
        if ah in (0x01, 0x11):
            # Always report "a key is waiting" so title screens advance.
            self._set(UC_X86_REG_AX, 0x3920)
            self.uc.reg_write(UC_X86_REG_EFLAGS, f & ~0x40)   # ZF=0
            return
        if ah in (0x00, 0x10):
            self._set(UC_X86_REG_AX, 0x3920)                  # space
            return
        if ah == 0x02:
            self._set(UC_X86_REG_AX, 0)
            return

    # ------------------------------------------------------------------- DOS
    def _dos(self):
        ax = self._reg(UC_X86_REG_AX)
        ah, al = ax >> 8, ax & 0xFF
        ds = self._reg(UC_X86_REG_DS)
        dx = self._reg(UC_X86_REG_DX)
        bx = self._reg(UC_X86_REG_BX)
        cx = self._reg(UC_X86_REG_CX)
        self.dos_counts[ah] += 1
        self._cf(False)

        if ah == 0x30:
            self._set(UC_X86_REG_AX, 0x0005)
            self._set(UC_X86_REG_BX, 0)
            return
        if ah == 0x25:
            self.uc.mem_write(al * 4, struct.pack("<HH", dx, ds))
            self.hooked_vectors[al] = (ds, dx)
            self._note(f"INT 21h set vector {al:02x}h -> {ds:04x}:{dx:04x}")
            return
        if ah == 0x35:
            seg, off = self._ivt(al)
            self._set(UC_X86_REG_BX, off)
            self.uc.reg_write(UC_X86_REG_ES, seg)
            return
        if ah == 0x19:
            self._set(UC_X86_REG_AX, 2)          # drive C:
            return
        if ah == 0x1A:
            self.dta = (ds, dx)
            return
        if ah == 0x2F:
            self.uc.reg_write(UC_X86_REG_ES, self.dta[0])
            self._set(UC_X86_REG_BX, self.dta[1])
            return
        if ah == 0x2C:
            n = self.int_counts[0x21]
            self._set(UC_X86_REG_CX, 0x0C00)
            self._set(UC_X86_REG_DX, (n // 100) % 60 << 8)
            return
        if ah == 0x2A:
            self._set(UC_X86_REG_CX, 2000)
            self._set(UC_X86_REG_DX, 0x0B02)
            self._set(UC_X86_REG_AX, 4)
            return
        if ah == 0x36:
            self._set(UC_X86_REG_AX, 8)
            self._set(UC_X86_REG_BX, 20000)
            self._set(UC_X86_REG_CX, 512)
            self._set(UC_X86_REG_DX, 40000)
            return
        if ah in (0x48,):
            self._set(UC_X86_REG_AX, 0x8000)
            self._set(UC_X86_REG_BX, 0x1000)
            return
        if ah == 0x43:
            # Get/set file attributes. Blindly reporting success told the
            # runtime that a save slot already existed, so its open() never
            # took the create path and fopen("wb") failed. It must answer
            # honestly about existence, including for overlay files.
            name = self._str(ds, dx)
            key = name.replace("/", "\\").upper()
            if (ax & 0xFF) == 0:
                exists = key in self.overlay or os.path.isfile(host_path(name))
                if exists:
                    self._set(UC_X86_REG_CX, 0x20)      # archive bit
                    self._cf(False)
                else:
                    self._fop(f"GETATTR {name!r} -> NOT FOUND")
                    self._cf(True)
                    self._set(UC_X86_REG_AX, 2)         # ENOENT
            else:
                self._cf(False)                         # set attrs: accept
            return
        # 0x44 was in this list, which is why the answer below never ran: an
        # accepted-and-ignored call leaves DX holding whatever it held before.
        if ah in (0x49, 0x4A, 0x33, 0x38, 0x0B, 0x62):
            if ah == 0x62:
                self._set(UC_X86_REG_BX, PSP_SEG)
            if ah == 0x0B:
                self._set(UC_X86_REG_AX, 0)
            return
        if ah == 0x47:
            self.uc.mem_write(self._reg(UC_X86_REG_DS) * 16 +
                              self._reg(UC_X86_REG_SI), b"\x00")
            return

        # ---- file services ----
        if ah in (0x3D, 0x3C, 0x5B):
            name = self._str(ds, dx)
            hp = host_path(name)
            creating = ah in (0x3C, 0x5B)
            key = name.replace("/", "\\").upper()
            if creating:
                self._fop(f"CREATE {name!r} -> overlay")
                self.overlay[key] = bytearray()
                h = Handle(name, b"", True)
                h.key = key
                self.files_written.setdefault(name, 0)
            else:
                if key in self.overlay:
                    # Served from the overlay so saved games can be loaded back
                    # within a session, without ever touching the real
                    # directory. A save that cannot be re-read is not a save.
                    blob = bytes(self.overlay[key])
                    self._fop(f"OPEN {name!r} -> overlay ({len(blob)} bytes)")
                    h = Handle(name, blob, True)
                    h.key = key
                elif os.path.isfile(hp):
                    with open(hp, "rb") as f:       # READ-ONLY
                        blob = f.read()
                    self._fop(f"OPEN {name!r} -> host ({len(blob)} bytes)")
                    h = Handle(name, blob, False)
                    self.files_read[name] = len(blob)
                else:
                    self._fop(f"OPEN {name!r} -> NOT FOUND")
                    self.files_missing.append(name)
                    self._cf(True)
                    self._set(UC_X86_REG_AX, 2)
                    return
            # Allocate the LOWEST free handle, as real DOS does. Handing out
            # ever-increasing numbers eventually exceeds the runtime's file
            # table (Borland validates every fd against [0x2f6c], typically 20)
            # and then fopen returns NULL - which looked like a save failure
            # only after enough levels had been loaded to burn through the
            # numbers.
            hn = next((n for n in range(5, 20) if n not in self.handles), None)
            if hn is None:
                self._fop(f"OPEN {name!r} -> NO FREE HANDLE")
                self._cf(True)
                self._set(UC_X86_REG_AX, 4)      # too many open files
                return
            self.handles[hn] = h
            self._fop(f"  -> handle {hn} ({len(self.handles)} open)")
            self._set(UC_X86_REG_AX, hn)
            return
        if ah == 0x3E:
            h = self.handles.pop(bx, None)
            if h is not None and getattr(h, "key", None):
                self.overlay[h.key] = bytearray(h.data)
                self._fop(f"CLOSE {h.path!r} -> overlay {len(h.data)} bytes")
            return
        if ah == 0x3F:
            h = self.handles.get(bx)
            if h is None:
                self._cf(True)
                self._set(UC_X86_REG_AX, 6)
                return
            chunk = h.data[h.pos:h.pos + cx]
            self.uc.mem_write(ds * 16 + dx, bytes(chunk))
            h.pos += len(chunk)
            self._set(UC_X86_REG_AX, len(chunk))
            return
        if ah == 0x40:
            if bx in (1, 2):
                self.stdout += self._rd(ds, dx, cx)
            else:
                h = self.handles.get(bx)
                nm = h.path if h else f"handle {bx}"
                self.files_written[nm] = self.files_written.get(nm, 0) + cx
                if h is not None and cx == 0:
                    # A zero-length DOS write truncates the file at the current
                    # position. The runtime uses it to empty a save slot before
                    # rewriting it, and ignoring it looked harmless only because
                    # the rewrite usually covers the whole file - but saves are
                    # not a fixed size (61 to 66 bytes observed), so writing a
                    # shorter save over a longer one left the old tail behind.
                    if len(h.data) > h.pos:
                        self._fop(f"TRUNCATE {h.path!r} {len(h.data)} -> {h.pos}")
                        del h.data[h.pos:]
                        h.written += 1      # dirty, so an abnormal exit flushes
                        if getattr(h, "key", None):
                            self.overlay[h.key] = bytearray(h.data)
                    self._set(UC_X86_REG_AX, 0)
                    return
                if h is not None:
                    data = self._rd(ds, dx, cx)
                    if h.pos + cx > len(h.data):
                        h.data.extend(b"\x00" * (h.pos + cx - len(h.data)))
                    h.data[h.pos:h.pos + cx] = data
                    h.pos += cx
                    h.written += cx
                    if getattr(h, "key", None):
                        self.overlay[h.key] = bytearray(h.data)
                    self._fop(f"WRITE {h.path!r} +{cx} at {h.pos - cx} "
                              f"(total {len(h.data)})")
            self._set(UC_X86_REG_AX, cx)
            return
        if ah == 0x42:
            h = self.handles.get(bx)
            if h is None:
                self._cf(True)
                return
            off = (cx << 16) | dx
            if off >= 1 << 31:
                off -= 1 << 32
            h.pos = {0: off, 1: h.pos + off, 2: len(h.data) + off}.get(al, off)
            h.pos = max(0, min(h.pos, len(h.data)))
            self._set(UC_X86_REG_AX, h.pos & 0xFFFF)
            self._set(UC_X86_REG_DX, (h.pos >> 16) & 0xFFFF)
            return
        if ah == 0x44 and al == 0x00:
            # IOCTL get-device-info. Ignoring this left the game reading
            # whatever happened to be in DX, and it uses the answer to decide
            # how a stream is buffered: told stdout was a file, it buffered the
            # startup messages and never flushed them, so nothing was written
            # and the BIOS cursor never moved. The game positions the text it
            # pokes into 0xb8000 itself by asking INT 10h 03h where the cursor
            # is, which is why the visible symptom was its 80-column rules
            # starting mid-line and running over the messages.
            self._set(UC_X86_REG_DX, self.device_info(bx))
            self._set(UC_X86_REG_AX, self.device_info(bx))
            return
        if ah == 0x41:
            name = self._str(ds, dx)
            self.overlay.pop(name.replace("/", "\\").upper(), None)
            self._fop(f"DELETE {name!r}")
            return
        if ah in (0x4E, 0x4F):
            self._cf(True)
            self._set(UC_X86_REG_AX, 18)      # no more files
            return
        if ah in (0x4C, 0x00):
            self.finished = f"INT 21h AH={ah:02x}h exit code {al}"
            self.uc.emu_stop()
            return
        if ah == 0x09:
            s = self._rd(ds, dx, 256).split(b"$")[0]
            self.stdout += s
            return
        if ah == 0x02:
            self.stdout.append(self._reg(UC_X86_REG_DX) & 0xFF)
            return
        if ah in (0x01, 0x06, 0x07, 0x08):    # console input -> supply a space
            self._set(UC_X86_REG_AX, (ax & 0xFF00) | 0x20)
            return
        self._fop(f"UNHANDLED INT 21h AH={ah:02x}h "
                  f"({DOS_FN.get(ah, '?')}) AX={ax:04x} -- may be why an "
                  f"operation failed")

    # ------------------------------------------------------------------- run
    def run(self):
        try:
            self.uc.emu_start(self.start, 0, count=self.max_insns)
        except UcError as e:
            self.finished = self.finished or (
                f"UcError {e} at {self._reg(UC_X86_REG_CS):04x}:"
                f"{self._reg(UC_X86_REG_IP):04x}")
        if self.finished is None:
            self.finished = f"instruction budget ({self.max_insns}) exhausted"
        return self.finished


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=UNPACKED)
    ap.add_argument("--cmdline", default="",
                    help="DOS command tail, e.g. a level file base name")
    ap.add_argument("--blaster", action="store_true")
    ap.add_argument("--max-insns", type=int, default=80_000_000)
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    print(f"=== tracing {args.exe} "
          f"(BLASTER {'set' if args.blaster else 'unset'}) ===")
    print(f"    host filesystem is READ-ONLY; writes are intercepted\n")
    m = DosMachine(args.exe, blaster=args.blaster, verbose=not args.quiet,
                   max_insns=args.max_insns, cmdline=args.cmdline)
    reason = m.run()

    print(f"\n=== stopped: {reason} ===")

    if m.stdout:
        print("\n=== program console output ===")
        txt = m.stdout.decode("latin1")
        for line in txt.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
            print(f"  | {line}")

    print("\n=== interrupts used ===")
    for n, c in sorted(m.int_counts.items()):
        g = m.guest_dispatch.get(n, 0)
        tag = f"  -> {g} dispatched to the program's own handler" if g else ""
        print(f"  INT {n:02x}h  x{c}{tag}")

    print("\n=== interrupt vectors the program hooked ===")
    for v, (seg, off) in sorted(m.hooked_vectors.items()):
        note = {0x00: "divide by zero", 0x02: "NMI", 0x04: "overflow",
                0x08: "timer (IRQ0)", 0x09: "keyboard (IRQ1)",
                0x1B: "Ctrl-Break", 0x1C: "timer tick",
                0x23: "Ctrl-C", 0x24: "critical error"}.get(v, "")
        if 0x34 <= v <= 0x3E:
            note = "Borland 80x87 FP emulation"
        print(f"  INT {v:02x}h -> {seg:04x}:{off:04x}  {note}")

    print("\n=== INT 21h functions used ===")
    for ah, c in sorted(m.dos_counts.items()):
        print(f"  AH={ah:02x}h x{c:<6} {DOS_FN.get(ah, '?')}")

    print("\n=== files READ ===")
    for k, v in sorted(m.files_read.items()) or [("(none)", 0)]:
        print(f"  {k!r}  {v} bytes")
    print("=== files the program tried to WRITE (intercepted) ===")
    for k, v in sorted(m.files_written.items()) or [("(none)", 0)]:
        print(f"  {k!r}  {v} bytes")
    print("=== files NOT FOUND ===")
    for k in m.files_missing or ["(none)"]:
        print(f"  {k!r}")

    print("\n=== port I/O (top 20) ===")
    for p, c in m.port_out.most_common(20):
        print(f"  OUT {p:#06x} x{c}")
    for p, c in m.port_in.most_common(10):
        print(f"  IN  {p:#06x} x{c}")
    if m.video_modes:
        print(f"\n=== video modes set: "
              f"{[hex(v) for v in m.video_modes]} ===")


if __name__ == "__main__":
    sys.exit(main())
