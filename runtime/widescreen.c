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
 *    through unmodified.
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
 *  2. On modern Windows the legacy D3D7 HAL refuses render targets
 *     larger than 2048px per axis. Wine/CrossOver has no such limit.
 * So before the renderer runs mode selection, the requested resolution
 * in ZSysInterface is validated and clamped to the best resolution that
 * works when a check fails.
 *
 * Borderless fullscreen (default on Wine/CrossOver): exclusive DirectDraw
 * fullscreen is broken under the CrossOver Mac driver — the emulated mode
 * change renders oversized on scaled Retina desktops, exclusive-mode
 * cursor clipping freezes the mouse, and alt-tab loses the exclusive
 * surfaces. The windowed path has none of these problems, so a fullscreen
 * request is converted before the renderer runs: the ZSysInterface
 * fullscreen flag (+0x12) is cleared and the request becomes a windowed
 * mode (see aspect handling below). The game's own windowed path already
 * creates an undecorated popup window; its style is never touched —
 * restyling a live DirectDraw device window blacks out rendering under
 * the CrossOver Mac driver.
 *
 * Aspect preservation (PreserveAspectRatio, default on): rendering at the
 * desktop size means the desktop's aspect ratio, not the one the player
 * picked. When they differ, the game MODE becomes the largest rectangle
 * with the requested aspect that fits the desktop, and the letterboxing
 * happens entirely inside the game's own window and present path — no
 * helper windows exist at all (an earlier design used backdrop + cover
 * windows and lost the fight against the macOS window manager: Cocoa
 * clamps window tops below the menu bar, boundary rows render as
 * hairlines, z-order churn stutters):
 *  1. The game window is sized to cover the whole desktop (client at
 *     0,0, desktop-sized) by rewriting the arguments of the single
 *     MoveWindow call that ever sizes it (RenderD3D 0x2959c, inside the
 *     window attach/restyle fn 0x29320 that runs on every (re)init) —
 *     before any DirectDraw surface exists, so nothing is live yet and
 *     the WS_EX_CLIENTEDGE ring hangs entirely off-screen (that ring is
 *     the classic "white stripes at the image edges": at any position
 *     other than 0,0/desktop-size it pokes into view, and it cannot be
 *     restyled away on a live window). The render-size setter (vtbl
 *     +0xa8, fn 0x24ac0) — fed the client size right after the resize
 *     and again on every WM_SIZE — is clamped to the fit mode, keeping
 *     the engine's relative-mouse normalization consistent with the
 *     backbuffer (the cursor itself is WM_MOUSEMOVE-delta based with a
 *     recenter warp; the reference point cancels, so a client larger
 *     than the mode needs no coordinate compensation).
 *  2. The same setup captures the client rect (screen coords) into the
 *     render object at +0xbdd — the per-frame present blits the WHOLE
 *     backbuffer to exactly that rect (primary->Blt at rva 0x7838) — and
 *     stores the client size at +0xc05/+0xc09, which sizes the windowed
 *     BACKBUFFER and the D3D viewport. A hook at the capture tail (rva
 *     0x70b7) rewrites the rect to the centered aspect-fit rectangle and
 *     the sizes to the fit mode, so the backbuffer/viewport match the
 *     mode the engine believes it runs at (ZSysInterface +0x21/+0x25)
 *     and the blit lands 1:1 in the centered rect.
 *  3. The bar areas are pixels of the game window's own client area that
 *     the game blit never touches: a hook at the present entry (rva
 *     0x7770) black-fills them with DDBLT_COLORFILL blits on the primary
 *     surface (clipped to the game window by the clipper the game itself
 *     attached) — as a short burst after every mode init plus a periodic
 *     one-shot refresh from the watchdog. NOT every frame: each Blt on
 *     the clipped primary is a heavyweight present under the CrossOver
 *     Mac driver (4 strips per frame measured 60fps -> ~22). Ordinary
 *     invalidation is already covered for free: the window class
 *     background is BLACK_BRUSH, so GDI erases paint the bars black.
 *     The image rectangle itself is confined to the desktop minus the
 *     macOS menu-bar/notch band (the work area's top inset only — the
 *     full work area also excludes the Dock, which merely overlays and
 *     must stay covered by the bars, not avoided).
 * RenderOpenGL presents window-relative and has no equivalent hook
 * points; under OpenGL a converted fullscreen request keeps the old
 * fill-the-desktop behavior (no letterboxing).
 *
 * In-game applies (resolution change or fullscreen toggle in the options
 * screen) do NOT go through the startup path: the options code calls the
 * renderer interface's SetResolution (vtbl+0xc, RenderD3D 0x298d0 /
 * RenderOpenGL 0x2be80) with the picked {w,h,bpp,fullscreen}, which
 * stages the request in a separate pending set — mode to si+0x21/+0x25,
 * bpp to +0x2d, fullscreen to si+0x13 — and raises the re-init dirty
 * byte si+0x38ed that the frame loop consumes. So SetResolution is
 * replaced wholesale with a reimplementation that runs the borderless
 * conversion on the staged values first; the mode-change re-init then
 * re-runs the windowed surface setup, which re-applies the window resize
 * and the rect/size overrides automatically.
 *
 * The options screen composes with all of this: the resolution list, its
 * "%d x %d" labels and the applied values all come from a static mode
 * table in the renderer, replaced via its accessor (see
 * ModernResolutionList below); the selection hook reports the resolution
 * the player actually picked (not the letterbox fit size) and resolves
 * the exact width+height entry itself (the stock matcher compares widths
 * only, which mis-highlights lists carrying 1920x1080 and 1920x1200).
 *
 * Settings persistence: the game rewrites hitman.ini itself (options
 * apply and exit) through a single EngineData.dll function (ZConfigFile
 * vtbl+0x84, rva 0x19b0) that formats "Resolution %dx%d" straight from
 * si+0x19/+0x1d and derives the "Window" line from si+0x12 (or the
 * config object's override dword at +0xf1) — i.e. from the CONVERTED
 * values, so the player's real preference used to be clobbered on every
 * save ("Window" + desktop/fit resolution). The writer is wrapped: the
 * picked resolution and fullscreen preference are swapped into those
 * fields for the duration of the write and the live values restored
 * after, so hitman.ini always round-trips what the player chose.
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
 *   PreserveAspectRatio=1  ; borderless keeps the aspect ratio of the
 *                          ; chosen resolution, centered with black
 *                          ; bars; 0 stretches to the desktop aspect
 *   ModernResolutionList=1 ; replace the options menu's 4:3 mode table
 *                          ; with common modern resolutions; 0 keeps
 *                          ; the stock list
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
 * `mov eax, imm32; ret` the game reaches it through. */
static const uint8_t SNAP_BYTES[16] = {
    0x0f, 0x84, 0x05, 0x01, 0x00, 0x00, 0x8b, 0x42, 0x19,
    0x3d, 0x00, 0x02, 0x00, 0x00, 0x7d, 0x0f
};
#define SNAP_SKIP 0x10b   /* je target, relative to the snap site */
static const struct renderer_site {
    const char *module;
    uint32_t timestamp, sizeofimage, rva;
    uint32_t mode_getter, mode_table;
    uint32_t setres;         /* renderer-interface vtbl+0xc: the in-game
                                options apply calls it with the picked
                                {w,h,bpp,fullscreen}; it stores the pending
                                mode (si+0x21/+0x25/+0x2d), the pending
                                fullscreen flag (si+0x13!) and a dirty byte
                                (si+0x38ed) the frame loop consumes to
                                re-init — completely bypassing si+0x12/
                                +0x19/+0x1d, so the borderless conversion
                                must happen HERE, synchronously */
    uint32_t setres_glob;    /* the global holding &ZSysInterface that its
                                first instruction loads (absolute operand,
                                byte-checked against the live base) */
    uint32_t movewin;        /* the one MoveWindow call (inside the window
                                attach/restyle fn 0x29320, which runs on
                                every (re)init after the styles are set
                                and before any surface exists): the ONLY
                                place the game window is ever sized. The
                                hook rewrites the pushed x/y/w/h so the
                                client covers the desktop while
                                letterboxing. 0 = no letterbox support */
    uint32_t movewin_thunk;  /* MoveWindow import thunk (the call's
                                absolute operand, byte-checked against
                                the live base) */
    uint32_t sizeset;        /* render-size setter (vtbl+0xa8): stores
                                w/h to [rend+0x83]/[rend+0x87], which
                                feed the relative-mouse math. Called with
                                the CLIENT size after the resize and on
                                every WM_SIZE, so while letterboxing the
                                hook clamps the arguments to the fit mode
                                or mouse sensitivity would skew */
    uint32_t capture;        /* tail of the client-rect capture inside the
                                setup: RECT at [obj+0xbdd] (screen coords,
                                the present's blit destination) and client
                                w/h at [obj+0xc05]/[obj+0xc09] (windowed
                                backbuffer + viewport size) were just
                                stored; the hook overwrites them with the
                                centered fit rect / the fit mode size */
    uint32_t present;        /* per-frame present entry (thiscall, byte
                                arg): windowed path blits the backbuffer
                                to [obj+0xbdd] on the primary ([obj+0x95d],
                                clipper-attached); the hook colorfills the
                                letterbox bars first */
} RENDERERS[2] = {
    { "RenderD3D.dll",    0x3a3e1338, 0x4d000, 0x29363, 0x298c0, 0x3d178,
      0x298d0, 0x35030, 0x2959c, 0x3515c, 0x24ac0, 0x70b7, 0x7770 },
    { "RenderOpenGL.dll", 0x3a3e1318, 0x56000, 0x2b913, 0x2be70, 0x36d28,
      0x2be80, 0x2f028, 0, 0, 0, 0, 0 },
};

/* SetResolution after the `mov eax,[glob]` the hook replaces:
 * mov eax,[eax]; mov edx,[eax+0x19]; push ebx; push esi;
 * mov esi,[esp+0xc]; mov ecx,[esi] */
static const uint8_t SETRES_TAIL[12] = {
    0x8b, 0x00, 0x8b, 0x50, 0x19, 0x53, 0x56,
    0x8b, 0x74, 0x24, 0x0c, 0x8b
};

/* Render-size setter (vtbl+0xa8, rva 0x24ac0), the complete function:
 * mov eax,[esp+4]; mov edx,[esp+8]; mov [ecx+0x83],eax;
 * mov [ecx+0x87],edx; ret 8. The hook steals the two 4-byte loads. */
static const uint8_t SIZESET_BYTES[23] = {
    0x8b, 0x44, 0x24, 0x04, 0x8b, 0x54, 0x24, 0x08,
    0x89, 0x81, 0x83, 0x00, 0x00, 0x00,
    0x89, 0x91, 0x87, 0x00, 0x00, 0x00,
    0xc2, 0x08, 0x00
};
#define SIZESET_HOOK_LEN 8

/* Capture tail (rva 0x70b7): the two size stores precede it, then
 * xor eax,eax; push 0; mov ecx,0x1f (memset of the DDSURFACEDESC2 —
 * all position-independent, replayed in the stub; the push 0 is a live
 * CreateSurface argument). */
#define OBJ_RECT    0xbdd  /* RECT, screen coords: present blit dest */
#define OBJ_W       0xc05  /* captured client w -> backbuffer/viewport */
#define OBJ_H       0xc09
#define OBJ_PRIMARY 0x95d  /* IDirectDrawSurface7* primary */
#define OBJ_EXCL    0xbfd  /* fullscreen-exclusive flag */
static const uint8_t CAPTURE_PRE[12] = {   /* site-12: the two stores */
    0x89, 0x8e, 0x09, 0x0c, 0x00, 0x00,    /* mov [esi+0xc09],ecx */
    0x89, 0x86, 0x05, 0x0c, 0x00, 0x00     /* mov [esi+0xc05],eax */
};
static const uint8_t CAPTURE_BYTES[9] = {
    0x33, 0xc0, 0x6a, 0x00, 0xb9, 0x1f, 0x00, 0x00, 0x00
};

/* Per-frame present entry (rva 0x7770): push esi / mov esi,ecx /
 * mov eax,[esi+0x95d] / test eax,eax / jnz ... — the hook steals the
 * first 9 (position-independent) bytes. */
static const uint8_t PRESENT_BYTES[22] = {
    0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x5d, 0x09, 0x00, 0x00,
    0x85, 0xc0, 0x75, 0x09, 0xb8, 0x0f, 0x00, 0x00, 0x82,
    0x5e, 0xc2, 0x04, 0x00
};
#define PRESENT_HOOK_LEN 9

#define DDBLT_COLORFILL 0x00000400
#define DDBLT_WAIT      0x01000000

/* EngineData.dll, retail build: the hitman.ini writer (ZConfigFile
 * vtbl+0x84, rva 0x19b0) — the single function through which every
 * hitman.ini rewrite goes (options-screen applies and game exit). It
 * formats "Resolution %dx%d" from si+0x19/+0x1d and decides the
 * "Window" line from the config object's override dword at +0xf1
 * (0: use the si+0x12 fullscreen byte; 1: windowed; >=2: fullscreen).
 * Those are exactly the fields the borderless conversion rewrites, so
 * without intervention the game saves the converted values and the
 * player's picked resolution/fullscreen preference is lost (observed:
 * hitman.ini clobbered to "Window" + desktop resolution). */
#define ED_TIMESTAMP    0x3a3e12f7
#define ED_SIZEOFIMAGE  0x3f000
#define INIWRITE_RVA    0x19b0
#define ED_SYSIF_SLOT   0x30000   /* import slot the writer's `mov eax`
                                     loads &g_pSysInterface from (its
                                     absolute operand, rebased at load) */
#define CFG_FSOVERRIDE  0xf1      /* dword in the ZConfigFile object */
static const uint8_t INIWRITE_HEAD[6] = {
    0x81, 0xec, 0xd8, 0x01, 0x00, 0x00          /* sub esp,0x1d8 */
};
static const uint8_t INIWRITE_TAIL[10] = {      /* after mov eax,[slot] */
    0x8b, 0x00, 0x53, 0x89, 0x4c, 0x24, 0x08, 0x8a, 0x48, 0x50
};

static FILE *g_log;
static char g_dir[MAX_PATH];
static int g_enabled = 1;
static float g_fovfactor = 1.0f;
static float g_ddfactor = 1.0f;
static int g_borderless = -1;        /* -1 auto (Wine), 0 never, 1 always */
static int g_borderless_active;      /* fullscreen request was converted */
static int g_preserve_aspect = 1;    /* borderless: letterbox to the
                                        requested aspect ratio */
static int g_modern_list = 1;        /* options menu: replace the stock
                                        4:3 mode table with common modern
                                        resolutions */
static int g_letterbox_ok;           /* the loaded renderer supports the
                                        in-window letterbox (RenderD3D) */

static int32_t g_req_w, g_req_h;     /* the resolution the player chose
                                        (before any borderless conversion
                                        replaced it) */
static int g_req_fs;                 /* the player asked for fullscreen */
static int g_letterbox;              /* fit != desktop: bars are showing */
static int32_t g_fit_x, g_fit_y;     /* centered fit rect, screen coords */
static int32_t g_fit_w, g_fit_h;     /* (client at 0,0 desktop-sized, so
                                        screen == client coordinates) */

static HWND g_gamewnd;               /* picked by apply_borderless */
static volatile LONG g_bar_fills;    /* presents that still colorfill the
                                        bars: a burst after every mode
                                        init, then a periodic one-shot
                                        from the watchdog — per-frame
                                        fills cost real frame rate (each
                                        Blt on the clipped primary is a
                                        heavyweight present under the
                                        CrossOver Mac driver; 4 strips
                                        per frame took 60fps to ~22) */

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
        else if (sscanf(line, " PreserveAspectRatio = %d", &b) == 1 ||
                 sscanf(line, " PreserveAspectRatio=%d", &b) == 1)
            g_preserve_aspect = b != 0;
        else if (sscanf(line, " ModernResolutionList = %d", &b) == 1 ||
                 sscanf(line, " ModernResolutionList=%d", &b) == 1)
            g_modern_list = b != 0;
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

/* Which renderer will the game load? Parsed from hitman.ini in the game
 * root (the parent of the scripts directory) so decisions can be made
 * before the renderer module even loads. */
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

/* Can the letterbox actually be rendered? Only RenderD3D has the capture/
 * present hook points; before the module loads, go by hitman.ini. */
static int letterbox_possible(void)
{
    return g_preserve_aspect && drawdll_is_d3d();
}

/* Usable screen area for a letterboxed image: the desktop minus the
 * menu-bar/notch band at the top. On notched MacBook displays that band
 * (~40px) PHYSICALLY occludes pixels, and an image centered on the full
 * desktop pokes into it whenever the bars are thinner than the band
 * (seen with 16:10 picks: 22px bars, image top under the notch). Only
 * the work area's TOP inset is honored — winemac's SPI_GETWORKAREA also
 * excludes the Dock and side panels, but those merely overlay the
 * desktop (fitting into the full work area shrank the image and drew a
 * giant bar across the dock zone). The window and the black bars still
 * cover the whole desktop. */
static void image_area(int32_t dw, int32_t dh, RECT *ia)
{
    RECT r;
    ia->left = 0;
    ia->top = 0;
    ia->right = dw;
    ia->bottom = dh;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0) &&
        r.top > 0 && r.top < dh / 4)
        ia->top = r.top;
}

/* Mode for a converted fullscreen request: the desktop, or with
 * PreserveAspectRatio the largest rectangle with the requested aspect
 * that fits the work area (centered by the capture hook, bars filled by
 * the present hook). */
static void borderless_fit(int32_t w, int32_t h, int32_t dw, int32_t dh,
                           int32_t *tw, int32_t *th)
{
    *tw = dw;
    *th = dh;
    if (!letterbox_possible() || w < 320 || h < 200)
        return;
    /* a request that nearly fills the desktop fills it outright (like
     * the desktop-resolution pick): no bars, notch band covered by the
     * image just as without aspect preservation */
    int32_t fw = dw, fh = (int32_t)((double)dw * h / w + 0.5);
    if (fh > dh) {
        fh = dh;
        fw = (int32_t)((double)dh * w / h + 0.5);
    }
    if (dw - fw <= 16 && dh - fh <= 16)
        return;
    /* letterbox: largest requested-aspect rectangle below the notch band */
    RECT wa;
    image_area(dw, dh, &wa);
    int32_t bw = wa.right - wa.left, bh = wa.bottom - wa.top;
    *tw = bw;
    *th = (int32_t)((double)bw * h / w + 0.5);
    if (*th > bh) {
        *th = bh;
        *tw = (int32_t)((double)bh * w / h + 0.5);
    }
    *tw &= ~1;
    *th &= ~1;
}

/* Record a conversion's outcome: what the player picked (for the options
 * screen and for saving) and the letterbox geometry (for the capture and
 * present hooks). */
static void borderless_record(int32_t w, int32_t h, int32_t tw, int32_t th,
                              int32_t dw, int32_t dh, const char *when)
{
    g_req_w = w;
    g_req_h = h;
    g_req_fs = 1;
    g_letterbox = tw != dw || th != dh;
    g_fit_w = tw;
    g_fit_h = th;
    g_fit_x = 0;
    g_fit_y = 0;
    if (g_letterbox) {
        RECT wa;
        image_area(dw, dh, &wa);
        g_fit_x = wa.left + (wa.right - wa.left - tw) / 2;
        g_fit_y = wa.top + (wa.bottom - wa.top - th) / 2;
        if (g_fit_x < 0) g_fit_x = 0;
        if (g_fit_y < 0) g_fit_y = 0;
        logf_("borderless: fullscreen %dx%d -> windowed %dx%d at %d,%d "
              "(image area top %ld on %dx%d desktop, black bars) (%s)",
              w, h, tw, th, g_fit_x, g_fit_y, (long)wa.top, dw, dh, when);
    } else {
        logf_("borderless: fullscreen %dx%d -> windowed %dx%d on %dx%d "
              "desktop (%s)", w, h, tw, th, dw, dh, when);
    }
    g_borderless_active = 1;
}

/* Turn a pending fullscreen request in ZSysInterface into a windowed
 * request. Called from the startup guard, from the renderer (re)init
 * hook, and from the watchdog as a backstop. */
