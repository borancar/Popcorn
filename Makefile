# Popcorn, reconstructed. Needs SDL3 - not SDL2, they are not interchangeable.
#
#   Fedora            sudo dnf install gcc make pkgconf-pkg-config SDL3-devel
#   Debian / Ubuntu   sudo apt install build-essential pkg-config libsdl3-dev
#   Arch              sudo pacman -S base-devel sdl3
#   macOS             brew install sdl3
#
# The game's data is src/data.c - the levels, the sprites, the fonts and the
# scripts, written down as one initializer - so the port needs nothing on disk
# to run. src/exepack.c recovers the same image from a real POPCORN.EXE, which
# is what `popcorn-dev --dump-exe` and validate.py use to check that the two
# agree byte for byte.

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Isrc
CFLAGS  += $(shell pkg-config --cflags sdl3)
LDLIBS  += $(shell pkg-config --libs sdl3) -lm

# Two binaries over one set of objects.
#
#   popcorn      the game. Its command line is the original's - an optional
#                level file - and nothing else, because that command line is
#                part of what the port is.
#   popcorn-dev  the same game with the flags the harness drives it by:
#                --lockstep, --verify, --shot, --keys and the rest. Every
#                tool here runs this one.
# src/ is the game: the transcription and what it needs to run.
# tools/ is what exists to check it - the lockstep protocol, the bot, and the
# entry point that carries the flags for both.
#
# Both binaries link both, because the game's own code calls into them:
# sdl_io.c asks autoplay whether it is driving and game.c offers the extra
# sync points. They cost nothing when nothing has turned them on, and the
# alternative is #ifdefs through a transcription, which would make the port
# harder to read against the disassembly than it needs to be.
GAME   = src/data.o src/exepack.o src/sdl_io.o src/game.o src/stubs.o
CHECK  = tools/verify.o tools/lockstep.o tools/autoplay.o
COMMON = $(GAME) $(CHECK)
BIN    = popcorn
DEVBIN = popcorn-dev

all: $(BIN) $(DEVBIN)

$(BIN): src/main.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ src/main.o $(COMMON) $(LDLIBS)

$(DEVBIN): tools/devmain.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ tools/devmain.o $(COMMON) $(LDLIBS)

src/main.o tools/devmain.o $(COMMON): src/game.h

run: $(BIN)
	./$(BIN)

amiga:
	$(MAKE) -f Makefile.amiga

# ---------------------------------------------------------------- nopad ---
#
# `make nopad` builds popcorn-nopad: the same game with every `_`-named field
# taken out of the structs, so each named field slides up against the next and
# the image is a layout the original never had.
#
# It is a test, and what it tests is whether anything is *read* that has not
# been named. It can work at all only because data.c computes the game's own
# pointers from the fields they point at, so moving a field moves every
# pointer to it; what cannot follow is a read of a byte no field covers.
# Anything that misbehaves in this binary is such a read.
#
# nopad/game.h arrives through -include, not through -I: `#include "game.h"`
# is a quoted include, so src/game.c would find its own directory's copy
# whatever -I said. Pulling the generated header in first means the guard is
# already defined and src/game.h expands to nothing.
NOPADBIN = popcorn-nopad
NOPAD_SRC = nopad/data.c src/exepack.c src/sdl_io.c src/game.c src/stubs.c \
            tools/verify.c tools/lockstep.c tools/autoplay.c tools/devmain.c

# KEEP puts named fields back, one at a time, to find which one a symptom
# belongs to:
#
#     make nopad                       every `_` field dropped
#     make nopad KEEP=_code2           that one put back
#     make nopad KEEP=_code1,_code2    or several
#
# Both generators get the same list, because a header and a data file that
# disagree about which fields exist is worse than either extreme. The recipe
# regenerates every time rather than depending on file dates: KEEP is not a
# file, and make cannot see it change.
nopad:
	cd .. && uv run gen_nopad.py $(if $(KEEP),--keep $(KEEP))
	cd .. && uv run gen_data.py --skip-padding $(if $(KEEP),--keep $(KEEP)) \
	         --out reconstruct/nopad/data.c
	$(CC) $(CFLAGS) -include nopad/game.h -o $(NOPADBIN) $(NOPAD_SRC) $(LDLIBS)

clean:
	rm -f src/*.o tools/*.o $(BIN) $(DEVBIN) $(NOPADBIN)

.PHONY: all run clean nopad amiga
