/* FrameLimit feature of hc47_tweaks.asi — configurable FPS cap for
 * Hitman: Codename 47.
 *
 * The engine has no frame limiter of its own: the per-frame game-time
 * latch in System.dll (function 0xe2e0) copies current->previous time and
 * reads a new timestamp, with no pacing anywhere in the main loop. On
 * fast machines the game runs uncapped unless vsync or an external
 * limiter steps in, and high frame rates are known to cause gameplay and
 * camera bugs in this engine.
 *
 * This feature hooks the latch's clock call site (`call [edx+0xb4]` at
 * System.dll+0xe309, byte-checked against the retail build) and stalls
 * each frame until the configured frame period has elapsed, measured
 * with QueryPerformanceCounter. The wait sleeps in 1 ms steps while more
 * than a few milliseconds remain and spins the rest, giving a stable cap
 * without burning a whole core. Pacing happens before the engine samples
 * its clock, so the measured frame delta stays clean at the cap.
 *
 * The call site (not the function entry) is hooked deliberately: the
 * hudextras feature entry-hooks the same latch function at +0xe2e0 for
 * its frame tick, and the two patches must not overlap. The engine's
 * fixed-step benchmark branch bypasses the clock call and therefore the
 * cap, which is the sensible behavior there.
 *
 * The deadline advances by one period per frame (drift-free); if the game
 * falls more than one period behind (loading, hitches), the deadline
 * resyncs instead of fast-forwarding through queued frames.
 *
 * Config (hc47_tweaks.ini):
 *   [FrameLimit]
 *   FpsCap=60        ; frames per second; 0 disables the cap
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "common.h"

/* System.dll, retail build (same stamp check as hudextras.c) */
#define SYS_TIMESTAMP    0x3a3e130a
#define SYS_SIZEOFIMAGE  0x47000
#define CLOCKCALL_RVA    0xe309
/* call [edx+0xb4] — the latch's per-frame clock read */
static const uint8_t CLOCKCALL_BYTES[6] = { 0xff, 0x92, 0xb4, 0x00, 0x00, 0x00 };

static float g_fpscap = 60.0f;

static LARGE_INTEGER g_freq;
static LONGLONG g_period;      /* QPC ticks per frame, 0 = off */
static LONGLONG g_deadline;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hc47_vlog("framelimit", fmt, ap);
    va_end(ap);
}

static void read_config(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", hc47_dir, HC47_INI);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v;
        if (sscanf(line, " FpsCap = %f", &v) == 1 ||
            sscanf(line, " FpsCap=%f", &v) == 1)
            g_fpscap = v;
    }
    fclose(f);
    if (g_fpscap != 0.0f && !(g_fpscap >= 10.0f && g_fpscap <= 1000.0f))
        g_fpscap = 60.0f;
}

/* called from the latch entry stub, once per frame */
void __cdecl frame_wait(void)
{
    if (!g_period) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_deadline == 0 || now.QuadPart > g_deadline + g_period) {
        g_deadline = now.QuadPart;      /* first frame or fell behind */
    } else {
        while (now.QuadPart < g_deadline) {
            LONGLONG remain_ms = (g_deadline - now.QuadPart) * 1000
                                 / g_freq.QuadPart;
            if (remain_ms > 4)
                Sleep(1);
            else
                Sleep(0);
            QueryPerformanceCounter(&now);
        }
    }
    g_deadline += g_period;
}

static int install_hook(void)
{
    HMODULE sys = GetModuleHandleA("System.dll");
    if (!sys) return 0;
    uint8_t *base = (uint8_t *)sys;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != SYS_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != SYS_SIZEOFIMAGE) {
        logf_("System.dll build mismatch (stamp %08lx) — cap disabled",
              (unsigned long)nt->FileHeader.TimeDateStamp);
        return -1;
    }
    uint8_t *site = base + CLOCKCALL_RVA;
    if (memcmp(site, CLOCKCALL_BYTES, sizeof(CLOCKCALL_BYTES)) != 0) {
        logf_("unexpected bytes at clock call site — cap disabled");
        return -1;
    }

    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return -1;
    uint8_t *p = stub;
    *p++ = 0x9C;                          /* pushfd */
    *p++ = 0x60;                          /* pushad */
    *p++ = 0xE8;                          /* call frame_wait */
    *(int32_t *)p = (int32_t)((uint8_t *)(uintptr_t)frame_wait - (p + 4));
    p += 4;
    *p++ = 0x61;                          /* popad */
    *p++ = 0x9D;                          /* popfd */
    memcpy(p, CLOCKCALL_BYTES, sizeof(CLOCKCALL_BYTES));
    p += sizeof(CLOCKCALL_BYTES);
    *p++ = 0xE9;                          /* jmp back */
    *(int32_t *)p = (int32_t)((site + sizeof(CLOCKCALL_BYTES)) - (p + 4));
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, 64);

    DWORD old;
    if (!VirtualProtect(site, sizeof(CLOCKCALL_BYTES),
                        PAGE_EXECUTE_READWRITE, &old))
        return -1;
    site[0] = 0xE9;
    *(int32_t *)(site + 1) = (int32_t)(stub - (site + 5));
    memset(site + 5, 0x90, sizeof(CLOCKCALL_BYTES) - 5);
    VirtualProtect(site, sizeof(CLOCKCALL_BYTES), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(CLOCKCALL_BYTES));
    logf_("frame limiter installed, cap %.1f fps", (double)g_fpscap);
    return 1;
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (install_hook() != 0) break;
        Sleep(25);
    }
    return 0;
}

void framelimit_init(void)
{
    read_config();
    QueryPerformanceFrequency(&g_freq);
    if (g_fpscap > 0.0f && g_freq.QuadPart > 0)
        g_period = (LONGLONG)((double)g_freq.QuadPart / (double)g_fpscap);
    logf_("FpsCap=%.1f%s", (double)g_fpscap,
          g_period ? "" : " (disabled)");
    if (g_period)
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
}
