#!/usr/bin/env python3
"""
XMS (HIMEM.SYS) driver for the Ducks emulator.

Ducks keeps its sound samples in extended memory. Its sound check calls
INT 2Fh AX=4300h to detect XMS and refuses to enable audio without it -
printing "HIMEM.SYS not installed" and disabling the whole audio subsystem,
which is why the mixer produced nothing but silence.

Real mode cannot reach extended memory directly, so XMS is reached through a
far pointer obtained via INT 2Fh AX=4310h. We publish a three byte stub in
guest memory (INT 60h; RETF) and implement the API in Python behind it: the
guest's far call lands on the stub, the interrupt hook services the request,
and the RETF returns to the caller.

Extended memory blocks are backed by Python bytearrays rather than emulated
address space, since nothing in real mode may address them anyway.
"""
from collections import Counter

# XMS error codes
ERR_OK = 0x00
ERR_NOT_IMPLEMENTED = 0x80
ERR_NO_HANDLES = 0xA1
ERR_INVALID_HANDLE = 0xA2
ERR_INVALID_SRC_HANDLE = 0xA3
ERR_INVALID_SRC_OFFSET = 0xA4
ERR_INVALID_DST_HANDLE = 0xA5
ERR_INVALID_DST_OFFSET = 0xA6
ERR_INVALID_LENGTH = 0xA7
ERR_OUT_OF_MEMORY = 0xA0

FN_NAMES = {
    0x00: "get version", 0x01: "request HMA", 0x02: "release HMA",
    0x03: "global enable A20", 0x04: "global disable A20",
    0x05: "local enable A20", 0x06: "local disable A20", 0x07: "query A20",
    0x08: "query free extended memory", 0x09: "allocate EMB",
    0x0A: "free EMB", 0x0B: "move EMB", 0x0C: "lock EMB", 0x0D: "unlock EMB",
    0x0E: "get EMB handle info", 0x0F: "reallocate EMB",
    0x10: "request UMB", 0x11: "release UMB", 0x88: "query free (XMS 3.0)",
    0x89: "allocate EMB (XMS 3.0)",
}


