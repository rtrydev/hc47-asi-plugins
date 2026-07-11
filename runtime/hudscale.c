/* HudScale feature of hc47_tweaks.asi — native GUI scaling for
 * Hitman: Codename 47.
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
 * Config (hc47_tweaks.ini):
 *   [HudScale]
 *   Scale=2.0        ; 1.0 = off; fractional values fine (e.g. 1.5).
 *                    ; An upper bound: the effective scale is clamped
 *                    ; per display mode so the virtual GUI size never
 *                    ; drops below 640x480 (the layouts' authored
 *                    ; minimum) — e.g. 2.0 acts as 1.5 at 1280x720.
 *   SharpText=1      ; re-rasterize TTF fonts at the real pixel size
 *
 * Timing matters: the renderer copies the requested resolution into the
 * current-mode fields (+0x21/+0x25) already at window creation, several
 * hundred ms BEFORE it runs display-mode selection, which reads +0x19/
 * +0x1d back. Writing the virtual size in that window makes fullscreen
 * mode selection pick a real mode of the virtual size (e.g. a 1920x1080
 * mode on a 4K display) and the GUI ends up unscaled. So the first apply
 * happens from an ntdll loader notification when HitmanDlc.dlc loads:
 * by then the renderer is fully initialized (real mode set, device
 * created), and none of the GUI layout code (which lives in that DLL)
 * can have run yet.
 *
 * A watchdog re-applies the virtual size if the game rewrites the fields
 * (mode change from the options menu re-runs apply-settings, which copies
 * the current mode back over them); it is gated on HitmanDlc.dlc being
 * loaded for the same reason.
 *
 * SharpText: menu/HUD text comes from TrueType fonts rasterized at load
 * time by a FreeType build embedded in HitmanDlc.dlc (class ZTTFONT).
 * The pixel size passed to FT_Set_Char_Size is the FontSize long from the
 * GUI resource (object +0x26c) — i.e. a size in *virtual* pixels, so with
 * Scale=2 the glyphs would be rasterized small and magnified 2x (blurry).
 * Two byte-checked patches (both verified against this exact game build):
 *  1. ZTTFONT lazy init (RVA 0x8cf2b): multiply the FT_Set_Char_Size pixel
 *     size by Scale — glyphs rasterize 1:1 with real screen pixels.
 *  2. The shared label glyph-geometry builder (RVA 0x5e250): labels
 *     (ZCHAROBJ etc.) multiply their own render-scale doubles into every
 *     glyph vertex; an entry hook computes an effective scale (divided by
 *     Scale when the label's font is a ZTTFONT) into globals, and the
 *     builder's 16 fmul sites are retargeted to those globals.
 * Net effect: crisp text at the original layout size, for any Scale.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "common.h"

/* ZSysInterface field offsets (packed struct, verified against System.dll
 * config parser, RenderD3D mode-set and HitmanDlc consumers of this build:
 * HitmanDlc.dlc TimeDateStamp 3A44AFAF era, System.dll 3a3e...) */
#define SI_WIDTH   0x19   /* int32: requested/layout width  */
#define SI_HEIGHT  0x1d   /* int32: requested/layout height */
#define SI_CURW    0x21   /* int32: current mode width  (set by renderer) */
#define SI_CURH    0x25   /* int32: current mode height (set by renderer) */

/* ZTTFONT lazy-init in HitmanDlc.dlc (build stamp 0x3A3E13D1) */
#define DLC_TIMESTAMP    0x3a3e13d1
#define DLC_SIZEOFIMAGE  0x274000
#define TTF_SIZE_RVA     0x8cf2b   /* mov ecx,[esi+0x26c] (6 bytes) */
static const uint8_t TTF_SIZE_BYTES[6] = { 0x8b, 0x8e, 0x6c, 0x02, 0x00, 0x00 };
#define ZFONT_SCALEX_OFF 0xd7      /* double, multiplied into glyph verts */
#define ZFONT_SCALEY_OFF 0xdf
#define ZTTFONT_VTBL_RVA 0x1fbfcc  /* ZTTFONT (FreeType font resource) vtable */
#define BUILDER_RVA      0x5e250   /* shared label glyph-geometry builder */
#define LABEL_FONT_OFF   0x10b     /* label -> font-ish object (charset?) */
#define LABEL_FONT2_OFF  0x20f     /* label -> glyph provider (GetGlyph target) */

