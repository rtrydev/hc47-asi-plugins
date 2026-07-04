CC := i686-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -msse2 -static-libgcc
LDFLAGS := -shared -Wl,--kill-at -static -lkernel32

all: ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi ../dist/HC47HudScale.asi ../dist/HC47HudExtras.asi

../dist/HC47HudScale.asi: hudscale.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47HudExtras.asi: hudextras.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# diagnostic render-interface tracer (not part of `all`)
rtrace: ../dist/HC47RenderTrace.asi

../dist/HC47RenderTrace.asi: rtrace.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# diagnostic EIP-sampling profiler (not part of `all`)
prof: ../dist/HC47Profile.asi

../dist/HC47Profile.asi: eipprof.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47ReducedX87.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47ReducedX87-diag.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DHC47_DIAG -o $@ $^ $(LDFLAGS)

clean:
	rm -f ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi
