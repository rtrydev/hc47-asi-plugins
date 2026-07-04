/* HC47 Widescreen — native widescreen support for Hitman: Codename 47
 * (32-bit ASI). Takes the resolution from hitman.ini and the live display
 * mode from ZSysInterface, so it composes cleanly with HC47HudScale.
 *
 * What the stock game does wrong at non-4:3 resolutions:
 *  1. The renderer's mode-set path (RenderD3D.dll / RenderOpenGL.dll)
 *     snaps the requested width to a fixed 4:3 ladder (512..1600) when the
 *     ZSysInterface fullscreen flag (+0x12) is set, discarding custom
 *     resolutions from hitman.ini.
 *  2. Camera FOV is authored for 4:3. Six HitmanDlc.dlc camera setups push
 *     the constant 67.4 (degrees, as a double) into SetFOV (vtable +0x37c);
 *     cutscene scripts supply degrees converted to radians into the camera
 *     FOV field (+0x16e); the sniper scope loads per-zoom radian floats
 *     into the same field. At wide aspects the image ends up Vert-.
 *
 * What this plugin patches (all sites byte-checked against this exact
 * retail build; a mismatch skips that patch path):
 *  - Resolution snap: the guarding `je` is made unconditional in whichever
 *    renderer is loaded, so the requested hitman.ini resolution passes
 *    through unmodified. No resolution is written anywhere, so there is
 *    nothing to keep in sync with hitman.ini or with HC47HudScale's
 *    virtual GUI size.
 *  - FOV: the real display mode is read from ZSysInterface (+0x21/+0x25 —
 *    the fields HC47HudScale leaves untouched) and each FOV source gets
 *    the standard Vert- -> Hor+ correction
 *        new = 2*atan(tan(old/2) * (aspect / (4/3)))
 *    The six 67.4-degree push sites are rewritten in place (the gameplay
 *    camera additionally honors FOVFactor); the cutscene and scope sites
 *    take their value from script data, so those get entry hooks that
 *    correct the loaded value at run time. The camera-copy path
 *    (SetFOV(fov_rad * 180/pi) at 0x24e71) is left alone: sources are
 *    corrected, so copies stay consistent.
 *  - Draw distance (optional): the far-clip float handed to the renderer
 *    at camera activation (0x24ec2) is scaled by DrawDistanceFactor.
 *
 * Resolution guard: the passthrough exposes two failure modes the stock
 * 4:3 ladder used to mask:
 *  1. Fullscreen requires the requested mode to exist verbatim in the
 *     driver's mode list; a miss throws "Unable to find a suitable
 *     display mode for true color. Try changing to 16bit colors."
 *     (Typical on CrossOver, whose mode list rarely contains the exact
 *     hitman.ini resolution.)
 *  2. On modern Windows the legacy D3D7 HAL refuses render targets
 *     larger than 2048px per axis (CreateDevice -> D3DERR_INVALID_DEVICE
 *     -> "Unable to initialize Direct3D"), in windowed AND fullscreen
 *     mode alike. Wine/CrossOver has no such limit.
 * So before the renderer runs mode selection, the requested resolution
 * in ZSysInterface is validated: fullscreen resolutions must exist in
 * the EnumDisplaySettings mode list, and with RenderD3D on real Windows
 * (not Wine) the size must pass a Direct3D7 render-target probe; when a
 * check fails the request is clamped to the best resolution that works.
 *
 * Borderless fullscreen (default on Wine/CrossOver): exclusive DirectDraw
 * fullscreen is broken under the CrossOver Mac driver — the emulated mode
 * change renders oversized on scaled Retina desktops, exclusive-mode
 * cursor clipping freezes the mouse, and alt-tab loses the exclusive
 * surfaces (permanent black window; the game never restores them). The
 * windowed path has none of these problems, so when hitman.ini asks for
 * fullscreen the request is converted before the renderer loads: the
 * ZSysInterface fullscreen flag (+0x12) is cleared and the resolution is
 * replaced with the current desktop size. The game's own windowed path
 * already creates an undecorated popup window, so this alone yields
 * borderless fullscreen; the window is only nudged so its client area
 * sits at 0,0 (its style is never touched — restyling a live DirectDraw
 * device window blacks out rendering under the CrossOver Mac driver).
 * The same conversion is installed as a hook on the renderer's mode-set
 * path (replacing the plain snap-skip jump), so toggling fullscreen in
 * the in-game options converts on the fly instead of breaking, and the
 * desktop resolution is added to the options screen's resolution list
 * (the list, its "%d x %d" labels and the applied values all come from
 * the renderer's static mode table; a copy with one extra entry replaces
 * it via the table accessor's imm32).
 *
 * A watchdog thread applies the renderer patch as soon as the renderer
 * module appears, applies the HitmanDlc patches once the display mode is
 * known, and rewrites the FOV constants if the mode changes at run time.
 *
 * Config: scripts/HC47Widescreen.ini
 *   [Widescreen]
 *   Enabled=1
 *   FOVFactor=1.0          ; extra factor on the gameplay camera FOV
 *   DrawDistanceFactor=1.0 ; 1.0 leaves the draw distance untouched
 *   Borderless=-1          ; fullscreen -> borderless window at desktop
 *                          ; resolution: -1 auto (on under Wine, off on
 *                          ; real Windows), 0 never, 1 always
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ZSysInterface (packed struct, ?g_pSysInterface@@3PAVZSysInterface@@A
 * from Globals.dll); +0x21/+0x25 hold the real current mode set by the
 * renderer — HC47HudScale only rewrites +0x19/+0x1d. */