static int convert_to_borderless(uint8_t *si, const char *when)
{
    int32_t dw, dh, w, h;
    if (!si[0x12] || !borderless_wanted() || !desktop_size(&dw, &dh))
        return 0;
    memcpy(&w, si + 0x19, 4);
    memcpy(&h, si + 0x1d, 4);
    int32_t tw, th;
    borderless_fit(w, h, dw, dh, &tw, &th);
    si[0x12] = 0;
    memcpy(si + 0x19, &tw, 4);
    memcpy(si + 0x1d, &th, 4);
    borderless_record(w, h, tw, th, dw, dh, when);
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

/* Options-screen sync (vtbl+0xdc, runs on every screen open): the config
 * block was just filled from LIVE ZSysInterface +0x19/+0x1d — under this
 * plugin stack that is HC47HudScale's virtual GUI size, and under
 * borderless the mode is the converted fit anyway, so neither is what
 * the player picked. This hook owns the selection completely:
 *  - resolve the picked resolution to a table entry (exact match, else
 *    nearest — never unresolved) and write THE ENTRY into the config
 *    (+0x11f/+0x123; the Apply button passes that block to SetResolution
 *    verbatim) and the entry index into +0xe7 (the slider position the
 *    stock tail SetMax/SetPos code applies),
 *  - hand the stock width-match loop a width that matches nothing, so it
 *    only counts entries for SetMax and cannot overwrite the index (its
 *    no-match exit writes nothing — verified),
 *  - write the "%d x %d" label text directly: the only code that ever
 *    sets it is the slider-drag handler, which a programmatic SetPos
 *    does not reliably trigger — without this the label keeps showing
 *    the layout's stale placeholder ("640 x 480") no matter what is
 *    selected or applied,
 *  - reflect the player's fullscreen preference in the config flag
 *    (+0x12b) that feeds the Full screen checkbox and the Apply block. */
void __cdecl reslist_entry(uint32_t *regs)
{
    uint8_t *obj = (uint8_t *)(uintptr_t)regs[1];    /* esi: screen object */
    int32_t *tab = (int32_t *)(uintptr_t)regs[7];    /* eax: mode table */
    regs[5] = 0x7fffffffu;                           /* edx: count only */
    int32_t w = 0, h = 0;
    uint8_t *si = sysiface();
    if (si) {
        memcpy(&w, si + SI_CURW, 4);
        memcpy(&h, si + SI_CURH, 4);
    }
    if (g_borderless_active && g_req_w >= 320 && g_req_h >= 200) {
        w = g_req_w;                     /* what the player picked */
        h = g_req_h;
    }
    if (!tab || w < 320 || h < 200 || w > 16384 || h > 16384)
        return;
    int sel = -1, exact = 0;
    int64_t best = -1;
    for (int i = 0; i < 64 && tab[i * 4]; i++) {
        int32_t ew = tab[i * 4], eh = tab[i * 4 + 1];
        if (ew == w && eh == h) {
            sel = i;
            exact = 1;
        } else if (!exact) {
            int64_t d = (int64_t)(ew - w) * (ew - w) +
                        (int64_t)(eh - h) * (eh - h);
            if (best < 0 || d < best) {
                best = d;
                sel = i;
            }
        }
    }
    if (sel < 0)
        return;                          /* empty table: leave everything */
    memcpy(obj + 0x11f, &tab[sel * 4], 4);
    memcpy(obj + 0x123, &tab[sel * 4 + 1], 4);
    memcpy(obj + 0xe7, &sel, 4);
    if (g_borderless_active && g_req_fs) {
        int32_t one = 1;
        memcpy(obj + 0x12b, &one, 4);    /* Full screen checkbox + Apply */
    }
    uint8_t *ctrl;
    memcpy(&ctrl, obj + 0x103, 4);       /* the resolution text control */
    if (ctrl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d x %d", tab[sel * 4], tab[sel * 4 + 1]);
        typedef void(__thiscall * settext_t)(void *, const char *, int);
        settext_t settext = (settext_t)(*(void ***)ctrl)[0x4e0 / 4];
        settext(ctrl, buf, 0);
    }
    logf_("options screen: selection %d = %dx%d%s%s", sel,
          tab[sel * 4], tab[sel * 4 + 1], exact ? "" : " (nearest match)",
          ctrl ? ", label set" : "");
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

/* Replaces the renderer interface's SetResolution (vtbl+0xc) wholesale —
 * the in-game options apply hands it &{w,h,bpp,fullscreen} and it stages
 * the request for the frame loop's re-init: pending mode to si+0x21/+0x25
 * (each field set to the CURRENT +0x19/+0x1d/+0x29 value when unchanged),
 * pending bpp to +0x2d, pending fullscreen to si+0x13, and the re-init
 * dirty byte si+0x38ed when anything changed. None of that touches
 * si+0x12/+0x19/+0x1d, so the startup-style conversion never sees a
 * runtime fullscreen request — it must be converted right here, before
 * it is staged, or the re-init enters the exclusive DirectDraw fullscreen
 * that is broken under the CrossOver Mac driver. The caller's config is
 * left alone (the menu keeps showing the resolution the player picked);
 * only the staged values are converted. */
void __cdecl setres_entry(uint32_t *regs)
{
    uint8_t *orig_esp = (uint8_t *)regs + 36;
    int32_t *req;
    memcpy(&req, orig_esp + 4, 4);       /* arg: {w,h,bpp,fullscreen} */
    uint8_t *si = sysiface();
    if (!req || !si)
        return;
    int32_t w = req[0], h = req[1], bpp = req[2];
    int fs = req[3] != 0;
    logf_("in-game mode request %dx%d bpp %d %s", w, h, bpp,
          fs ? "fullscreen" : "windowed");
    int32_t dw, dh;
    if (borderless_wanted() && desktop_size(&dw, &dh)) {
        /* Under borderless EVERY apply is treated as a fullscreen pick,
         * whatever the Full screen checkbox says: the conversion cleared
         * si+0x12, so the options screen believes it is windowed and
         * hands us fs=0 even when the player never touched the checkbox
         * — honoring that literally sizes a real window to the picked
         * resolution (1920x1200 on a 1800x1169 desktop = clamped,
         * sheared mess; observed). The borderless letterbox IS this
         * setup's fullscreen; a real small window needs Borderless=0. */
        int32_t tw, th;
        borderless_fit(w, h, dw, dh, &tw, &th);
        borderless_record(w, h, tw, th, dw, dh, "in-game apply");
        w = tw;
        h = th;
        fs = 0;
    } else if (!fs) {
        /* borderless off: a windowed request is honored as-is — window
         * at its own size, no bars, the plain size is what gets saved */
        g_letterbox = 0;
        g_borderless_active = 0;
        g_req_w = w;
        g_req_h = h;
        g_req_fs = 0;
    }
    int dirty = 0;
    int32_t cur;
    const struct { int32_t val; int curoff, pendoff; } f[3] = {
        { w, 0x19, 0x21 }, { h, 0x1d, 0x25 }, { bpp, 0x29, 0x2d },
    };
    for (int i = 0; i < 3; i++) {
        memcpy(&cur, si + f[i].curoff, 4);
        int32_t pend = f[i].val == cur ? cur : f[i].val;
        memcpy(si + f[i].pendoff, &pend, 4);
        if (f[i].val != cur)
            dirty = 1;
    }
    if ((uint8_t)fs != si[0x12]) {
        si[0x13] = (uint8_t)fs;
        dirty = 1;
    }
    if (dirty)
        si[0x38ed] = 1;
}

/* Runs at the single MoveWindow call that sizes the game window (inside
 * RenderD3D's window attach/restyle, on every (re)init: the styles were
 * just set, no DirectDraw surface exists yet, and nothing resizes the
 * window afterwards). While borderless, rewrite the pushed x/y/w/h so
 * the CLIENT area covers the desktop exactly: the stock engine sizes the
 * window to the mode, which under a letterbox is smaller than the
 * desktop — the clipper would then refuse the bar fills, the desktop
 * would show through, and the WS_EX_CLIENTEDGE ring would poke into
 * view. At client 0,0/desktop-size the ring hangs entirely off-screen. */
void __cdecl movewin_entry(uint32_t *regs)
{
    uint8_t *orig_esp = (uint8_t *)regs + 36;
    HWND w;
    memcpy(&w, orig_esp, 4);             /* MoveWindow arg 0: hwnd */
    int32_t dw, dh;
    if (!g_borderless_active || !w || !desktop_size(&dw, &dh))
        return;
    g_gamewnd = w;
    RECT r = { 0, 0, dw, dh };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(w, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongA(w, GWL_EXSTYLE));
    int32_t v;
    int32_t ow, oh;
    memcpy(&ow, orig_esp + 0xc, 4);
    memcpy(&oh, orig_esp + 0x10, 4);
    v = r.left;            memcpy(orig_esp + 0x4, &v, 4);
    v = r.top;             memcpy(orig_esp + 0x8, &v, 4);
    v = r.right - r.left;  memcpy(orig_esp + 0xc, &v, 4);
    v = r.bottom - r.top;  memcpy(orig_esp + 0x10, &v, 4);
    logf_("window size: %dx%d outer -> client %dx%d at 0,0 (desktop)",
          ow, oh, dw, dh);
}

/* Runs at the entry of the render-size setter (vtbl+0xa8): the attach
 * path calls it with the CLIENT size after the resize, and the WM_SIZE
 * handler repeats that on every resize. Those fields ([rend+0x83]/
 * [rend+0x87]) drive the relative-mouse normalization and recenter warp;
 * while letterboxing the client is desktop-sized but the game renders at
 * the fit mode, so clamp the stored size to the fit mode to keep the
 * mouse math consistent with the backbuffer. */
void __cdecl sizeset_entry(uint32_t *regs)
{
    if (!g_borderless_active || !g_letterbox)
        return;
    if (g_fit_w < 320 || g_fit_h < 200)
        return;
    uint8_t *orig_esp = (uint8_t *)regs + 36;        /* [0]=ret, args +4/+8 */
    memcpy(orig_esp + 4, &g_fit_w, 4);
    memcpy(orig_esp + 8, &g_fit_h, 4);
}

/* Runs at the tail of the client-rect capture in the same setup: the
 * RECT at [obj+0xbdd] (present blit destination, screen coords) and the
 * client size at [obj+0xc05]/[obj+0xc09] (windowed backbuffer + viewport
 * size) hold the full desktop client now. While letterboxing, retarget
 * them: rect = the centered fit rectangle, sizes = the fit mode — the
 * backbuffer then matches the mode the engine renders for (si+0x21/+0x25)
 * and the per-frame blit lands 1:1 in the centered rect. */
void __cdecl capture_entry(uint32_t *regs)
{
    uint8_t *obj = (uint8_t *)(uintptr_t)regs[1];    /* esi */
    if (!g_borderless_active || !g_letterbox)
        return;                          /* full-client capture is correct */
    if (g_fit_w < 320 || g_fit_h < 200)
        return;
    RECT rc = { g_fit_x, g_fit_y, g_fit_x + g_fit_w, g_fit_y + g_fit_h };
    memcpy(obj + OBJ_RECT, &rc, sizeof(rc));
    memcpy(obj + OBJ_W, &g_fit_w, 4);
    memcpy(obj + OBJ_H, &g_fit_h, 4);
    g_bar_fills = 30;                    /* burst: fresh surfaces hold
                                            garbage until painted over */
    logf_("present rect -> %d,%d %dx%d, backbuffer/viewport %dx%d",
          g_fit_x, g_fit_y, g_fit_w, g_fit_h, g_fit_w, g_fit_h);
}

/* Runs at the per-frame present entry. While letterboxing (windowed,
 * non-exclusive), black-fill the bar areas on the primary surface before
 * the game's own blit lands in the centered rect: the regions are
 * disjoint and the clipper the game attached confines everything to the
 * game window. Only fills while g_bar_fills is armed (init burst +
 * periodic watchdog refresh): steady-state per-frame fills are far too
 * expensive on this present path, and the window class's black
 * background brush already covers ordinary invalidation. */
void __cdecl present_entry(uint32_t *regs)
{
    uint8_t *obj = (uint8_t *)(uintptr_t)regs[6];    /* ecx (thiscall) */
    if (!g_letterbox || !g_borderless_active)
        return;
    if (g_bar_fills <= 0)
        return;
    if (obj[OBJ_EXCL])
        return;                          /* exclusive fullscreen: no bars */
    uint8_t *prim;
    memcpy(&prim, obj + OBJ_PRIMARY, 4);
    if (!prim)
        return;
    int32_t dw, dh;
    if (!desktop_size(&dw, &dh))
        return;
    typedef HRESULT(__stdcall *blt_t)(void *, RECT *, void *, RECT *,
                                      DWORD, void *);
    blt_t blt = (blt_t)(*(void ***)prim)[5];         /* IDDSurface7::Blt */
    uint32_t fx[0x64 / 4];                           /* DDBLTFX */
    memset(fx, 0, sizeof(fx));
    fx[0] = 0x64;                                    /* dwSize */
    fx[0x50 / 4] = 0;                                /* dwFillColor: black */
    /* the frame around the image rect — up to four strips (the image is
     * centered in the work area, so it can be inset on every side) */
    int32_t x0 = g_fit_x, y0 = g_fit_y;
    int32_t x1 = g_fit_x + g_fit_w, y1 = g_fit_y + g_fit_h;
    RECT bars[4];
    int n = 0;
    if (y0 > 0)  { RECT r = { 0, 0, dw, y0 };   bars[n++] = r; }
    if (y1 < dh) { RECT r = { 0, y1, dw, dh };  bars[n++] = r; }
    if (x0 > 0)  { RECT r = { 0, y0, x0, y1 };  bars[n++] = r; }
    if (x1 < dw) { RECT r = { x1, y0, dw, y1 }; bars[n++] = r; }
    for (int i = 0; i < n; i++)
        blt(prim, &bars[i], NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, fx);
    InterlockedDecrement(&g_bar_fills);
}

/* Wraps the hitman.ini writer (thiscall, no stack args): the borderless
 * conversion left the CONVERTED mode in si+0x19/+0x1d and cleared the
 * fullscreen byte si+0x12 (and by save time HC47HudScale has usually
 * replaced +0x19/+0x1d with its virtual GUI size on top) — none of which
 * is what the player picked. Restore the picked resolution and
 * fullscreen preference (tracked by the conversion and the SetResolution
 * replacement) for the duration of the write, force the config object's
 * fullscreen override to match, then put the live values back. The
 * writer runs synchronously on the game's main thread, so the swap is
 * invisible to everything else. */
static int(__fastcall *g_iniwrite_orig)(void *, void *);

static int __fastcall iniwrite_wrap(void *self, void *dummy)
{
    uint8_t *si = sysiface();
    if (!si || !g_iniwrite_orig || g_req_w < 320 || g_req_h < 200)
        return g_iniwrite_orig ? g_iniwrite_orig(self, dummy) : 0;
    int32_t sw, sh, sovr;
    uint8_t sfs = si[0x12];
    memcpy(&sw, si + 0x19, 4);
    memcpy(&sh, si + 0x1d, 4);
    memcpy(&sovr, (uint8_t *)self + CFG_FSOVERRIDE, 4);
    int32_t ovr = g_req_fs ? 2 : 1;
    memcpy(si + 0x19, &g_req_w, 4);
    memcpy(si + 0x1d, &g_req_h, 4);
    si[0x12] = (uint8_t)(g_req_fs ? 1 : 0);
    memcpy((uint8_t *)self + CFG_FSOVERRIDE, &ovr, 4);
    int ret = g_iniwrite_orig(self, dummy);
    memcpy(si + 0x19, &sw, 4);
    memcpy(si + 0x1d, &sh, 4);
    si[0x12] = sfs;
    memcpy((uint8_t *)self + CFG_FSOVERRIDE, &sovr, 4);
    logf_("hitman.ini saved with Resolution %dx%d, %s", g_req_w, g_req_h,
          g_req_fs ? "fullscreen" : "windowed");
    return ret;
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

/* Verify site bytes, then install an entry hook that calls fn, replays
 * the stolen instructions and continues after them. The stolen bytes
 * must be position-independent. */
static int hook_with_replay(const char *what, uint8_t *site,
                            const uint8_t *expect, size_t expect_len,
                            size_t steal, void *fn)
{
    if (memcmp(site, expect, expect_len) != 0) {
        logf_("%s: unexpected bytes — skipped", what);
        return 0;
    }
    if (!ensure_stubs())
        return 0;
    uint8_t *stub = g_stub_p;
    uint8_t *p = emit_hook_call(stub, fn);
    memcpy(p, site, steal);
    p += steal;
    p = emit_jmp(p, site + steal);
    g_stub_p = p;
    if (!hook_site(site, steal, stub))
        return 0;
    logf_("%s hook installed", what);
    return 1;
}

static int module_matches(HMODULE m, uint32_t stamp, uint32_t size)
{
    uint8_t *base = (uint8_t *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == stamp &&
           nt->OptionalHeader.SizeOfImage == size;
}

/* Maximum render-target axis for the legacy Direct3D7 HAL that modern
 * Windows emulates: CreateDevice fails with D3DERR_INVALID_DEVICE above
 * 2048px per axis, windowed and fullscreen alike (verified empirically:
 * 2048x2048 works; 2560x1440, 3840x1080 and 1920x2160 all fail).
 * Wine/CrossOver implements D3D7 natively and has no such limit. */
#define D3D7_MAX_RT_AXIS 2048

/* Resolutions modern games commonly offer (16:9, 16:10, 21:9), for the
 * options-menu list when ModernResolutionList is on. Ascending; labels
 * are sprintf'd "%d x %d" from the entries by the game. */
static const int32_t MODERN_MODES[][2] = {
    { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1440, 900 },
    { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
    { 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 },
    { 3840, 2160 },
};
#define MODERN_COUNT (int)(sizeof(MODERN_MODES) / sizeof(MODERN_MODES[0]))
#define MENU_MAX 40

static int display_mode_exists(int32_t w, int32_t h)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++)
        if (dm.dmBitsPerPel >= 32 &&
            (int32_t)dm.dmPelsWidth == w && (int32_t)dm.dmPelsHeight == h)
            return 1;
    return 0;
}

/* Rebuild the renderer's static display-mode table (the source of the
 * in-game options resolution list — entries {w,h,bpp,0}, 16 bytes each,
 * null-terminated): build a replacement in a VirtualAlloc'd block and
 * repoint the accessor's imm32 at it. With ModernResolutionList on the
 * stock 4:3 ladder is replaced by MODERN_MODES; either way the desktop
 * resolution (borderless: the size a fullscreen request fills) and the
 * startup hitman.ini resolution are kept in the list, so the options
 * screen can show and re-apply the mode actually running.
 *
 * Filtering mirrors the resolution guard: when exclusive fullscreen is
 * possible (not borderless) an entry must exist verbatim in the display
 * mode list or the mode-set dies with the "16bit colors" error, and
 * RenderD3D on real Windows refuses render targets above 2048px per
 * axis. Under borderless everything is a windowed request, and with
 * PreserveAspectRatio any aspect is meaningful (it letterboxes to the
 * largest fit), so nothing is filtered there. */
static void inject_menu_mode(const struct renderer_site *r, uint8_t *base)
{
    int borderless = borderless_wanted();
    if (!g_modern_list && !borderless)
        return;                           /* stock table, nothing to add */
    uint8_t *getter = base + r->mode_getter;
    int32_t *tab = (int32_t *)(base + r->mode_table);
    uint32_t expect = (uint32_t)(uintptr_t)tab;
    if (getter[0] != 0xb8 || getter[5] != 0xc3 ||
        memcmp(getter + 1, &expect, 4) != 0) {
        logf_("%s: unexpected mode-table accessor — resolution list kept",
              r->module);
        return;
    }
    int32_t limit = (r == &RENDERERS[0] && !is_wine()) ? D3D7_MAX_RT_AXIS
                                                       : 0x7fffffff;
    int32_t out[MENU_MAX][2];
    int n = 0;
    if (g_modern_list) {
        for (int i = 0; i < MODERN_COUNT && n < MENU_MAX; i++) {
            int32_t w = MODERN_MODES[i][0], h = MODERN_MODES[i][1];
            if (w > limit || h > limit)
                continue;
            if (!borderless && !display_mode_exists(w, h))
                continue;
            out[n][0] = w;
            out[n][1] = h;
            n++;
        }
    } else {
        for (int i = 0; i < MENU_MAX && tab[i * 4]; i++) {
            out[n][0] = tab[i * 4];
            out[n][1] = tab[i * 4 + 1];
            n++;
        }
    }
    /* the mode actually running always belongs in the list: the desktop
     * size (what a borderless fullscreen request fills without aspect
     * preservation) and the startup request (already validated by the
     * resolution guard) */
    int32_t rq_w = g_req_w, rq_h = g_req_h;
    if (rq_w < 320 || rq_h < 200) {
        uint8_t *si = sysiface();
        if (si) {
            memcpy(&rq_w, si + 0x19, 4);
            memcpy(&rq_h, si + 0x1d, 4);
        }
    }
    int32_t dw = 0, dh = 0;
    int32_t extra[2][2] = { { 0, 0 }, { rq_w, rq_h } };
    if (borderless && desktop_size(&dw, &dh)) {
        extra[0][0] = dw;
        extra[0][1] = dh;
    }
    for (int e = 0; e < 2; e++) {
        if (extra[e][0] < 320 || extra[e][1] < 200 || n >= MENU_MAX)
            continue;
        int have = 0;
        for (int i = 0; i < n; i++)
            if (out[i][0] == extra[e][0] && out[i][1] == extra[e][1])
                have = 1;
        if (!have) {
            out[n][0] = extra[e][0];
            out[n][1] = extra[e][1];
            n++;
        }
    }
    if (n == 0) {
        logf_("%s: no usable menu resolution — stock list kept", r->module);
        return;
    }
    /* ascending like the stock list (order is cosmetic: the selection
     * hook matches entries exactly and applies copy the entry verbatim) */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && (out[j - 1][0] > out[j][0] ||
                                  (out[j - 1][0] == out[j][0] &&
                                   out[j - 1][1] > out[j][1])); j--) {
            int32_t tw = out[j][0], th = out[j][1];
            out[j][0] = out[j - 1][0]; out[j][1] = out[j - 1][1];
            out[j - 1][0] = tw; out[j - 1][1] = th;
        }
    int32_t *nt = (int32_t *)VirtualAlloc(NULL, (size_t)(n + 1) * 16,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!nt) return;
    for (int i = 0; i < n; i++) {
        nt[i * 4] = out[i][0];
        nt[i * 4 + 1] = out[i][1];
        nt[i * 4 + 2] = 32;               /* terminator stays zeroed */
    }
    uint32_t nv = (uint32_t)(uintptr_t)nt;
    if (write_code(getter + 1, (uint8_t *)&nv, 4)) {
        char list[MENU_MAX * 12], *p = list;
        for (int i = 0; i < n; i++)
            p += snprintf(p, sizeof(list) - (size_t)(p - list), "%s%dx%d",
                          i ? " " : "", out[i][0], out[i][1]);
        logf_("%s: options resolution list %s(%d): %s", r->module,
              g_modern_list ? "replaced " : "extended ", n, list);
    }
}

/* Install the letterbox machinery on RenderD3D (window sizing at the
 * MoveWindow call, mouse-size clamp at the render-size setter, rect/size
 * retarget at the capture tail, bar fills at the present entry). All
 * four must land or the letterbox is disabled (fall back to filling the
 * desktop) — a partial install would show a sheared or unclipped image. */
static void patch_letterbox(const struct renderer_site *r, uint8_t *base)
{
    if (!borderless_wanted() || !g_preserve_aspect)
        return;
    if (!r->movewin) {
        logf_("%s: no letterbox support (window-relative present) — "
              "fullscreen fills the desktop", r->module);
        return;
    }
    /* the MoveWindow call site: ff 15 <abs thunk va>, operand computed
     * from the live base (the module can relocate) */
    uint8_t *mw = base + r->movewin;
    uint32_t thunk = (uint32_t)(uintptr_t)(base + r->movewin_thunk);
    uint8_t mw_expect[6] = { 0xff, 0x15 };
    memcpy(mw_expect + 2, &thunk, 4);
    uint8_t *cap = base + r->capture;
    if (memcmp(cap - sizeof(CAPTURE_PRE), CAPTURE_PRE,
               sizeof(CAPTURE_PRE)) != 0) {
        logf_("%s: unexpected bytes before capture tail — letterbox off",
              r->module);
        return;
    }
    /* the stolen bytes are position-independent for all four sites (the
     * MoveWindow call uses an absolute operand, replayed verbatim) */
    if (!hook_with_replay("window sizing (MoveWindow)", mw,
                          mw_expect, sizeof(mw_expect),
                          sizeof(mw_expect), (void *)movewin_entry))
        return;
    if (!hook_with_replay("render-size clamp", base + r->sizeset,
                          SIZESET_BYTES, sizeof(SIZESET_BYTES),
                          SIZESET_HOOK_LEN, (void *)sizeset_entry))
        return;
    if (!hook_with_replay("present-rect capture", cap,
                          CAPTURE_BYTES, sizeof(CAPTURE_BYTES),
                          sizeof(CAPTURE_BYTES), (void *)capture_entry))
        return;
    if (!hook_with_replay("present (letterbox bars)", base + r->present,
                          PRESENT_BYTES, sizeof(PRESENT_BYTES),
                          PRESENT_HOOK_LEN, (void *)present_entry))
        return;
    g_letterbox_ok = 1;
    logf_("%s: in-window letterbox armed", r->module);
}

/* Replace the renderer interface's SetResolution with the converting
 * reimplementation (setres_entry): the 5-byte `mov eax,[glob]` at its
 * entry becomes a jmp to a stub that calls the C function and returns
 * with `ret 4` — the original body is fully reproduced there, so nothing
 * jumps back. Without this hook an in-game options apply stages a raw
 * exclusive-fullscreen request that no other patch point can intercept. */
static void patch_setres(const struct renderer_site *r, uint8_t *base)
{
    if (!borderless_wanted())
        return;                    /* stock staging is fine as-is */
    uint8_t *site = base + r->setres;
    uint32_t glob = (uint32_t)(uintptr_t)(base + r->setres_glob);
    if (site[0] != 0xa1 || memcmp(site + 1, &glob, 4) != 0 ||
        memcmp(site + 5, SETRES_TAIL, sizeof(SETRES_TAIL)) != 0) {
        logf_("%s: unexpected bytes at SetResolution — in-game applies "
              "keep the stock path", r->module);
        return;
    }
    if (!ensure_stubs() || !sysiface())
        return;
    uint8_t *stub = g_stub_p;
    uint8_t *p = emit_hook_call(stub, (void *)setres_entry);
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;      /* ret 4 */
    g_stub_p = p;
    if (hook_site(site, 5, stub))
        logf_("%s: in-game mode requests routed through the borderless "
              "conversion", r->module);
}

/* Wrap the hitman.ini writer (see iniwrite_wrap): verify EngineData and
 * the function head, build a trampoline from the stolen `sub esp,0x1d8`,
 * and point the entry at the wrapper (thiscall == fastcall with an
 * unused edx). Returns 1 done or given up, 0 = module not loaded yet. */
static int patch_iniwriter(void)
{
    HMODULE m = GetModuleHandleA("EngineData.dll");
    if (!m) return 0;
    uint8_t *base = (uint8_t *)m;
    if (!module_matches(m, ED_TIMESTAMP, ED_SIZEOFIMAGE)) {
        logf_("EngineData.dll build mismatch — ini save fix skipped");
        return 1;
    }
    uint8_t *site = base + INIWRITE_RVA;
    uint32_t slot = (uint32_t)(uintptr_t)(base + ED_SYSIF_SLOT);
    if (memcmp(site, INIWRITE_HEAD, sizeof(INIWRITE_HEAD)) != 0 ||
        site[6] != 0xa1 || memcmp(site + 7, &slot, 4) != 0 ||
        memcmp(site + 11, INIWRITE_TAIL, sizeof(INIWRITE_TAIL)) != 0) {
        logf_("hitman.ini writer: unexpected bytes — ini save fix skipped");
        return 1;
    }
    if (!ensure_stubs()) return 1;
    uint8_t *tramp = g_stub_p;
    memcpy(tramp, site, sizeof(INIWRITE_HEAD));
    uint8_t *p = tramp + sizeof(INIWRITE_HEAD);
    p = emit_jmp(p, site + sizeof(INIWRITE_HEAD));
    g_stub_p = p;
    g_iniwrite_orig = (int(__fastcall *)(void *, void *))(uintptr_t)tramp;
    uint8_t buf[6];
    buf[0] = 0xE9;
    *(int32_t *)(buf + 1) =
        (int32_t)((uint8_t *)(uintptr_t)iniwrite_wrap - (site + 5));
    buf[5] = 0x90;
    if (write_code(site, buf, sizeof(buf)))
        logf_("hitman.ini save fix installed — the picked resolution and "
              "fullscreen preference persist");
    return 1;
}

/* Disable the resolution-snap ladder in whichever renderer is loaded and
 * put the (re)init hook in its place, so an in-game fullscreen request
 * is converted to borderless before mode selection runs. Also installs
 * the mode-table replacement, the SetResolution replacement and the
 * letterbox hooks. Returns 1 patched, -1 mismatch (give up), 0 not
 * loaded yet. On success *which is set to the RENDERERS index. */
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
        patch_letterbox(&RENDERERS[i], (uint8_t *)m);
        patch_setres(&RENDERERS[i], (uint8_t *)m);
        /* the letterbox hooks did not all land (or OpenGL): fall back to
         * filling the desktop so the image is never sheared */
        if (g_preserve_aspect && borderless_wanted() && !g_letterbox_ok) {
            g_preserve_aspect = 0;
            uint8_t *si = sysiface();
            int32_t dw, dh;
            if (g_letterbox && si && desktop_size(&dw, &dh)) {
                memcpy(si + 0x19, &dw, 4);
                memcpy(si + 0x1d, &dh, 4);
                g_letterbox = 0;
                g_fit_x = g_fit_y = 0;
                g_fit_w = dw;
                g_fit_h = dh;
                logf_("letterbox unavailable — mode raised to the "
                      "desktop size %dx%d", dw, dh);
            }
        }
        *which = i;
        return 1;
    }
    return 0;
}

