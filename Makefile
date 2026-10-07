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
# The layout the original has, byte for byte: what every check here compares
# against. In CPPFLAGS, which the compile rule uses and `make nopad` leaves
# out - see game.h for the other layout.
CPPFLAGS += -DMATCH_MEMORY_LAYOUT
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
GAME   = src/data.o src/exepack.o src/sdl_io.o src/game.o src/stubs.o \
         src/layout_check.o
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
# `make nopad` builds popcorn-nopad: the same game in game.h's other layout,
# without MATCH_MEMORY_LAYOUT - every struct unpacked and every `_`-named
# field gone, so the image is a layout the original never had. It is the
# layout the Amiga build uses, here on the host.
#
# It is a test, and what it tests is whether anything is *read* that has not
# been named. It can work at all only because data.c computes the game's own
# pointers from the fields they point at, so moving a field moves every
# pointer to it; what cannot follow is a read of a byte no field covers.
# Anything that misbehaves in this binary is such a read. The harness flags
# that reach into the image by address - --verify, --lockstep, --resume -
# assume the original's layout and mean nothing here.
NOPADBIN = popcorn-nopad
NOPAD_SRC = src/data.c src/exepack.c src/sdl_io.c src/game.c src/stubs.c \
            tools/verify.c tools/lockstep.c tools/autoplay.c tools/devmain.c

nopad:
	$(CC) $(CFLAGS) -o $(NOPADBIN) $(NOPAD_SRC) $(LDLIBS)

clean:
	rm -f src/*.o tools/*.o $(BIN) $(DEVBIN) $(NOPADBIN)

.PHONY: all run clean nopad amiga