#define SI_CURW  0x21
#define SI_CURH  0x25

/* HitmanDlc.dlc, retail build */
#define DLC_TIMESTAMP    0x3a3e13d1
#define DLC_SIZEOFIMAGE  0x274000
#define CUTSCENE_RVA     0x90167   /* fld qword [esp+0x14]; fmul deg2rad */
#define DEG2RAD_RVA      0x1f03f0  /* the fmul's absolute operand (rebased
                                      at load — HitmanDlc.dlc relocates) */
#define SCOPE1_RVA       0x2e544   /* fld dword [esi+eax*4+0xd8] */
#define SCOPE2_RVA       0x2e633
#define DRAWDIST_RVA     0x24ec2   /* mov eax,[ebp+0x18a] (far clip) */
#define RESSEL_RVA       0x6acfb   /* mov edx,[esi+0x11f] — options-screen
                                      init loads the config width here and
                                      matches it against the mode table to
                                      pick the selected resolution entry */
static const uint8_t CUT_BYTES[6]   = { 0xdd, 0x44, 0x24, 0x14, 0xdc, 0x0d };
#define CUT_SITE_LEN 10
static const uint8_t SCOPE_BYTES[7] = { 0xd9, 0x84, 0x86, 0xd8, 0x00, 0x00, 0x00 };
static const uint8_t DD_BYTES[6]    = { 0x8b, 0x85, 0x8a, 0x01, 0x00, 0x00 };
static const uint8_t RESSEL_BYTES[6] = { 0x8b, 0x96, 0x1f, 0x01, 0x00, 0x00 };

/* `push 0x4050d999; push 0x9999999a` — the double 67.4 handed to SetFOV.
 * Index 0 is the gameplay (mouse-controlled) camera; the rest are base /
 * main / dialog / plane / static-activation cameras. */
static const uint32_t FOV_PUSH_RVAS[6] = {
    0x26b14, 0x21456, 0x2400d, 0x285fe, 0x2c318, 0x30057
};
static const uint8_t PUSH_BYTES[10] = { 0x68, 0x99, 0xd9, 0x50, 0x40,
                                        0x68, 0x9a, 0x99, 0x99, 0x99 };
#define ORIG_FOV_DEG 67.4

/* Resolution-snap guard in the renderers: identical 16-byte sequence
 * `je +0x105; mov eax,[edx+0x19]; cmp eax,0x200; jge` in both. The `je`
 * skips the 4:3 snap ladder; its target is site+0x10b.
 *
 * mode_table/mode_getter: the renderer's static display-mode table
 * ({w,h,bpp,0} entries, 16 bytes each, null-terminated) that backs the
 * in-game options resolution list, and the one-instruction accessor
 * `mov eax, imm32; ret` the game reaches it through (menu labels are
 * formatted "%d x %d" from the entries, and applying a selection copies
 * the entry's w/h into the config, so extending the table is all it
 * takes to add a resolution to the menu). */
static const uint8_t SNAP_BYTES[16] = {
    0x0f, 0x84, 0x05, 0x01, 0x00, 0x00, 0x8b, 0x42, 0x19,
    0x3d, 0x00, 0x02, 0x00, 0x00, 0x7d, 0x0f
};
#define SNAP_SKIP 0x10b   /* je target, relative to the snap site */
static const struct renderer_site {
    const char *module;
    uint32_t timestamp, sizeofimage, rva;
    uint32_t mode_getter, mode_table;
} RENDERERS[2] = {
    { "RenderD3D.dll",    0x3a3e1338, 0x4d000, 0x29363, 0x298c0, 0x3d178 },
    { "RenderOpenGL.dll", 0x3a3e1318, 0x56000, 0x2b913, 0x2be70, 0x36d28 },
};

static FILE *g_log;
static char g_dir[MAX_PATH];
static int g_enabled = 1;
static float g_fovfactor = 1.0f;
static float g_ddfactor = 1.0f;
static int g_borderless = -1;        /* -1 auto (Wine), 0 never, 1 always */
static int g_borderless_active;      /* fullscreen request was converted */

static double g_fov_scale = 1.0;   /* aspect / (4/3), read by hook callbacks */
static double g_cut_out;           /* cutscene FOV, radians, set per call */
static float g_scope_out;          /* scope FOV, radians, set per call */
static uint32_t g_dd_out;          /* scaled far clip, float bits */

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

static void read_config(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\HC47Widescreen.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v;
        int b;
        if (sscanf(line, " Enabled = %d", &b) == 1 ||
            sscanf(line, " Enabled=%d", &b) == 1)
            g_enabled = b;
        else if (sscanf(line, " FOVFactor = %f", &v) == 1 ||
                 sscanf(line, " FOVFactor=%f", &v) == 1)
            g_fovfactor = v;
        else if (sscanf(line, " DrawDistanceFactor = %f", &v) == 1 ||
                 sscanf(line, " DrawDistanceFactor=%f", &v) == 1)
            g_ddfactor = v;
        else if (sscanf(line, " Borderless = %d", &b) == 1 ||
                 sscanf(line, " Borderless=%d", &b) == 1)
            g_borderless = b < 0 ? -1 : b != 0;
    }
    fclose(f);
    if (!(g_fovfactor >= 0.5f && g_fovfactor <= 2.0f)) g_fovfactor = 1.0f;
    if (!(g_ddfactor >= 0.25f && g_ddfactor <= 16.0f)) g_ddfactor = 1.0f;
}

static uint8_t *sysiface(void)
{
    HMODULE g = GetModuleHandleA("Globals.dll");
    if (!g) return NULL;
    uint8_t **pp = (uint8_t **)(uintptr_t)GetProcAddress(g,
        "?g_pSysInterface@@3PAVZSysInterface@@A");
    return (pp && *pp) ? *pp : NULL;
}

static int is_wine(void)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"),
                          "wine_get_version") != NULL;
}

static int borderless_wanted(void)
{
    return g_borderless == 1 || (g_borderless == -1 && is_wine());
}

static int desktop_size(int32_t *w, int32_t *h)
{
    *w = GetSystemMetrics(SM_CXSCREEN);
    *h = GetSystemMetrics(SM_CYSCREEN);
    return *w >= 320 && *h >= 200;
}

/* Turn a pending fullscreen request in ZSysInterface into a windowed
 * request at the desktop resolution. Called from the startup guard, from
 * the renderer (re)init hook (in-game options apply), and from the
 * watchdog as a backstop. */
static int convert_to_borderless(uint8_t *si, const char *when)
{
    int32_t dw, dh, w, h;
    if (!si[0x12] || !borderless_wanted() || !desktop_size(&dw, &dh))
        return 0;
    memcpy(&w, si + 0x19, 4);
    memcpy(&h, si + 0x1d, 4);
    si[0x12] = 0;
    memcpy(si + 0x19, &dw, 4);
    memcpy(si + 0x1d, &dh, 4);
    g_borderless_active = 1;
    logf_("borderless: fullscreen %dx%d -> windowed %dx%d "
          "(desktop resolution, %s)", w, h, dw, dh, when);
    return 1;
}

/* Vert- -> Hor+ FOV correction */
static double fov_deg_corrected(double deg)
{
    return atan(tan(deg * M_PI / 360.0) * g_fov_scale) * 360.0 / M_PI;
}

static double fov_rad_corrected(double rad)
{
    return atan(tan(rad * 0.5) * g_fov_scale) * 2.0;
}

/* hook callbacks; regs = pushad block (EDI ESI EBP ESP EBX EDX ECX EAX
 * ascending), original esp = regs + 8 dwords + eflags dword */
void __cdecl cutscene_entry(uint32_t *regs)
{
    uint8_t *orig_esp = (uint8_t *)regs + 36;
    double deg;
    memcpy(&deg, orig_esp + 0x14, 8);
    g_cut_out = fov_deg_corrected(deg) * (M_PI / 180.0);
}

void __cdecl scope_entry(uint32_t *regs)
{
    float v;
    memcpy(&v, (uint8_t *)(uintptr_t)(regs[1] + regs[7] * 4 + 0xd8), 4);
    g_scope_out = (float)fov_rad_corrected((double)v);
}

void __cdecl drawdist_entry(uint32_t *regs)
{
    float v;
    memcpy(&v, (uint8_t *)(uintptr_t)regs[2] + 0x18a, 4);
    v *= g_ddfactor;
    memcpy(&g_dd_out, &v, 4);
}

/* Options-screen init: the game restores the resolution selection (and
 * the values a no-change apply writes back) from its saved profile, not
 * from the mode actually running — a hand-edited hitman.ini shows up as
 * the 800x600 default. Replace the config w/h with the real current mode
 * from ZSysInterface and hand the matcher that width (it goes into EDX,
 * which the replaced instruction loaded the config width into). */
void __cdecl reslist_entry(uint32_t *regs)
{
    uint8_t *obj = (uint8_t *)(uintptr_t)regs[1];    /* esi */
    int32_t w = 0, h = 0;
    uint8_t *si = sysiface();
    if (si) {
        memcpy(&w, si + SI_CURW, 4);
        memcpy(&h, si + SI_CURH, 4);
    }
    if (w >= 320 && h >= 200 && w <= 16384 && h <= 16384) {
        memcpy(obj + 0x11f, &w, 4);
        memcpy(obj + 0x123, &h, 4);
    } else {
        memcpy(&w, obj + 0x11f, 4);                  /* fall back */
    }
    regs[5] = (uint32_t)w;                           /* edx */
}

/* Runs at the renderer's window-create/mode-set path on every (re)init —
 * including the in-game options apply, which re-requests fullscreen when
 * the player toggles it in the menu. */
void __cdecl reinit_entry(uint32_t *regs)
{
    (void)regs;
    uint8_t *si = sysiface();
    if (si)
        convert_to_borderless(si, "renderer re-init");
}

/* stub emission */
static uint8_t *g_stubs;
static uint8_t *g_stub_p;

