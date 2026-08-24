# Popcorn, reconstructed. Needs SDL3 - not SDL2, they are not interchangeable.
#
#   Fedora            sudo dnf install gcc make pkgconf-pkg-config SDL3-devel
#   Debian / Ubuntu   sudo apt install build-essential pkg-config libsdl3-dev
#   Arch              sudo pacman -S base-devel sdl3
#   macOS             brew install sdl3
#
# The game itself is not here and is not distributed: put your own POPCORN.EXE
# in ../popcorn/ (or point POPCORN_EXE at it) and the port reads its data.

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
GAME   = src/exepack.o src/sdl_io.o src/game.o src/stubs.o
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

clean:
	rm -f src/*.o tools/*.o $(BIN) $(DEVBIN)

.PHONY: all run clean
