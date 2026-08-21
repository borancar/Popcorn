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

OBJS = main.o exepack.o sdl_io.o game.o verify.o
BIN  = popcorn

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): game.h

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all run clean
