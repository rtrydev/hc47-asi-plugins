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
 * A watchdog thread applies the renderer patch as soon as the renderer
 * module appears, applies the HitmanDlc patches once the display mode is
 * known, and rewrites the FOV constants if the mode changes at run time.
 *
 * Config: scripts/HC47Widescreen.ini
 *   [Widescreen]
 *   Enabled=1
 *   FOVFactor=1.0          ; extra factor on the gameplay camera FOV
 *   DrawDistanceFactor=1.0 ; 1.0 leaves the draw distance untouched
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
static const uint8_t CUT_BYTES[6]   = { 0xdd, 0x44, 0x24, 0x14, 0xdc, 0x0d };
#define CUT_SITE_LEN 10
static const uint8_t SCOPE_BYTES[7] = { 0xd9, 0x84, 0x86, 0xd8, 0x00, 0x00, 0x00 };
static const uint8_t DD_BYTES[6]    = { 0x8b, 0x85, 0x8a, 0x01, 0x00, 0x00 };

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
 * `je +0x105; mov eax,[edx+0x19]; cmp eax,0x200; jge` in both. */
static const uint8_t SNAP_BYTES[16] = {
    0x0f, 0x84, 0x05, 0x01, 0x00, 0x00, 0x8b, 0x42, 0x19,
    0x3d, 0x00, 0x02, 0x00, 0x00, 0x7d, 0x0f
};
static const struct renderer_site {
    const char *module;
    uint32_t timestamp, sizeofimage, rva;
} RENDERERS[2] = {
    { "RenderD3D.dll",    0x3a3e1338, 0x4d000, 0x29363 },
    { "RenderOpenGL.dll", 0x3a3e1318, 0x56000, 0x2b913 },
};

static FILE *g_log;
static char g_dir[MAX_PATH];
static int g_enabled = 1;
static float g_fovfactor = 1.0f;
static float g_ddfactor = 1.0f;

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

/* stub emission */
static uint8_t *g_stubs;
static uint8_t *g_stub_p;

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

/* Make the resolution-snap `je` unconditional in whichever renderer is
 * loaded. Returns 1 patched, -1 mismatch (give up), 0 not loaded yet.
 * On success *which is set to the RENDERERS index (0 = RenderD3D). */
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
        /* je rel32 -> jmp rel32 (one byte shorter) + nop */
        static const uint8_t jmp6[6] = { 0xe9, 0x06, 0x01, 0x00, 0x00, 0x90 };
        if (!write_code(site, jmp6, 6)) return -1;
        logf_("%s: resolution snap disabled — hitman.ini resolution passes through",
              RENDERERS[i].module);
        *which = i;
        return 1;
    }
    return 0;
}

/* ---- resolution guard ------------------------------------------------ */

static int is_wine(void)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"),
                          "wine_get_version") != NULL;
}

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

    g_stubs = (uint8_t *)VirtualAlloc(NULL, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) return -1;
    g_stub_p = g_stubs;

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
        if (w == lastw && h == lasth)
            continue;
        lastw = w; lasth = h;
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
