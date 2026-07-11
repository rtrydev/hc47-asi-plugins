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

all: ../dist/hc47_reduced_x87.asi ../dist/hc47_reduced_x87_diag.asi ../dist/hc47_tweaks.asi ../dist/dsound.dll

# the combined gameplay/graphics plugin: widescreen + hudscale +
# hudextras + framelimit in one ASI (see tweaks.c for why they merge)
TWEAKS_SRC := tweaks.c widescreen.c hudscale.c hudextras.c framelimit.c

../dist/hc47_tweaks.asi: $(TWEAKS_SRC) common.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $(TWEAKS_SRC) $(LDFLAGS)

../dist/dsound.dll: asiloader.c dsound.def
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--enable-stdcall-fixup

# diagnostic window-mode/D3D-init prober (not part of `all`)
wmdiag: ../dist/hc47_win_mode_diag.asi

../dist/hc47_win_mode_diag.asi: wmdiag.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# diagnostic render-interface tracer (not part of `all`)
rtrace: ../dist/hc47_render_trace.asi

../dist/hc47_render_trace.asi: rtrace.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# diagnostic EIP-sampling profiler (not part of `all`)
prof: ../dist/hc47_profile.asi

../dist/hc47_profile.asi: eipprof.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/hc47_reduced_x87.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/hc47_reduced_x87_diag.asi: hc47x87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DHC47_DIAG -o $@ $^ $(LDFLAGS)

clean:
	rm -f ../dist/hc47_reduced_x87.asi ../dist/hc47_reduced_x87_diag.asi \
	      ../dist/hc47_tweaks.asi \
	      ../dist/dsound.dll ../dist/hc47_win_mode_diag.asi \
	      ../dist/hc47_render_trace.asi ../dist/hc47_profile.asi
