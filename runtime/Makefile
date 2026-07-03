CC := i686-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -msse2 -static-libgcc
LDFLAGS := -shared -Wl,--kill-at -static -lkernel32

all: ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi

../dist/HC47ReducedX87.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47ReducedX87-diag.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DHC47_DIAG -o $@ $^ $(LDFLAGS)

clean:
	rm -f ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi
