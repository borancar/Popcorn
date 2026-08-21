#!/usr/bin/env python3
"""
Sound Blaster (DSP + 8237 DMA) model for the Ducks emulator.

Enough of a card for a DOS game's autodetect and playback path:

  * DSP reset handshake on 0x226 -> 0xaa readable at 0x22a
  * the DSP command set a game needs: speaker on/off, time constant / sample
    rate, DMA block size, single-cycle and auto-init 8-bit output, direct DAC,
    version query, IRQ test
  * the DMA controller registers for channel 1, so the card knows which guest
    memory to play from
  * IRQ on block completion, acknowledged by reading 0x22e

Audio is pulled from guest memory on a wall-clock schedule and appended to a PCM
buffer, which the host can both play and dump to a WAV for verification.

Kept separate from emulation.py so the sound work can be reviewed on its own.
"""
import struct
from collections import Counter

# DSP commands (offsets from the base port, written to base+0xc).
CMD_DIRECT_DAC = 0x10
CMD_DMA_8BIT = 0x14
CMD_DMA_2BIT = 0x16
CMD_DMA_ADPCM = 0x17
CMD_AUTOINIT_8BIT = 0x1C
CMD_AUTOINIT_ADPCM = 0x1F
CMD_MIDI_WRITE = 0x38
CMD_SET_TIME_CONSTANT = 0x40
CMD_SET_SAMPLE_RATE = 0x41
CMD_SET_BLOCK_SIZE = 0x48
CMD_HALT_DMA = 0xD0
CMD_SPEAKER_ON = 0xD1
CMD_SPEAKER_OFF = 0xD3
CMD_SPEAKER_STATUS = 0xD8
CMD_CONTINUE_DMA = 0xD4
CMD_STOP_AUTOINIT = 0xDA
CMD_DSP_VERSION = 0xE1
CMD_DSP_ID = 0xE0
CMD_TRIGGER_IRQ = 0xF2

# How many parameter bytes follow each command.
CMD_PARAMS = {
    CMD_DIRECT_DAC: 1,
    CMD_DMA_8BIT: 2,
    CMD_DMA_2BIT: 2,
    CMD_DMA_ADPCM: 2,
    CMD_AUTOINIT_8BIT: 0,
    CMD_AUTOINIT_ADPCM: 0,
    CMD_SET_TIME_CONSTANT: 1,
    CMD_SET_SAMPLE_RATE: 2,
    CMD_SET_BLOCK_SIZE: 2,
    CMD_MIDI_WRITE: 1,
    CMD_DSP_ID: 1,
    0x80: 2,                      # silence period
}

CMD_NAMES = {
    CMD_DIRECT_DAC: "direct DAC", CMD_DMA_8BIT: "single-cycle 8-bit DMA out",
    CMD_AUTOINIT_8BIT: "auto-init 8-bit DMA out",
    CMD_SET_TIME_CONSTANT: "set time constant",
    CMD_SET_SAMPLE_RATE: "set sample rate",
    CMD_SET_BLOCK_SIZE: "set DMA block size", CMD_HALT_DMA: "halt DMA",
    CMD_SPEAKER_ON: "speaker on", CMD_SPEAKER_OFF: "speaker off",
    CMD_SPEAKER_STATUS: "speaker status", CMD_CONTINUE_DMA: "continue DMA",
    CMD_STOP_AUTOINIT: "stop auto-init", CMD_DSP_VERSION: "get DSP version",
    CMD_DSP_ID: "DSP identification", CMD_TRIGGER_IRQ: "trigger IRQ (IRQ test)",
    0x80: "silence period",
}