static int ensure_stubs(void)
{
    if (!g_stubs) {
        g_stubs = (uint8_t *)VirtualAlloc(NULL, 4096,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_stub_p = g_stubs;
    }
    return g_stubs != NULL;
}

static uint8_t *emit_hook_call(uint8_t *p, void *fn)
{
    *p++ = 0x9C;                      /* pushfd */
    *p++ = 0x60;                      /* pushad */
    *p++ = 0x54;                      /* push esp */
    *p++ = 0xE8;                      /* call fn */
    *(int32_t *)p = (int32_t)((uint8_t *)(uintptr_t)fn - (p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;   /* add esp,4 */
    *p++ = 0x61;                      /* popad */
    *p++ = 0x9D;                      /* popfd */
    return p;
}

static uint8_t *emit_jmp(uint8_t *p, uint8_t *tgt)
{
    *p++ = 0xE9;
    *(int32_t *)p = (int32_t)(tgt - (p + 4)); p += 4;
    return p;
}

static int write_code(uint8_t *site, const uint8_t *buf, size_t len)
{
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site, buf, len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

static int hook_site(uint8_t *site, size_t len, uint8_t *stub)
{
    uint8_t buf[16];
    buf[0] = 0xE9;
    *(int32_t *)(buf + 1) = (int32_t)(stub - (site + 5));
    memset(buf + 5, 0x90, len - 5);
    return write_code(site, buf, len);
}

static int module_matches(HMODULE m, uint32_t stamp, uint32_t size)
{
    uint8_t *base = (uint8_t *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == stamp &&
           nt->OptionalHeader.SizeOfImage == size;
}

/* Extend the renderer's static display-mode table (the source of the
 * in-game options resolution list) with the desktop resolution: build a
 * copy with one extra entry and repoint the accessor's imm32 at it. The
 * options screen then lists the desktop resolution, and because the
 * startup conversion put the desktop size into the config, it also opens
 * with that entry selected. */
static void inject_menu_mode(const struct renderer_site *r, uint8_t *base)
{
    int32_t dw, dh;
    if (!borderless_wanted() || !desktop_size(&dw, &dh))
        return;
    uint8_t *getter = base + r->mode_getter;
    int32_t *tab = (int32_t *)(base + r->mode_table);
    uint32_t expect = (uint32_t)(uintptr_t)tab;
    if (getter[0] != 0xb8 || getter[5] != 0xc3 ||
        memcmp(getter + 1, &expect, 4) != 0) {
        logf_("%s: unexpected mode-table accessor — menu entry not added",
              r->module);
        return;
    }
    int n = 0;
    while (n < 32 && tab[n * 4]) {
        if (tab[n * 4] == dw && tab[n * 4 + 1] == dh)
            return;                       /* already in the list */
        n++;
    }
    int32_t *nt = (int32_t *)VirtualAlloc(NULL, (size_t)(n + 2) * 16,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!nt) return;
    memcpy(nt, tab, (size_t)n * 16);
    nt[n * 4] = dw;
    nt[n * 4 + 1] = dh;
    nt[n * 4 + 2] = 32;                   /* terminator stays zeroed */
    uint32_t nv = (uint32_t)(uintptr_t)nt;
    if (write_code(getter + 1, (uint8_t *)&nv, 4))
        logf_("%s: %dx%d added to the options resolution list",
              r->module, dw, dh);
}

/* Disable the resolution-snap ladder in whichever renderer is loaded and
 * put the (re)init hook in its place, so an in-game fullscreen request
 * is converted to borderless before mode selection runs. Also extends
 * the options menu with the desktop resolution. Returns 1 patched, -1
 * mismatch (give up), 0 not loaded yet. On success *which is set to the
 * RENDERERS index (0 = RenderD3D). */
static int patch_renderer_snap(int *which)
{
    for (int i = 0; i < 2; i++) {
        HMODULE m = GetModuleHandleA(RENDERERS[i].module);
        if (!m) continue;
        if (!module_matches(m, RENDERERS[i].timestamp, RENDERERS[i].sizeofimage)) {
            logf_("%s: build mismatch — resolution patch skipped",
                  RENDERERS[i].module);
            return -1;
        }
        uint8_t *site = (uint8_t *)m + RENDERERS[i].rva;
        if (memcmp(site, SNAP_BYTES, sizeof(SNAP_BYTES)) != 0) {
            logf_("%s: unexpected bytes at snap site — resolution patch skipped",
                  RENDERERS[i].module);
            return -1;
        }
        /* je rel32 -> jmp to a stub that converts a fullscreen request,
         * then continues at the je target (skipping the snap ladder) */
        if (!ensure_stubs()) return -1;
        uint8_t *stub = g_stub_p;
        uint8_t *p = emit_hook_call(stub, (void *)reinit_entry);
        p = emit_jmp(p, site + SNAP_SKIP);
        g_stub_p = p;
        if (!hook_site(site, 6, stub)) return -1;
        logf_("%s: resolution snap disabled — hitman.ini resolution passes through",
              RENDERERS[i].module);
        inject_menu_mode(&RENDERERS[i], (uint8_t *)m);
        *which = i;
        return 1;
    }
    return 0;
}

/* ---- resolution guard ------------------------------------------------ */

/* Maximum render-target axis for the legacy Direct3D7 HAL that modern
 * Windows emulates: CreateDevice fails with D3DERR_INVALID_DEVICE above
 * 2048px per axis, windowed and fullscreen alike (verified empirically:
 * 2048x2048 works; 2560x1440, 3840x1080 and 1920x2160 all fail).
 * Wine/CrossOver implements D3D7 natively and has no such limit. */
#define D3D7_MAX_RT_AXIS 2048

/* Which renderer will the game load? Parsed from hitman.ini in the game
 * root (the parent of the scripts directory) so the guard can run before
 * the renderer module even loads — probing DirectDraw concurrently with
 * the game's own DirectDraw startup is not safe. */
static int drawdll_is_d3d(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\..\\Hitman.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    int d3d = 1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "DrawDll", 7) != 0) continue;
        d3d = strstr(p, "OpenGL") == NULL && strstr(p, "3DFX") == NULL;
        break;
    }
    fclose(f);
    return d3d;
}

/* Validate the hitman.ini resolution in ZSysInterface before the renderer
 * runs display-mode selection; clamp it to the best working resolution
 * when it cannot succeed. Runs once, as soon as the parsed resolution
 * shows up in ZSysInterface (well before the renderer module loads). */
static int guard_resolution(uint8_t *si)
{
    int32_t w, h;
    uint8_t fullscreen = si[0x12];
    memcpy(&w, si + 0x19, 4);
    memcpy(&h, si + 0x1d, 4);
    if (w < 320 || h < 200 || w > 16384 || h > 16384)
        return 0;                         /* ini not parsed yet — retry */

    /* fullscreen -> borderless window at the desktop resolution (see the
     * header comment; default on Wine, where exclusive DirectDraw
     * fullscreen is broken under the CrossOver Mac driver) */
    if (fullscreen && convert_to_borderless(si, "startup")) {
        fullscreen = 0;
        memcpy(&w, si + 0x19, 4);
        memcpy(&h, si + 0x1d, 4);
    }

    int32_t limit = drawdll_is_d3d() && !is_wine() ? D3D7_MAX_RT_AXIS
                                                   : 0x7fffffff;

    int32_t bw = 0, bh = 0;               /* best replacement */

    if (fullscreen) {
        /* the mode list the renderer will match against */
        DEVMODEA dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        int exact = 0;
        for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
            if (dm.dmBitsPerPel < 32) continue;
            int32_t mw = (int32_t)dm.dmPelsWidth, mh = (int32_t)dm.dmPelsHeight;
            if (mw == w && mh == h) exact = 1;
            /* candidate ranking: within the limit, prefer the requested
             * aspect, then size */
            if (mw > w || mh > h || mw > limit || mh > limit) continue;
            int cand_asp = mw * h == mh * w;
            int best_asp = bw && bh && bw * h == bh * w;
            if (cand_asp != best_asp ? cand_asp
                                     : (int64_t)mw * mh > (int64_t)bw * bh) {
                bw = mw; bh = mh;
            }
        }
        if (exact && w <= limit && h <= limit)
            return 1;                     /* request is fine as-is */
        if (exact)
            logf_("fullscreen %dx%d exceeds the D3D7 render-target limit "
                  "on this system", w, h);
        else
            logf_("fullscreen %dx%d not in the display mode list", w, h);
    } else {
        if (w <= limit && h <= limit)
            return 1;                     /* windowed size works as-is */
        logf_("windowed %dx%d exceeds the D3D7 render-target limit "
              "on this system", w, h);
        /* shrink aspect-preserving to fit the limit box */
        double f = (double)limit / (w > h ? w : h);
        bw = ((int32_t)(w * f) + 4) & ~7;
        bh = ((int32_t)(h * f) + 4) & ~7;
        if (bw > limit) bw = limit;
        if (bh > limit) bh = limit;
    }

    if (!bw || !bh) {
        logf_("no working replacement resolution found — leaving %dx%d", w, h);
        return 1;
    }
    memcpy(si + 0x19, &bw, 4);
    memcpy(si + 0x1d, &bh, 4);
    logf_("resolution clamped %dx%d -> %dx%d (%s; use the OpenGL renderer "
          "for larger sizes on Windows)", w, h, bw, bh,
          fullscreen ? "fullscreen" : "windowed");
    return 1;
}

/* Strip the game window to a borderless popup covering the desktop.
 * Runs once the renderer has established the display mode (so the window
 * exists); retried by the watchdog until a window is found. The game
 * window is the process's visible top-level window whose client area
 * matches the display mode (fallback: the largest one); every candidate
 * is logged for diagnosis. */
struct wnd_pick {
    HWND best;
    int best_exact;
    int32_t best_area;
    int32_t w, h;                         /* current display mode */
};

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lp)
{
    struct wnd_pick *pick = (struct wnd_pick *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    char cls[64] = "", title[64] = "";
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    RECT r = {0,0,0,0}, c = {0,0,0,0};
    GetWindowRect(hwnd, &r);
    GetClientRect(hwnd, &c);
    int vis = IsWindowVisible(hwnd);
    logf_("  wnd class='%s' title='%s' outer %ldx%ld at %ld,%ld "
          "client %ldx%ld style %08lx/%08lx%s", cls, title,
          (long)(r.right - r.left), (long)(r.bottom - r.top),
          (long)r.left, (long)r.top,
          (long)c.right, (long)c.bottom,
          (unsigned long)GetWindowLongA(hwnd, GWL_STYLE),
          (unsigned long)GetWindowLongA(hwnd, GWL_EXSTYLE),
          vis ? "" : " (hidden)");
    if (!vis || c.right < 320 || c.bottom < 200)
        return TRUE;                      /* skip splash/helper windows */
    int exact = c.right == pick->w && c.bottom == pick->h;
    int32_t area = c.right * c.bottom;
    if (exact > pick->best_exact ||
        (exact == pick->best_exact && area > pick->best_area)) {
        pick->best = hwnd;
        pick->best_exact = exact;
        pick->best_area = area;
    }
    return TRUE;
}

static int apply_borderless(int32_t w, int32_t h)
{
    struct wnd_pick pick = { NULL, 0, 0, w, h };
    logf_("borderless: process windows:");
    EnumWindows(find_game_window, (LPARAM)&pick);
    if (!pick.best) return 0;

    /* The game's windowed path already creates an undecorated popup
     * (that is why hitman.ini has StartUpperPos: the window cannot be
     * dragged) — with the resolution forced to the desktop size it IS a
     * borderless-fullscreen window. Touching the style of a live
     * DirectDraw device window breaks rendering under the CrossOver Mac
     * driver (permanent black window), so intervene as little as
     * possible: never restyle, and only move the window when the client
     * area does not sit at 0,0. */
    POINT origin = { 0, 0 };
    ClientToScreen(pick.best, &origin);
    RECT c;
    GetClientRect(pick.best, &c);
    if (origin.x == 0 && origin.y == 0) {
        logf_("borderless: game window client %ldx%ld already at 0,0 — "
              "left untouched", (long)c.right, (long)c.bottom);
        return 1;
    }
    RECT r;
    GetWindowRect(pick.best, &r);
    SetWindowPos(pick.best, NULL, r.left - origin.x, r.top - origin.y,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    logf_("borderless: moved game window client (was at %ld,%ld) to 0,0",
          (long)origin.x, (long)origin.y);
    return 1;
}

static int g_push_ok;   /* push sites verified, safe to (re)write */

/* Rewrite the 67.4-degree immediates for the current g_fov_scale. */
static void apply_fov_pushsites(uint8_t *base)
{
    if (!g_push_ok) return;
    double corrected = fov_deg_corrected(ORIG_FOV_DEG);
    for (int i = 0; i < 6; i++) {
        double v = i == 0 ? corrected * (double)g_fovfactor : corrected;
        uint64_t bits;
        memcpy(&bits, &v, 8);
        uint32_t hi = (uint32_t)(bits >> 32), lo = (uint32_t)bits;
        uint8_t *site = base + FOV_PUSH_RVAS[i];
        write_code(site + 1, (uint8_t *)&hi, 4);
        write_code(site + 6, (uint8_t *)&lo, 4);
    }
    logf_("camera FOV %.2f -> %.2f deg (gameplay cam %.2f, factor %.2f)",
          ORIG_FOV_DEG, corrected, corrected * (double)g_fovfactor,
          (double)g_fovfactor);
}

/* One-time HitmanDlc.dlc patches: verify build, verify the six push
 * sites, install the cutscene/scope/draw-distance entry hooks.
 * Returns 1 on success, -1 on mismatch (give up). */
static int patch_dlc(uint8_t *base)
{
    if (!module_matches((HMODULE)base, DLC_TIMESTAMP, DLC_SIZEOFIMAGE)) {
        logf_("HitmanDlc.dlc build mismatch — FOV patches skipped");
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (memcmp(base + FOV_PUSH_RVAS[i], PUSH_BYTES, 10) != 0) {
            logf_("FOV push site %d: unexpected bytes — FOV patches skipped", i);
            return -1;
        }
    }
    g_push_ok = 1;

    if (!ensure_stubs()) return -1;

    /* cutscene: replace `fld qword [esp+0x14]; fmul deg2rad` with a hook
     * that computes the corrected value in radians, then fld it */
    uint8_t *site = base + CUTSCENE_RVA;
    uint32_t deg2rad_va = (uint32_t)(uintptr_t)(base + DEG2RAD_RVA);
    if (memcmp(site, CUT_BYTES, sizeof(CUT_BYTES)) == 0 &&
        memcmp(site + 6, &deg2rad_va, 4) == 0) {
        uint8_t *stub = g_stub_p;
        uint8_t *p = emit_hook_call(stub, (void *)cutscene_entry);
        *p++ = 0xDD; *p++ = 0x05;              /* fld qword [g_cut_out] */
        *(uint32_t *)p = (uint32_t)(uintptr_t)&g_cut_out; p += 4;
        p = emit_jmp(p, site + CUT_SITE_LEN);
        g_stub_p = p;
        if (hook_site(site, CUT_SITE_LEN, stub))
            logf_("cutscene FOV hook installed");
    } else {
        logf_("cutscene FOV: unexpected bytes — skipped");
    }

    /* scope: replace `fld dword [esi+eax*4+0xd8]` at both zoom sites */
    static const uint32_t scope_rvas[2] = { SCOPE1_RVA, SCOPE2_RVA };
    for (int i = 0; i < 2; i++) {
        site = base + scope_rvas[i];
        if (memcmp(site, SCOPE_BYTES, sizeof(SCOPE_BYTES)) != 0) {
            logf_("scope FOV site %d: unexpected bytes — skipped", i);
            continue;
        }
        uint8_t *stub = g_stub_p;
        uint8_t *p = emit_hook_call(stub, (void *)scope_entry);
        *p++ = 0xD9; *p++ = 0x05;              /* fld dword [g_scope_out] */
        *(uint32_t *)p = (uint32_t)(uintptr_t)&g_scope_out; p += 4;
        p = emit_jmp(p, site + sizeof(SCOPE_BYTES));
        g_stub_p = p;
        if (hook_site(site, sizeof(SCOPE_BYTES), stub))
            logf_("scope FOV hook %d installed", i);
    }

    /* options-screen resolution selection: replace the config-width load
     * so the menu reflects (and re-applies) the actual current mode */
    site = base + RESSEL_RVA;
    if (memcmp(site, RESSEL_BYTES, sizeof(RESSEL_BYTES)) == 0) {
        uint8_t *stub = g_stub_p;
        uint8_t *p = emit_hook_call(stub, (void *)reslist_entry);
        p = emit_jmp(p, site + sizeof(RESSEL_BYTES));
        g_stub_p = p;
        if (hook_site(site, sizeof(RESSEL_BYTES), stub))
            logf_("options-screen resolution selection hook installed");
    } else {
        logf_("options-screen selection: unexpected bytes — skipped");
    }

    /* draw distance: replace `mov eax,[ebp+0x18a]` only when scaling */
    if (g_ddfactor != 1.0f) {
        site = base + DRAWDIST_RVA;
        if (memcmp(site, DD_BYTES, sizeof(DD_BYTES)) == 0) {
            uint8_t *stub = g_stub_p;
            uint8_t *p = emit_hook_call(stub, (void *)drawdist_entry);
            *p++ = 0xA1;                       /* mov eax,[g_dd_out] */
            *(uint32_t *)p = (uint32_t)(uintptr_t)&g_dd_out; p += 4;
            p = emit_jmp(p, site + sizeof(DD_BYTES));
            g_stub_p = p;
            if (hook_site(site, sizeof(DD_BYTES), stub))
                logf_("draw distance factor %.2f installed",
                      (double)g_ddfactor);
        } else {
            logf_("draw distance: unexpected bytes — skipped");
        }
    }
    return 1;
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    int snap_state = 0, dlc_state = 0;
    int snap_tries = 0;
    int renderer = -1, guarded = 0;
    (void)renderer;
    int borderless_done = 0;
    int32_t lastw = 0, lasth = 0;
    for (;;) {
        int settled = snap_state != 0 && dlc_state != 0;
        Sleep(settled ? 250 : guarded ? 25 : 5);
        uint8_t *si = sysiface();
        /* guard first: it must rewrite the requested resolution before
         * the renderer creates its window, and its DirectDraw probe must
         * not run concurrently with the game's own DirectDraw startup —
         * both are safe this early because the renderer module has not
         * been loaded yet when the parsed ini resolution appears */
        if (si && !guarded)
            guarded = guard_resolution(si);
        /* backstop: if something re-requested fullscreen through a path
         * the renderer (re)init hook does not cover, convert it here */
        if (si && guarded)
            convert_to_borderless(si, "watchdog");
        if (snap_state == 0) {
            snap_state = patch_renderer_snap(&renderer);
            if (snap_state == 0 && ++snap_tries > 1600) {
                logf_("no supported renderer module after 40s — resolution patch skipped");
                snap_state = -1;
            }
        }
        if (!si) continue;
        int32_t w, h;
        memcpy(&w, si + SI_CURW, 4);
        memcpy(&h, si + SI_CURH, 4);
        if (w < 320 || h < 200 || w > 16384 || h > 16384)
            continue;               /* mode not established yet */
        int changed = w != lastw || h != lasth;
        if (changed) {
            lastw = w; lasth = h;
            borderless_done = 0;
        }
        /* mode established -> the renderer window exists; retried until
         * EnumWindows finds it, re-run when the mode changes */
        if (g_borderless_active && !borderless_done)
            borderless_done = apply_borderless(w, h);
        if (!changed)
            continue;
        g_fov_scale = ((double)w / (double)h) / (4.0 / 3.0);
        logf_("mode %dx%d, aspect scale %.4f", w, h, g_fov_scale);
        if (dlc_state == 0) {
            HMODULE dlc = GetModuleHandleA("HitmanDlc.dlc");
            if (!dlc) { lastw = lasth = 0; continue; }
            dlc_state = patch_dlc((uint8_t *)dlc);
        }
        if (dlc_state > 0)
            apply_fov_pushsites((uint8_t *)GetModuleHandleA("HitmanDlc.dlc"));
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
        snprintf(logpath, sizeof(logpath), "%s\\HC47Widescreen.log", g_dir);
        g_log = fopen(logpath, "w");
        read_config();
        logf_("HC47 Widescreen loaded%s, FOVFactor=%.2f DrawDistanceFactor=%.2f",
              g_enabled ? "" : " (disabled)",
              (double)g_fovfactor, (double)g_ddfactor);
        if (g_enabled)
            CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    }
    return TRUE;
}