/* ---- resolution guard ------------------------------------------------ */

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

    /* remember what hitman.ini actually asked for (this runs before any
     * conversion and before HC47HudScale replaces +0x19/+0x1d with its
     * virtual GUI size): the ini-writer wrap restores these at save time
     * so the file round-trips the player's preference */
    if (g_req_w < 320 || g_req_h < 200) {
        g_req_w = w;
        g_req_h = h;
        g_req_fs = fullscreen != 0;
    }

    /* fullscreen -> borderless window (see the header comment; default
     * on Wine, where exclusive DirectDraw fullscreen is broken under the
     * CrossOver Mac driver) */
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

/* Find the game window (ZSystemClass only: during an in-game mode change
 * the window is destroyed and recreated, and a looser match could adopt
 * a foreign window in that gap) and keep its client area anchored: at
 * 0,0 always, and desktop-sized while letterboxing (backstop for the
 * MoveWindow hook — e.g. if the window manager nudged the window later).
 * Runs once the renderer has established the display mode; retried by
 * the watchdog until a window is found, re-run when the mode changes. */
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
    if (strncmp(cls, "ZSystemClass", 12) != 0)
        return TRUE;
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
    g_gamewnd = pick.best;

    /* The game's windowed path already creates an undecorated popup
     * (that is why hitman.ini has StartUpperPos: the window cannot be
     * dragged). Touching the style of a live DirectDraw device window
     * breaks rendering under the CrossOver Mac driver (permanent black
     * window), so intervene as little as possible: never restyle, and
     * only move the window when the client area does not sit at 0,0. */
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
     * so the menu reflects (and re-applies) the mode the player picked */
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
    /* the ini save fix matters beyond the borderless conversion: the
     * writer reads si+0x19/+0x1d, which HC47HudScale replaces with its
     * virtual GUI size — install it unconditionally */
    int ini_state = 0;
    int snap_tries = 0;
    int renderer = -1, guarded = 0;
    (void)renderer;
    int borderless_done = 0;
    int bar_tick = 0;
    int32_t lastw = 0, lasth = 0;
    for (;;) {
        int settled = snap_state != 0 && dlc_state != 0;
        Sleep(settled ? 250 : guarded ? 25 : 5);
        uint8_t *si = sysiface();
        /* guard first: it must rewrite the requested resolution before
         * the renderer creates its window, and its mode-list scan must
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
        if (ini_state == 0)
            ini_state = patch_iniwriter();
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
        if (g_borderless_active) {
            if (borderless_done && g_gamewnd && !IsWindow(g_gamewnd))
                borderless_done = 0;      /* window recreated — re-pick */
            if (!borderless_done)
                borderless_done = apply_borderless(w, h);
            /* periodic one-shot bar refresh (~2s): heals anything that
             * dirtied the primary without invalidating the window, at
             * no steady-state present cost */
            if (g_letterbox && settled && ++bar_tick >= 8) {
                bar_tick = 0;
                if (g_bar_fills < 1)
                    g_bar_fills = 1;
            }
        }
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
        logf_("HC47 Widescreen loaded%s, FOVFactor=%.2f DrawDistanceFactor=%.2f "
              "PreserveAspectRatio=%d ModernResolutionList=%d",
              g_enabled ? "" : " (disabled)",
              (double)g_fovfactor, (double)g_ddfactor, g_preserve_aspect,
              g_modern_list);
        if (g_enabled)
            CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    }
    return TRUE;
}
