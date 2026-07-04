# mac/Linux: mingw-w64 cross compiler (brew install mingw-w64).
# Windows: MSYS2's 32-bit native gcc (pacman -S mingw-w64-i686-gcc); run
# make with C:\msys64\mingw32\bin on PATH or via ..\build.ps1.
ifeq ($(OS),Windows_NT)
CC := gcc
else
CC := i686-w64-mingw32-gcc
endif
CFLAGS := -O2 -Wall -Wextra -msse2 -static-libgcc
LDFLAGS := -shared -Wl,--kill-at -static -lkernel32

.PHONY: all clean rtrace prof wmdiag

all: ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi ../dist/HC47HudScale.asi ../dist/HC47HudExtras.asi ../dist/HC47Widescreen.asi ../dist/HC47FrameLimit.asi ../dist/dsound.dll

../dist/HC47HudScale.asi: hudscale.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47HudExtras.asi: hudextras.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47Widescreen.asi: widescreen.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/HC47FrameLimit.asi: framelimit.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/dsound.dll: asiloader.c dsound.def
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--enable-stdcall-fixup

# diagnostic window-mode/D3D-init prober (not part of `all`)
wmdiag: ../dist/HC47WinModeDiag.asi

../dist/HC47WinModeDiag.asi: wmdiag.c
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
	rm -f ../dist/HC47ReducedX87.asi ../dist/HC47ReducedX87-diag.asi \
	      ../dist/HC47HudScale.asi ../dist/HC47HudExtras.asi \
	      ../dist/HC47Widescreen.asi ../dist/HC47FrameLimit.asi \
	      ../dist/dsound.dll ../dist/HC47WinModeDiag.asi \
	      ../dist/HC47RenderTrace.asi ../dist/HC47Profile.asi
