CC := i686-w64-mingw32-gcc
CFLAGS := -O1 -Wall -msse2 -static -static-libgcc

all: difftest.exe

difftest.exe: difftest.c probe.S ../runtime/helpers.S
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f difftest.exe
