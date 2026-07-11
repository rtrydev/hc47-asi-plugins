/* HC47 Tweaks — combined runtime plugin for Hitman: Codename 47
 * (32-bit ASI). One DLL housing the four gameplay/graphics features
 * that used to ship as separate ASIs:
 *
 *  - widescreen.c  — resolution passthrough, borderless fullscreen with
 *                    letterbox, modern options-menu resolution list,
 *                    hitman.ini persistence, FOV correction
 *  - hudscale.c    — native GUI/HUD scaling + sharp TrueType text
 *  - hudextras.c   — crosshair shrink, mission timer, FPS readout
 *  - framelimit.c  — configurable FPS cap
 *
 * They were merged because they are closely coupled: widescreen and
 * hudscale coordinate over the same ZSysInterface resolution fields
 * (the HC47_ModeChangeTick handshake, now a plain shared global);
 * framelimit and hudextras hook the same System.dll frame-latch
 * function (entry vs. its clock call site) and must not overlap; and
 * hudextras positions its overlay in hudscale's virtual GUI pixels.
 *
 * Config: scripts/hc47_tweaks.ini — sections [Widescreen], [HudScale],
 * [HudExtras], [FrameLimit] with the same keys the individual plugins
 * used. Log: scripts/hc47_tweaks.log, one tagged line per event.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

char hc47_dir[MAX_PATH];

static FILE *g_log;
static CRITICAL_SECTION g_loglock;

/* one tagged line per call, atomic across the feature threads */
void hc47_vlog(const char *tag, const char *fmt, va_list ap)
{
    if (!g_log) return;
    EnterCriticalSection(&g_loglock);
    fprintf(g_log, "%s: ", tag);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_loglock);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, hc47_dir, sizeof(hc47_dir));
        char *sl = strrchr(hc47_dir, '\\');
        if (sl) *sl = 0;
        InitializeCriticalSection(&g_loglock);
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\hc47_tweaks.log", hc47_dir);
        g_log = fopen(logpath, "w");
        framelimit_init();
        hudscale_init();
        hudextras_init();
        widescreen_init();
    }
    return TRUE;
}