static float g_scale = 1.0f;
static int g_sharptext = 1;
static double g_invscale = 1.0;    /* referenced from the stub */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hc47_vlog("hudscale", fmt, ap);
    va_end(ap);
}

static float read_scale(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", hc47_dir, HC47_INI);
    FILE *f = fopen(path, "r");
    if (!f) return 1.0f;
    float scale = 1.0f;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v;
        int b;
        if (sscanf(line, " Scale = %f", &v) == 1 ||
            sscanf(line, " Scale=%f", &v) == 1)
            scale = v;
        else if (sscanf(line, " SharpText = %d", &b) == 1 ||
                 sscanf(line, " SharpText=%d", &b) == 1)
            g_sharptext = b;
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

static double g_eff_sx = 1.0, g_eff_sy = 1.0;  /* read by patched fmuls */
static uint32_t g_zttfont_vtbl;

/* called from the builder entry thunk; regs = pushad block
 * (EDI ESI EBP ESP EBX EDX ECX EAX ascending), ecx = label object */
void __cdecl builder_entry(uint32_t *regs)
{
    uint8_t *label = (uint8_t *)regs[6];
    double sx, sy;
    memcpy(&sx, label + ZFONT_SCALEX_OFF, 8);
    memcpy(&sy, label + ZFONT_SCALEY_OFF, 8);
    uint8_t *f1 = *(uint8_t **)(label + LABEL_FONT_OFF);
    uint8_t *f2 = *(uint8_t **)(label + LABEL_FONT2_OFF);
    int ttf = (f1 && !IsBadReadPtr(f1, 4) && *(uint32_t *)f1 == g_zttfont_vtbl) ||
              (f2 && !IsBadReadPtr(f2, 4) && *(uint32_t *)f2 == g_zttfont_vtbl);
    if (ttf) {
        sx *= g_invscale;
        sy *= g_invscale;
    }
#ifdef HUDSCALE_DEBUG
    {
        static LONG dbg_n;
        if (InterlockedIncrement(&dbg_n) <= 40)
            logf_("builder: label=%p vt=%08x f1=%p(%08x) f2=%p(%08x) ttf=%d s=(%.3f,%.3f)",
                  label, *(uint32_t *)label,
                  f1, f1 && !IsBadReadPtr(f1,4) ? *(uint32_t *)f1 : 0,
                  f2, f2 && !IsBadReadPtr(f2,4) ? *(uint32_t *)f2 : 0,
                  ttf, sx, sy);
    }
#endif
    g_eff_sx = sx;
    g_eff_sy = sy;
}

/* ---- SharpText: patch ZTTFONT size selection ----
 * stub replacing `mov ecx,[esi+0x26c]`:
 *   mov ecx,[esi+0x26c]
 *   imul ecx,ecx,S4096        ; S4096 = round(Scale*4096)
 *   add ecx,0x800
 *   sar ecx,12                ; ecx = round(FontSize*Scale)
 *   jmp back
 */
static int patch_sharptext(void)
{
    HMODULE dlc = GetModuleHandleA("HitmanDlc.dlc");
    if (!dlc) return 0;
    uint8_t *base = (uint8_t *)dlc;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != DLC_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != DLC_SIZEOFIMAGE) {
        logf_("SharpText: HitmanDlc.dlc build mismatch (stamp %08lx) — skipped",
              (unsigned long)nt->FileHeader.TimeDateStamp);
        return -1;
    }
    uint8_t *site = base + TTF_SIZE_RVA;
    if (memcmp(site, TTF_SIZE_BYTES, 6) != 0) {
        logf_("SharpText: unexpected bytes at patch site — skipped");
        return -1;
    }

    g_invscale = 1.0 / (double)g_scale;
    int32_t s4096 = (int32_t)lroundf(g_scale * 4096.0f);

    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 128,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return -1;
    uint8_t *p = stub;
    /* mov ecx,[esi+0x26c] */
    memcpy(p, TTF_SIZE_BYTES, 6); p += 6;
    /* imul ecx,ecx,imm32 */
    *p++ = 0x69; *p++ = 0xC9; memcpy(p, &s4096, 4); p += 4;
    /* add ecx,0x800 */
    *p++ = 0x81; *p++ = 0xC1; *(uint32_t *)p = 0x800; p += 4;
    /* sar ecx,12 */
    *p++ = 0xC1; *p++ = 0xF9; *p++ = 0x0C;
    /* jmp back to site+6 */
    *p++ = 0xE9;
    *(int32_t *)p = (int32_t)((site + 6) - (p + 4)); p += 4;

    DWORD old;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return -1;
    site[0] = 0xE9;
    *(int32_t *)(site + 1) = (int32_t)(stub - (site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);

    /* Labels (ZCHAROBJ/ZLINEOBJ/...) each carry their own render-scale
     * doubles at +0xd7/+0xdf and the shared glyph-geometry builder
     * (BUILDER_RVA) multiplies them into every glyph vertex. Compensation
     * must apply only when the label's font (ptr at +0x10b) is a ZTTFONT.
     * We hook the builder entry to compute the effective scale pair into
     * globals (label scale, times 1/Scale for TTF fonts) and retarget the
     * 16 `fmul qword [ebx+0xd7/0xdf]` sites in the builder to read the
     * globals instead (same 6-byte encoding). Single render thread. */
    {
        static const uint8_t prolog[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
        uint8_t *bsite = base + BUILDER_RVA;
        if (memcmp(bsite, prolog, 6) != 0) {
            logf_("SharpText: builder prologue mismatch — label scale fix skipped");
        } else {
            int nx = 0, ny = 0, bad = 0;
            /* rewrite fmul sites first (they only take effect once the
             * entry hook fills the globals every call) */
            for (uint32_t rva = BUILDER_RVA; rva < BUILDER_RVA + 0xd00; rva++) {
                uint8_t *q = base + rva;
                if (q[0] != 0xDC || q[1] != 0x8B) continue;
                uint32_t disp;
                memcpy(&disp, q + 2, 4);
                if (disp != ZFONT_SCALEX_OFF && disp != ZFONT_SCALEY_OFF) continue;
                double *tgt = disp == ZFONT_SCALEX_OFF ? &g_eff_sx : &g_eff_sy;
                DWORD o2;
                if (!VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &o2)) { bad++; continue; }
                q[1] = 0x0D;   /* fmul qword [abs32] */
                uint32_t a = (uint32_t)(uintptr_t)tgt;
                memcpy(q + 2, &a, 4);
                VirtualProtect(q, 6, o2, &o2);
                if (disp == ZFONT_SCALEX_OFF) nx++; else ny++;
            }
            g_zttfont_vtbl = (uint32_t)(uintptr_t)(base + ZTTFONT_VTBL_RVA);
            /* entry thunk: pushfd/pushad/push esp/call C/add esp,4/popad/
             * popfd/<prolog>/jmp builder+6 */
            uint8_t *t = stub + 64;
            uint8_t *q = t;
            *q++ = 0x9C; *q++ = 0x60; *q++ = 0x54;
            *q++ = 0xE8; *(int32_t *)q = (int32_t)((uint8_t *)(uintptr_t)builder_entry
                                                   - (q + 4)); q += 4;
            *q++ = 0x83; *q++ = 0xC4; *q++ = 0x04;
            *q++ = 0x61; *q++ = 0x9D;
            memcpy(q, prolog, 6); q += 6;
            *q++ = 0xE9; *(int32_t *)q = (int32_t)((bsite + 6) - (q + 4)); q += 4;
            DWORD o3;
            if (VirtualProtect(bsite, 6, PAGE_EXECUTE_READWRITE, &o3)) {
                bsite[0] = 0xE9;
                *(int32_t *)(bsite + 1) = (int32_t)(t - (bsite + 5));
                bsite[5] = 0x90;
                VirtualProtect(bsite, 6, o3, &o3);
                FlushInstructionCache(GetCurrentProcess(), bsite, 6);
                logf_("SharpText: label scale fix active (%d+%d fmul sites%s)",
                      nx, ny, bad ? ", some skipped!" : "");
            }
        }
    }

    FlushInstructionCache(GetCurrentProcess(), stub, 128);
    logf_("SharpText: TTF fonts rasterize at %.3fx size (render scale %.4f)",
          (double)g_scale, g_invscale);
    return 1;
}

static CRITICAL_SECTION g_applylock;
static int g_applied;
static int32_t g_vw, g_vh;          /* virtual size we maintain */
static int32_t g_realw, g_realh;    /* last seen real mode */
static int32_t g_prevw, g_prevh;    /* real mode before the last change */
static int32_t g_prevvw, g_prevvh;  /* and the virtual size it used */

/* The GUI layouts are authored for a 640x480 minimum (the engine's
 * default screen size): a virtual size below that lays menu elements
 * out past the screen edge (observed: at 1280x720 with Scale=2 the
 * options screen's bottom button row landed below the 640x360 virtual
 * screen). Clamp the effective scale per mode so the virtual size never
 * drops under 640x480 — the configured scale simply becomes "at most".
 * SharpText intentionally keeps rasterizing at the CONFIGURED scale:
 * its label-geometry compensation is relative to the raster scale, not
 * the GUI magnification, so text keeps its layout size at any effective
 * scale and is merely supersampled when the clamp bites. */
#define MIN_VIRTUAL_W 640
#define MIN_VIRTUAL_H 480

static float eff_scale(int32_t w, int32_t h)
{
    float s = g_scale;
    float m = (float)w / (float)MIN_VIRTUAL_W;
    float mh = (float)h / (float)MIN_VIRTUAL_H;
    if (mh < m) m = mh;
    if (s > m) s = m;
    if (s < 1.0f) s = 1.0f;
    return s;
}

/* Widescreen handshake: HC47_ModeChangeTick (widescreen.c) holds a
 * GetTickCount stamp while an in-game mode change is staged/consumed.
 * The renderer re-init reads +0x19/+0x1d back as the DISPLAY MODE in
 * that window — re-applying the virtual size then makes the game come
 * up in a mode of the virtual size (GUI ends up at scale² too small).
 * A stamp older than 3s is treated as stale (renderer paths without
 * the end-of-consumption hook), so a missed clear cannot wedge us. */
static int modechange_inflight(void)
{
    DWORD t = HC47_ModeChangeTick;
    return t != 0 && GetTickCount() - t < 3000;
}

/* Apply (or re-apply) the virtual size. Only safe once HitmanDlc.dlc is
 * loaded: before that the current-mode fields merely mirror the request
 * (mode selection has not run) and shrinking the request would make the
 * game set a real mode of the virtual size. */
static void try_apply(void)
{
    if (!GetModuleHandleA("HitmanDlc.dlc")) return;
    uint8_t *si = sysiface();
    if (!si) return;
    EnterCriticalSection(&g_applylock);
    int32_t curw, curh, w, h;
    memcpy(&curw, si + SI_CURW, 4);
    memcpy(&curh, si + SI_CURH, 4);
    memcpy(&w, si + SI_WIDTH, 4);
    memcpy(&h, si + SI_HEIGHT, 4);
    if (curw < 320 || curh < 200 || curw > 16384 || curh > 16384) {
        LeaveCriticalSection(&g_applylock);
        return;                     /* mode not established yet */
    }
    if (curw != g_realw || curh != g_realh) {
        /* first mode-set, or the mode changed */
        g_prevw = g_realw;
        g_prevh = g_realh;
        g_prevvw = g_vw;
        g_prevvh = g_vh;
        g_realw = curw; g_realh = curh;
        float eff = eff_scale(curw, curh);
        g_vw = (int32_t)lroundf((float)g_realw / eff);
        g_vh = (int32_t)lroundf((float)g_realh / eff);
        logf_("mode %dx%d -> virtual GUI %dx%d (scale %.3f%s)",
              g_realw, g_realh, g_vw, g_vh, (double)eff,
              eff != g_scale ? ", clamped so the GUI keeps >= 640x480"
                             : "");
        g_applied = 0;
    }
    if ((w != g_vw || h != g_vh) && !modechange_inflight()) {
        memcpy(si + SI_WIDTH, &g_vw, 4);
        memcpy(si + SI_HEIGHT, &g_vh, 4);
        if (!g_applied)
            logf_("applied virtual GUI size %dx%d (was %dx%d)",
                  g_vw, g_vh, w, h);
        else
            logf_("re-applied virtual GUI size (game wrote %dx%d)", w, h);
        g_applied = 1;
    }
    LeaveCriticalSection(&g_applylock);
}

/* Called by widescreen's SetResolution replacement: a mode request that
 * equals the virtual GUI size of the current or the previous real mode
 * is an engine path echoing back snapshotted layout fields, not a
 * player pick (observed: the options screen's Cancel after a resolution
 * change restores the PRE-CHANGE +0x19/+0x1d — the old virtual size —
 * which the re-init then adopts as a real display mode at 1/scale).
 * Rewrite it to the corresponding real mode. The caller has already
 * checked the request against the display-mode list, so a legitimate
 * pick (always a real mode) can never be translated. Returns 1 when
 * translated. */
int hc47_hudscale_unvirtual(int32_t *w, int32_t *h)
{
    if (g_scale == 1.0f)
        return 0;               /* feature off (lock not initialized) */
    int ret = 0;
    EnterCriticalSection(&g_applylock);
    /* compare against the virtual sizes actually used (the effective
     * scale is clamped per mode, so they cannot be recomputed from the
     * configured scale alone) */
    const int32_t vw[2] = { g_vw, g_prevvw };
    const int32_t vh[2] = { g_vh, g_prevvh };
    const int32_t rw[2] = { g_realw, g_prevw };
    const int32_t rh[2] = { g_realh, g_prevh };
    for (int i = 0; i < 2 && !ret; i++) {
        if (rw[i] < 320 || rh[i] < 200 || vw[i] < 320 || vh[i] < 200)
            continue;
        if (*w == vw[i] && *h == vh[i] &&
            (*w != rw[i] || *h != rh[i])) {
            *w = rw[i];
            *h = rh[i];
            ret = 1;
        }
    }
    LeaveCriticalSection(&g_applylock);
    return ret;
}

/* Called by widescreen's renderer init-OK hooks, on the game's own
 * thread, the instant a (re)init has consumed the layout fields (the
 * handshake was just cleared): reclaim them for the virtual GUI size
 * BEFORE control returns to the engine, whose post-mode-change code
 * re-lays the open screens out against whatever the fields hold.
 * Leaving that to the polling watchdog let the settings screen lay
 * itself out against the REAL mode and then render normalized by the
 * virtual size — zoomed off the bottom-right corner, mouse mapping
 * equally off, until the screen was rebuilt. */
void hc47_hudscale_apply(void)
{
    if (g_scale == 1.0f)
        return;                 /* feature off (lock not initialized) */
    try_apply();
}

/* ntdll loader notification: fires on HitmanDlc.dlc load before any of
 * its code runs — the deterministic first-apply point. */
typedef VOID (CALLBACK *LDR_NOTIFY_FN)(ULONG reason, const void *data, void *ctx);
typedef LONG (NTAPI *LdrRegisterDllNotification_t)(ULONG, LDR_NOTIFY_FN, void *, void **);

static VOID CALLBACK on_dll_notify(ULONG reason, const void *data, void *ctx)
{
    (void)data; (void)ctx;
    if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */)
        try_apply();
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    int sharp_done = 0;
    for (;;) {
        Sleep(g_applied ? 250 : 50);
        if (g_sharptext && !sharp_done && patch_sharptext() != 0)
            sharp_done = 1;
        try_apply();
    }
    return 0;
}

void hudscale_init(void)
{
    g_scale = read_scale();
    logf_("Scale=%.3f%s", (double)g_scale,
          g_scale == 1.0f ? " (disabled)" : "");
    if (g_scale != 1.0f) {
        InitializeCriticalSection(&g_applylock);
        LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)
            (uintptr_t)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                      "LdrRegisterDllNotification");
        void *cookie = NULL;
        if (reg && reg(0, on_dll_notify, NULL, &cookie) == 0)
            logf_("using loader notifications for first apply");
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    }
}
