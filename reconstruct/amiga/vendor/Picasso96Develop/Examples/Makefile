# This Makefile makes the examples for Piccasso96 as of 02.03.1997

# The next variable should be set to the directory where You have installed
# the Picasso96-includes (The given value is where I have installed them)
PICASSO=/ADE/local/os-include

CC=gcc
CFLAGS=-I$(PICASSO) -O2 -m68020 -fbaserel -fomit-frame-pointer
LFLAGS=-noixemul -m68020 -fbaserel

.PHONY:	all
.PHONY:	clean
.PHONY:	no-obj

#
#
all: BestModeID.gnu ModeList.gnu OpenScreen.gnu \
     RequestModeID.gnu WriteTrueColorData.gnu OpenPIP.gnu
clean:
	rm -rf BestModeID.gnu ModeList.gnu OpenScreen.gnu \
	       RequestModeID.gnu WriteTrueColorData.gnu OpenPIP.gnu \
	       *.o
no-obj:
	rm -rf *.o

#
#
BestModeID.gnu: BestModeID.o
	$(CC) $(LFLAGS) -o $@ $<

ModeList.gnu: ModeList.o
	$(CC) $(LFLAGS) -o $@ $<

OpenScreen.gnu: OpenScreen.o
	$(CC) $(LFLAGS) -o $@ $<

RequestModeID.gnu: RequestModeID.o
	$(CC) $(LFLAGS) -o $@ $<

WriteTrueColorData.gnu: WriteTrueColorData.o
	$(CC) $(LFLAGS) -o $@ $<

OpenPIP.gnu: OpenPIP.o
	$(CC) $(LFLAGS) -o $@ $<
