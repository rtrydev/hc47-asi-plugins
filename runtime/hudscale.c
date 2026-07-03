/* HC47 HUD Scale — native GUI scaling for Hitman: Codename 47 (32-bit ASI).
 *
 * The engine's GUI (menus, HUD, text) lays itself out in pixels against the
 * resolution stored in ZSysInterface (+0x19 width, +0x1d height, packed
 * struct, exported as ?g_pSysInterface@@3PAVZSysInterface@@A from
 * Globals.dll). All pixel->normalized conversions, anchor math (left /
 * center / right, top / bottom), culling and cursor clamping in
 * HitmanDlc.dlc use these two ints. The renderer (RenderD3D.dll) keeps its
 * own copy of the real mode (+0x83/+0x87 in the render object) taken at
 * mode-set time and maps normalized coordinates onto the real viewport.
 *
 * Therefore: once the display mode is up, writing width/scale and
 * height/scale into the ZSysInterface fields makes the GUI lay out for a
 * smaller virtual screen which the renderer stretches to the real one —
 * i.e. the HUD gets `scale` times bigger. The 3D scene, display mode, and
 * window mode (exclusive fullscreen / borderless / windowed) are untouched,
 * unlike the dgVoodoo resolution-forcing trick this replaces.
 *
 * Config: scripts/HC47HudScale.ini
 *   [HudScale]
 *   Scale=2.0        ; 1.0 = off; fractional values fine (e.g. 1.5)
 *
 * A watchdog re-applies the virtual size if the game rewrites the fields
 * (mode change from the options menu re-runs apply-settings, which copies
 * the current mode back over them).
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ZSysInterface field offsets (packed struct, verified against System.dll
 * config parser, RenderD3D mode-set and HitmanDlc consumers of this build:
 * HitmanDlc.dlc TimeDateStamp 3A44AFAF era, System.dll 3a3e...) */
#define SI_WIDTH   0x19   /* int32: requested/layout width  */
#define SI_HEIGHT  0x1d   /* int32: requested/layout height */
#define SI_CURW    0x21   /* int32: current mode width  (set by renderer) */
#define SI_CURH    0x25   /* int32: current mode height (set by renderer) */

static FILE *g_log;
static float g_scale = 1.0f;
static char g_dir[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static float read_scale(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\HC47HudScale.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 1.0f;
    float scale = 1.0f;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v;
        if (sscanf(line, " Scale = %f", &v) == 1 ||
            sscanf(line, " Scale=%f", &v) == 1)
            scale = v;
    }
    fclose(f);
    if (!(scale >= 0.5f && scale <= 8.0f)) scale = 1.0f;
    return scale;
}

static uint8_t *sysiface(void)
{
    HMODULE g = GetModuleHandleA("Globals.dll");
    if (!g) return NULL;
    uint8_t **pp = (uint8_t **)(uintptr_t)GetProcAddress(g,
        "?g_pSysInterface@@3PAVZSysInterface@@A");
    return (pp && *pp) ? *pp : NULL;
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    int applied = 0;
    int32_t vw = 0, vh = 0;         /* virtual size we maintain */
    int32_t realw = 0, realh = 0;   /* last seen real mode */
    for (;;) {
        Sleep(applied ? 250 : 50);
        uint8_t *si = sysiface();
        if (!si) continue;
        int32_t curw, curh, w, h;
        memcpy(&curw, si + SI_CURW, 4);
        memcpy(&curh, si + SI_CURH, 4);
        memcpy(&w, si + SI_WIDTH, 4);
        memcpy(&h, si + SI_HEIGHT, 4);
        if (curw < 320 || curh < 200 || curw > 16384 || curh > 16384)
            continue;               /* mode not established yet */

        if (curw != realw || curh != realh) {
            /* first mode-set, or the mode changed */
            realw = curw; realh = curh;
            vw = (int32_t)lroundf((float)realw / g_scale);
            vh = (int32_t)lroundf((float)realh / g_scale);
            logf_("mode %dx%d -> virtual GUI %dx%d (scale %.3f)",
                  realw, realh, vw, vh, (double)g_scale);
            applied = 0;
        }
        if (w != vw || h != vh) {
            memcpy(si + SI_WIDTH, &vw, 4);
            memcpy(si + SI_HEIGHT, &vh, 4);
            if (!applied)
                logf_("applied virtual GUI size %dx%d (was %dx%d)",
                      vw, vh, w, h);
            else
                logf_("re-applied virtual GUI size (game wrote %dx%d)", w, h);
            applied = 1;
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        char *sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\HC47HudScale.log", g_dir);
        g_log = fopen(logpath, "w");
        g_scale = read_scale();
        logf_("HC47 HUD Scale loaded, Scale=%.3f%s", (double)g_scale,
              g_scale == 1.0f ? " (disabled)" : "");
        if (g_scale != 1.0f)
            CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    }
    return TRUE;
}
