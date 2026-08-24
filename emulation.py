#!/usr/bin/env python3
"""
Popcorn under the emulator: the shared machine, pointed at this game.

The emulator itself is not here. It lives in `dos_emulator`, shared with the
other reconstruction projects, and is pinned to a commit in requirements.txt -
every measurement in this repository is taken against it, so which version
produced a number is part of the number.

What is left here is the part that is *about Popcorn*: where its files are,
where its code segment starts, and a command line that defaults to it, so the
documented `python emulation.py --scale 3` still means what it always did.

    python emulation.py --scale 3
    python emulation.py --scale 3 --cmdline poptab
    python emulation.py --shots 4 --shot-every 4 --shot-dir debug

Everything the tools here import from the emulator comes through this module,
so there is one place to look when the shared code moves under them.
"""
import os
import sys

from dos_emulator import (
    DosMachine,
    VgaDos,
    KEYMAP,
    make_surface,
    set_game_dir,
    shift_ascii,
    capture,
    IPS_8086_8MHZ,
    CP437,
    CGA4,
    CGA16,
)
from dos_emulator import emulator as _emu

# The game lives under the repository, in popcorn/, and is never part of it.
# Anchor on this file rather than on the working directory, so the tools can be
# run from anywhere; POPCORN_GAME_DIR overrides it for a different layout.
HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.abspath(os.environ.get(
    "POPCORN_GAME_DIR", os.path.join(HERE, "popcorn")))
UNPACKED = os.path.join(HERE, "popcorn.unpacked.exe")

# Where Popcorn's one code segment starts in the load image. The tools talk in
# image offsets - `1ac2:1c3f` is image 0x1bd20 - and this is what turns one into
# the other.
GAME_CODE = 0x1AC20

set_game_dir(GAME_DIR)
_emu.GAME_CODE = GAME_CODE

__all__ = [
    "DosMachine", "VgaDos", "KEYMAP", "make_surface", "shift_ascii", "capture",
    "IPS_8086_8MHZ", "CP437", "CGA4", "CGA16",
    "GAME_DIR", "UNPACKED", "GAME_CODE", "set_game_dir",
]


def main():
    """The shared CLI, with Popcorn's executable as the default argument.

    The upstream tool takes the program to run as its first argument. Here it
    is always the same program, and every documented invocation in this
    repository is flags-only - so a leading flag (or nothing at all) means
    Popcorn. Pass a path first to run something else through it, which is how
    POPGEN and POPSPEED get looked at.
    """
    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        sys.argv.insert(1, UNPACKED)
    if "--code-base" not in sys.argv:
        sys.argv += ["--code-base", hex(GAME_CODE)]
    return _emu.main()


if __name__ == "__main__":
    sys.exit(main())