class SoundBlaster:
    def __init__(self, base=0x220, irq=5, dma=1, version=(2, 1), log=None,
                 verbose=True):
        self.base = base
        self.irq = irq
        self.dma_channel = dma
        self.version = version
        self._log = log or (lambda s: None)
        self.verbose = verbose

        # DSP state
        self.reset_stage = 0
        self.out_queue = []            # bytes readable at base+0xa
        self.pending_cmd = None
        self.pending_args = []
        self.speaker_on = False
        self.time_constant = 0
        self.sample_rate = 11025
        self.block_size = 0
        self.dma_active = False
        self.autoinit = False
        self.remaining = 0

        # 8237 DMA controller state for our channel
        self.dma_flipflop = 0
        self.dma_addr = 0
        self.dma_count = 0
        self.dma_page = 0
        self.dma_mode = 0
        self.dma_masked = True
        self.cur_addr = 0
        self.cur_remaining = 0
        self.dma_len = 0
        self.irq_period = 0
        self.irq_countdown = 0

        # IRQ / PIC
        self.irq_pending = False
        self.pic_mask = 0xFF           # everything masked until the game says so

        # Output
        self.pcm = bytearray()
        self.direct_pcm = bytearray()
        self.cmd_counts = Counter()
        self.bytes_played = 0
        self._frac = 0.0
        self.blocks_completed = 0
        self.read_failures = 0
        self.sample_values = Counter()
        self.buf_writes = 0
        self.buf_write_values = Counter()
        self.saw_signal = False        # has anything but 0x80 (silence) landed?

    # ------------------------------------------------------------------ log
    def note(self, msg):
        self._log(f"  [sb] {msg}")

    # --------------------------------------------------------------- ports
    def owns(self, port):
        return (self.base <= port <= self.base + 0x0F
                or port in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F, 0x81, 0x82,
                            0x83, 0x87)
                or port in (0x20, 0x21))

    def write(self, port, value):
        v = value & 0xFF
        if self.base <= port <= self.base + 0x0F:
            return self._dsp_write(port - self.base, v)
        if port in (0x20, 0x21):
            if port == 0x21:
                self.pic_mask = v
            return True
        return self._dma_write(port, v)

    def read(self, port):
        if self.base <= port <= self.base + 0x0F:
            return self._dsp_read(port - self.base)
        if port == 0x21:
            return self.pic_mask
        return self._dma_read(port)

    def _dma_read(self, port):
        """Current address / current count read-back for our DMA channel.

        A sound check confirms the card is really playing by watching these
        count down. Returning a constant makes it look like nothing is being
        consumed, so the check fails and the game concludes it has no working
        audio - which disables its audio menu and leaves the mixer silent.
        Reads are latched low byte first, sharing the byte-pointer flip-flop.
        """
        ch = self.dma_channel
        if port == ch * 2:
            val = self.cur_addr & 0xFFFF
        elif port == ch * 2 + 1:
            val = max(0, self.cur_remaining - 1) & 0xFFFF
        else:
            return None
        byte = val & 0xFF if self.dma_flipflop == 0 else (val >> 8) & 0xFF
        self.dma_flipflop ^= 1
        return byte

    # ----------------------------------------------------------------- DSP
    def _dsp_write(self, off, v):
        if off == 0x06:                        # DSP reset
            if v & 1:
                self.reset_stage = 1
            elif self.reset_stage == 1:
                # Reset completed: the card answers 0xaa.
                self.reset_stage = 0
                self.out_queue = [0xAA]
                self.dma_active = False
                self.autoinit = False
                self.pending_cmd = None
                self.pending_args = []
                if self.verbose:
                    self.note("DSP reset -> 0xaa")
            return True
        if off in (0x0C, 0x0D):                # command / data write
            self._dsp_command_byte(v)
            return True
        if off in (0x00, 0x01, 0x02, 0x03, 0x08, 0x09):
            return True                        # FM/AdLib registers: ignored
        if off in (0x04, 0x05):
            return True                        # mixer: ignored
        return True

    def _dsp_command_byte(self, v):
        if self.pending_cmd is not None:
            self.pending_args.append(v)
            need = CMD_PARAMS.get(self.pending_cmd, 0)
            if len(self.pending_args) >= need:
                cmd, args = self.pending_cmd, self.pending_args
                self.pending_cmd, self.pending_args = None, []
                self._exec(cmd, args)
            return
        need = CMD_PARAMS.get(v, 0)
        if need:
            self.pending_cmd, self.pending_args = v, []
        else:
            self._exec(v, [])

    def _exec(self, cmd, args):
        self.cmd_counts[cmd] += 1
        name = CMD_NAMES.get(cmd, f"unknown {cmd:#04x}")
        first = self.cmd_counts[cmd] == 1

        if cmd == CMD_SPEAKER_ON:
            self.speaker_on = True
        elif cmd == CMD_SPEAKER_OFF:
            self.speaker_on = False
        elif cmd == CMD_SPEAKER_STATUS:
            self.out_queue.append(0xFF if self.speaker_on else 0x00)
        elif cmd == CMD_SET_TIME_CONSTANT:
            self.time_constant = args[0]
            self.sample_rate = int(1000000 / (256 - args[0])) if args[0] < 256 \
                else 11025
            self.note(f"sample rate {self.sample_rate} Hz "
                      f"(time constant {args[0]:#04x})")
        elif cmd == CMD_SET_SAMPLE_RATE:
            self.sample_rate = (args[0] << 8) | args[1]
            self.note(f"sample rate {self.sample_rate} Hz (direct)")
        elif cmd == CMD_SET_BLOCK_SIZE:
            self.block_size = ((args[1] << 8) | args[0]) + 1
            self.note(f"DMA block size {self.block_size}")
        elif cmd in (CMD_DMA_8BIT, CMD_DMA_2BIT, CMD_DMA_ADPCM):
            self.block_size = ((args[1] << 8) | args[0]) + 1
            self._start_dma(autoinit=False)
        elif cmd in (CMD_AUTOINIT_8BIT, CMD_AUTOINIT_ADPCM):
            self._start_dma(autoinit=True)
        elif cmd == CMD_HALT_DMA:
            self.dma_active = False
        elif cmd == CMD_CONTINUE_DMA:
            self.dma_active = True
        elif cmd == CMD_STOP_AUTOINIT:
            self.autoinit = False
            self.dma_active = False
        elif cmd == CMD_DIRECT_DAC:
            self.direct_pcm.append(args[0])
            self.pcm.append(args[0])
        elif cmd == CMD_DSP_VERSION:
            self.out_queue.extend(self.version)
        elif cmd == CMD_DSP_ID:
            self.out_queue.append((~args[0]) & 0xFF)
        elif cmd == CMD_TRIGGER_IRQ:
            self.out_queue.append(0xAA)
            self.irq_pending = True
            self.note("IRQ test requested -> raising IRQ")
        elif cmd == 0x80:
            n = ((args[1] << 8) | args[0]) + 1
            self.pcm.extend(b"\x80" * min(n, 65536))

        if first and cmd not in (CMD_DIRECT_DAC,):
            self.note(f"command {cmd:#04x} ({name})"
                      + (f" args={[hex(a) for a in args]}" if args else ""))

    def _dsp_read(self, off):
        if off == 0x0A:                        # read data
            return self.out_queue.pop(0) if self.out_queue else 0x00
        if off == 0x0C:                        # write-buffer status: never busy
            return 0x00
        if off == 0x0E:                        # read-buffer status; also ACKs IRQ
            self.irq_pending = False
            return 0x80 if self.out_queue else 0x00
        if off == 0x0F:                        # SB16 16-bit IRQ ack
            self.irq_pending = False
            return 0x00
        if off in (0x00, 0x08):                # FM status: no timers expired
            return 0x00
        return 0x00

    # ----------------------------------------------------------------- DMA
    def _dma_write(self, port, v):
        ch = self.dma_channel
        if port == 0x0C:                       # clear byte-pointer flip-flop
            self.dma_flipflop = 0
            return True
        if port == ch * 2:                     # base address
            if self.dma_flipflop == 0:
                self.dma_addr = (self.dma_addr & 0xFF00) | v
            else:
                self.dma_addr = (self.dma_addr & 0x00FF) | (v << 8)
            self.dma_flipflop ^= 1
            return True
        if port == ch * 2 + 1:                 # base count
            if self.dma_flipflop == 0:
                self.dma_count = (self.dma_count & 0xFF00) | v
            else:
                self.dma_count = (self.dma_count & 0x00FF) | (v << 8)
            self.dma_flipflop ^= 1
            return True
        if port == 0x0A:                       # single channel mask
            if (v & 0x03) == ch:
                self.dma_masked = bool(v & 0x04)
            return True
        if port == 0x0B:                       # mode register
            if (v & 0x03) == ch:
                self.dma_mode = v
            return True
        if port == {0: 0x87, 1: 0x83, 2: 0x81, 3: 0x82}.get(ch, 0x83):
            self.dma_page = v                  # page (A16-A23)
            return True
        return True

    def _start_dma(self, autoinit):
        self.autoinit = autoinit
        self.dma_active = True
        self.cur_addr = (self.dma_page << 16) | self.dma_addr
        # Two independent periods, which is the whole point of the auto-init
        # double buffer: the DMA controller wraps after count+1 bytes, while the
        # DSP raises an interrupt every block_size bytes so the game can refill
        # the half that is not currently playing. Conflating them makes the game
        # refill at half the rate it needs to.
        self.dma_len = (self.dma_count + 1) if self.dma_count else \
            (self.block_size or 512)
        self.irq_period = self.block_size or self.dma_len
        if not autoinit:
            self.dma_len = min(self.dma_len, self.irq_period)
        self.cur_remaining = self.dma_len
        self.irq_countdown = self.irq_period
        self.note(f"{'auto-init' if autoinit else 'single-cycle'} DMA start: "
                  f"addr={self.cur_addr:#07x} dma_len={self.dma_len} "
                  f"irq every {self.irq_period} bytes "
                  f"rate={self.sample_rate}Hz "
                  f"speaker={'on' if self.speaker_on else 'off'}")

    # ---------------------------------------------------------------- clock
    def tick(self, uc, dt):
        """Advance playback by dt seconds; returns True if an IRQ should fire."""
        if not self.dma_active or self.dma_masked and False:
            return False
        want = self.sample_rate * dt + self._frac
        n = int(want)
        self._frac = want - n
        if n <= 0:
            return False
        fired = False
        while n > 0 and self.dma_active:
            # Consume up to whichever comes first: the end of the DMA buffer or
            # the next interrupt boundary. These are different periods - the
            # controller wraps every dma_len bytes while the DSP interrupts
            # every block_size - and treating them as one means the game is
            # told to refill only half as often as it should, so half the
            # buffer replays stale audio.
            take = min(n, self.cur_remaining, self.irq_countdown)
            if take > 0:
                try:
                    chunk = bytes(uc.mem_read(self.cur_addr, take))
                except Exception as e:
                    # Do not silently substitute silence here: that hides a bad
                    # DMA address as "the game produced no sound".
                    self.read_failures += 1
                    if self.read_failures == 1:
                        self.note(f"!! cannot read DMA buffer at "
                                  f"{self.cur_addr:#07x}: {e}")
                    chunk = b"\x80" * take
                else:
                    self.sample_values.update(chunk[:64])
                self.pcm.extend(chunk)
                self.bytes_played += take
                self.cur_addr += take
                self.cur_remaining -= take
                self.irq_countdown -= take
                n -= take
            if self.irq_countdown <= 0:
                self.blocks_completed += 1
                fired = True
                self.irq_countdown = self.irq_period
            if self.cur_remaining <= 0:
                if self.autoinit:
                    self.cur_addr = (self.dma_page << 16) | self.dma_addr
                    self.cur_remaining = self.dma_len
                else:
                    self.dma_active = False
                    break
        if fired:
            self.irq_pending = True
        return fired

    def irq_enabled(self):
        return not (self.pic_mask >> self.irq) & 1

    # --------------------------------------------------------------- output
    def write_wav(self, path):
        if not self.pcm:
            return None
        import wave
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(1)
            w.setframerate(max(4000, self.sample_rate))
            w.writeframes(bytes(self.pcm))
        return path

    def summary(self):
        return {
            "dsp_commands": {f"{k:#04x}": v for k, v in
                             sorted(self.cmd_counts.items())},
            "command_names": [CMD_NAMES.get(k, hex(k))
                              for k in sorted(self.cmd_counts)],
            "sample_rate": self.sample_rate,
            "time_constant": f"{self.time_constant:#04x}",
            "block_size": self.block_size,
            "speaker_on": self.speaker_on,
            "autoinit": self.autoinit,
            "dma_addr": f"{(self.dma_page << 16) | self.dma_addr:#07x}",
            "dma_count": self.dma_count,
            "dma_masked": self.dma_masked,
            "blocks_completed": self.blocks_completed,
            "pcm_bytes": len(self.pcm),
            "dma_read_failures": self.read_failures,
            "distinct_sample_values_seen": len(self.sample_values),
            "top_sample_values": [f"{v:#04x}:{n}" for v, n in
                                  self.sample_values.most_common(5)],
            "guest_writes_to_dma_buffer": self.buf_writes,
            "distinct_values_written": len(self.buf_write_values),
            "top_values_written": [f"{v:#04x}:{n}" for v, n in
                                   self.buf_write_values.most_common(5)],
            "direct_dac_bytes": len(self.direct_pcm),
            "irq_enabled": self.irq_enabled(),
            "pic_mask": f"{self.pic_mask:#04x}",
        }
