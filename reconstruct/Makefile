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
COMMON = exepack.o sdl_io.o game.o verify.o lockstep.o stubs.o
BIN    = popcorn
DEVBIN = popcorn-dev

all: $(BIN) $(DEVBIN)

$(BIN): main.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ main.o $(COMMON) $(LDLIBS)

$(DEVBIN): devmain.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ devmain.o $(COMMON) $(LDLIBS)

main.o devmain.o $(COMMON): game.h

run: $(BIN)
	./$(BIN)

clean:
	rm -f main.o devmain.o $(COMMON) $(BIN) $(DEVBIN)

.PHONY: all run clean