class XMS:
    def __init__(self, total_kb=16384, log=None, verbose=True):
        self.total_kb = total_kb
        self.free_kb = total_kb
        self.handles = {}            # handle -> bytearray
        self.locks = {}              # handle -> lock count
        self.next_handle = 1
        self.a20 = 0
        self.calls = Counter()
        self._log = log or (lambda s: None)
        self.verbose = verbose
        self.moved_bytes = 0
        self.move_failures = 0

    def note(self, msg):
        self._log(f"  [xms] {msg}")

    # ------------------------------------------------------------------ API
    def dispatch(self, ah, regs, mem):
        """Service one XMS call.

        `regs` is a dict of the guest registers; return a dict of registers to
        write back. `mem` gives read/write access to guest memory for moves.
        """
        self.calls[ah] += 1
        first = self.calls[ah] == 1
        if first and self.verbose:
            self.note(f"function {ah:#04x} ({FN_NAMES.get(ah, '?')})")

        if ah == 0x00:                        # get version
            return {"ax": 0x0300, "bx": 0x0000, "dx": 0x0001}

        if ah in (0x03, 0x04, 0x05, 0x06):    # A20 control
            self.a20 = 1 if ah in (0x03, 0x05) else 0
            return {"ax": 1, "bx": ERR_OK}
        if ah == 0x07:                        # query A20
            return {"ax": self.a20, "bx": ERR_OK}

        if ah == 0x08:                        # query free extended memory
            # AX = largest free block (KB), DX = total free (KB)
            return {"ax": self.free_kb, "dx": self.free_kb, "bx": ERR_OK}

        if ah == 0x09:                        # allocate EMB, DX = KB
            kb = regs["dx"]
            if kb > self.free_kb:
                return {"ax": 0, "bx": ERR_OUT_OF_MEMORY}
            h = self.next_handle
            self.next_handle += 1
            self.handles[h] = bytearray(kb * 1024)
            self.locks[h] = 0
            self.free_kb -= kb
            if self.verbose:
                self.note(f"allocated handle {h}: {kb} KB "
                          f"({self.free_kb} KB free)")
            return {"ax": 1, "dx": h}

        if ah == 0x0A:                        # free EMB, DX = handle
            h = regs["dx"]
            if h not in self.handles:
                return {"ax": 0, "bx": ERR_INVALID_HANDLE}
            self.free_kb += len(self.handles.pop(h)) // 1024
            self.locks.pop(h, None)
            return {"ax": 1}

        if ah == 0x0B:                        # move EMB
            return self._move(regs, mem)

        if ah == 0x0C:                        # lock EMB -> return a fake linear
            h = regs["dx"]
            if h not in self.handles:
                return {"ax": 0, "bx": ERR_INVALID_HANDLE}
            self.locks[h] = self.locks.get(h, 0) + 1
            # Real mode cannot address it; hand back a plausible address.
            addr = 0x100000 + h * 0x10000
            return {"ax": 1, "bx": addr & 0xFFFF, "dx": (addr >> 16) & 0xFFFF}
        if ah == 0x0D:                        # unlock EMB
            h = regs["dx"]
            if h not in self.handles:
                return {"ax": 0, "bx": ERR_INVALID_HANDLE}
            self.locks[h] = max(0, self.locks.get(h, 0) - 1)
            return {"ax": 1}

        if ah == 0x0E:                        # get handle info
            h = regs["dx"]
            if h not in self.handles:
                return {"ax": 0, "bx": ERR_INVALID_HANDLE}
            return {"ax": 1, "bx": self.locks.get(h, 0) << 8 | 0,
                    "dx": len(self.handles[h]) // 1024}

        if ah == 0x0F:                        # reallocate EMB
            # BX = new size in KB, DX = handle. Getting these the wrong way
            # round makes every reallocation fail, so the block stays at its
            # initial zero size and every subsequent move is rejected.
            kb, h = regs["bx"], regs["dx"]
            if h not in self.handles:
                return {"ax": 0, "bx": ERR_INVALID_HANDLE}
            old = self.handles[h]
            new = bytearray(kb * 1024)
            keep = min(len(old), len(new))
            new[:keep] = old[:keep]
            self.free_kb += (len(old) - len(new)) // 1024
            self.handles[h] = new
            if self.verbose:
                self.note(f"handle {h} resized {len(old) // 1024} KB -> "
                          f"{kb} KB ({self.free_kb} KB free)")
            return {"ax": 1}

        if ah in (0x01, 0x02, 0x10, 0x11):    # HMA / UMB: politely decline
            return {"ax": 0, "bx": ERR_NOT_IMPLEMENTED}

        self.note(f"unimplemented function {ah:#04x}")
        return {"ax": 0, "bx": ERR_NOT_IMPLEMENTED}

    # ----------------------------------------------------------------- move
    def _move(self, regs, mem):
        """AH=0Bh: copy between conventional and extended memory.

        DS:SI points at:
            DWORD length
            WORD  src handle (0 = conventional, offset is a real-mode pointer)
            DWORD src offset
            WORD  dst handle
            DWORD dst offset
        """
        p = regs["ds"] * 16 + regs["si"]
        blob = mem.read(p, 16)
        length = int.from_bytes(blob[0:4], "little")
        src_h = int.from_bytes(blob[4:6], "little")
        src_o = int.from_bytes(blob[6:10], "little")
        dst_h = int.from_bytes(blob[10:12], "little")
        dst_o = int.from_bytes(blob[12:16], "little")

        def fail(code, why):
            # A silently failing move looks exactly like "the game produced no
            # sound", so say so - but only a few times, since a broken setup
            # fails every single transfer.
            self.move_failures += 1
            if self.move_failures <= 5:
                self.note(f"move FAILED ({why}): len={length} "
                          f"src=h{src_h}+{src_o:#x} dst=h{dst_h}+{dst_o:#x}")
            return {"ax": 0, "bx": code}

        if length & 1:
            return fail(ERR_INVALID_LENGTH, "odd length")

        # Read the source.
        if src_h == 0:
            lin = ((src_o >> 16) & 0xFFFF) * 16 + (src_o & 0xFFFF)
            data = mem.read(lin, length)
        else:
            buf = self.handles.get(src_h)
            if buf is None:
                return fail(ERR_INVALID_SRC_HANDLE, "bad src handle")
            if src_o + length > len(buf):
                return fail(ERR_INVALID_SRC_OFFSET, "src out of range")
            data = bytes(buf[src_o:src_o + length])

        # Write the destination.
        if dst_h == 0:
            lin = ((dst_o >> 16) & 0xFFFF) * 16 + (dst_o & 0xFFFF)
            mem.write(lin, data)
        else:
            buf = self.handles.get(dst_h)
            if buf is None:
                return fail(ERR_INVALID_DST_HANDLE, "bad dst handle")
            if dst_o + length > len(buf):
                return fail(ERR_INVALID_DST_OFFSET, "dst out of range")
            buf[dst_o:dst_o + length] = data

        self.moved_bytes += length
        return {"ax": 1, "bx": ERR_OK}

    # -------------------------------------------------------------- summary
    def summary(self):
        return {
            "calls": {f"{k:#04x}({FN_NAMES.get(k, '?')})": v
                      for k, v in sorted(self.calls.items())},
            "handles_open": len(self.handles),
            "allocated_kb": sum(len(b) for b in self.handles.values()) // 1024,
            "free_kb": self.free_kb,
            "bytes_moved": self.moved_bytes,
            "move_failures": self.move_failures,
        }
