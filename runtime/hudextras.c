/* HC47 HUD Extras — crosshair shrink + mission timer + FPS overlay (32-bit ASI).
 *
 * Three independent features, all driven from a single per-frame hook on the
 * engine's game-tick call site (System.dll), so every engine call we make
 * runs on the game thread between tick and render:
 *
 * 1. CrosshairScale: the aiming crosshair / center dot is a set of 2D GUI
 *    sprites (engine class ZWINOBJ, one per weapon class + "+Contex"
 *    variants) living in the level pack under Bitmaps/Pointers. ZWINOBJ
 *    carries render-scale doubles at +0xd7/+0xdf and a virtual
 *    SetScale(double sc[2], uchar flag) at vtable +0x4a4 that rescales the
 *    already-built quad vertices by new/old ratio. We resolve each sprite
 *    by name through the engine's object database and call SetScale.
 *    Objects are recreated on level load with scale 1.0; we detect the new
 *    handle (or a reset +0xd7) and re-apply.
 *
 * 2. Mission timer: the engine keeps one global clock (double seconds,
 *    ZSysInterface +0x37c5, latched once per frame; never reset, ticks
 *    monotonically for the whole run). There is no native per-mission
 *    counter, but LevelControl::Init (HitmanDlc RVA 0x45fc0) runs on every
 *    mission (re)start and the teardown (RVA 0x46fc0) on unload, so we
 *    latch mission start time there. Savegames (JPMHS) persist only
 *    profile/level-unlock/equipment — no mid-mission state exists, so a
 *    timer that restarts with the mission is faithful to engine semantics.
 *
 * 3. FPS: derived from the engine's own frame timestamps
 *    (+0x37c5 current / +0x37cd previous), smoothed over a window.
 *
 * Timer and FPS are rendered below the top-left HUD with the game's own
 * HUD text pipeline (see hudtext section below).
 *
 * Config: scripts/HC47HudExtras.ini
 *   [HudExtras]
 *   CrosshairScale=0.5   ; 1.0 = untouched
 *   ShowTimer=1
 *   ShowFPS=1
 *   TextX=10             ; overlay position, virtual GUI pixels
 *   TextY=100            ;   (just below the top-left HUD)
 *   LineGap=16
 *
 * All patch/hook sites are byte-checked against the exact retail build
 * (HitmanDlc.dlc stamp 0x3a3e13d1, System.dll stamp 0x3a3e130a); any
 * mismatch disables that feature only, with a log line.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---- build identity ---- */
#define DLC_TIMESTAMP    0x3a3e13d1
#define DLC_SIZEOFIMAGE  0x274000
#define SYS_TIMESTAMP    0x3a3e130a
#define SYS_SIZEOFIMAGE  0x47000

/* ---- ZSysInterface offsets (packed struct) ---- */
#define SI_GAMETIME   0x37c5   /* double: current frame game time, seconds */
#define SI_PREVTIME   0x37cd   /* double: previous frame game time */

/* ---- object database (HitmanDlc data, VAs at preferred base 0xfcc0000) ---- */
#define DLC_PREF_BASE 0x0fcc0000u
#define VA_GAMEPTR    0x0feb000cu  /* [[VA]] -> engine root; +0x59 -> db owner */
#define VA_HANDLETAB  0x0feb0008u  /* handle table root for handle->ptr */
#define ZWINOBJ_VTBL_VA 0x0feb3ce0u
#define SETSCALE_SLOT 0x4a4        /* ZWINOBJ vtable: SetScale(double[2], uchar) */

/* ---- hook sites ---- */
#define SYS_TICKCALL_RVA 0xd238    /* call [esi+0x3bdb] — per-frame game tick */
static const uint8_t SYS_TICKCALL_BYTES[6] = { 0xFF, 0x96, 0xDB, 0x3B, 0x00, 0x00 };
#define DLC_LCINIT_RVA   0x45fc0   /* LevelControl::Init (mission start) */
static const uint8_t DLC_LCINIT_BYTES[5] = { 0x8B, 0x44, 0x24, 0x04, 0x53 };
#define DLC_LCDONE_RVA   0x46fc0   /* LevelControl teardown (mission end) */
/* first insn is `mov eax,[VA_GAMEPTR]` — absolute address, so the expected
 * bytes are built at runtime with the relocation delta applied */

static FILE *g_log;
static char g_dir[MAX_PATH];
static float g_xhair = 1.0f;
static int g_show_timer = 1, g_show_fps = 1;
static float g_text_x = 10.0f, g_text_y = 100.0f, g_line_gap = 16.0f;

static uint8_t *g_dlc;             /* HitmanDlc.dlc base (0 until loaded) */
static uint8_t *g_sys;             /* System.dll base */
static uint8_t *g_si;              /* ZSysInterface object */
static uint32_t g_dlc_delta;       /* actual base - preferred base */

static volatile int g_in_mission;
static double g_mission_start;

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

static void read_ini(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\HC47HudExtras.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v; int b;
        if (sscanf(line, " CrosshairScale = %f", &v) == 1 ||
            sscanf(line, " CrosshairScale=%f", &v) == 1) g_xhair = v;
        else if (sscanf(line, " ShowTimer = %d", &b) == 1 ||
                 sscanf(line, " ShowTimer=%d", &b) == 1) g_show_timer = b;
        else if (sscanf(line, " ShowFPS = %d", &b) == 1 ||
                 sscanf(line, " ShowFPS=%d", &b) == 1) g_show_fps = b;
        else if (sscanf(line, " TextX = %f", &v) == 1 ||
                 sscanf(line, " TextX=%f", &v) == 1) g_text_x = v;
        else if (sscanf(line, " TextY = %f", &v) == 1 ||
                 sscanf(line, " TextY=%f", &v) == 1) g_text_y = v;
        else if (sscanf(line, " LineGap = %f", &v) == 1 ||
                 sscanf(line, " LineGap=%f", &v) == 1) g_line_gap = v;
    }
    fclose(f);
    if (!(g_xhair >= 0.1f && g_xhair <= 4.0f)) g_xhair = 1.0f;
}

/* ---- engine object database access (game thread only) ---- */

static uint32_t dlc_va(uint32_t pref_va) { return pref_va + g_dlc_delta; }

typedef void *(__fastcall *vfn_this_t)(void *self, void *edx);

/* returns object-db, or NULL while the engine is not up yet */
static void *obj_db(void)
{
    uint8_t **root = *(uint8_t ***)(uintptr_t)dlc_va(VA_GAMEPTR);
    if (!root || IsBadReadPtr(root, 4)) return NULL;
    uint8_t *p = (uint8_t *)*root;
    if (!p || IsBadReadPtr(p + 0x59, 4)) return NULL;
    uint8_t *owner;
    memcpy(&owner, p + 0x59, 4);
    if (!owner || IsBadReadPtr(owner, 4)) return NULL;
    void **vt = *(void ***)owner;
    if (IsBadReadPtr(vt + 0x30 / 4, 4)) return NULL;
    vfn_this_t get = (vfn_this_t)vt[0x30 / 4];
    return get(owner, NULL);
}

/* generic __thiscall dispatcher: pushes argv[argc-1]..argv[0] (so argv[0]
 * is the first argument), ecx = self, calls fn (callee cleans args). */
uint32_t engine_thiscall(void *self, void *fn, int argc, const uint32_t *argv);
__asm__(
    ".text\n"
    ".globl _engine_thiscall\n"
    "_engine_thiscall:\n"
    "  push %ebp\n"
    "  mov  %esp, %ebp\n"
    "  push %esi\n"
    "  mov  16(%ebp), %eax\n"          /* argc */
    "  mov  20(%ebp), %esi\n"          /* argv */
    "  test %eax, %eax\n"
    "  jz   2f\n"
    "1: push -4(%esi,%eax,4)\n"
    "  dec  %eax\n"
    "  jnz  1b\n"
    "2: mov  8(%ebp), %ecx\n"          /* self */
    "  call *12(%ebp)\n"               /* fn (callee-cleans pushed args) */
    "  pop  %esi\n"
    "  pop  %ebp\n"
    "  ret\n");

static uint32_t vcall(void *self, uint32_t slot, int argc, const uint32_t *argv)
{
    void **vt = *(void ***)self;
    return engine_thiscall(self, vt[slot / 4], argc, argv);
}

/* thiscall db->[vt+0x9c](name, &out): name -> handle (0 on failure) */
static uint32_t db_lookup(void *db, const char *name)
{
    uint32_t handle = 0;
    uint32_t args[2] = { (uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)&handle };
    vcall(db, 0x9c, 2, args);
    return handle;
}

/* handle -> object pointer (replicates HitmanDlc RVA 0x26000) */
static uint8_t *handle_ptr(uint32_t h)
{
    if (!h) return NULL;
    uint8_t **rootpp = (uint8_t **)(uintptr_t)dlc_va(VA_HANDLETAB);
    if (IsBadReadPtr(rootpp, 4) || !*rootpp || IsBadReadPtr(*rootpp, 4))
        return NULL;
    uint8_t *A = **(uint8_t ***)rootpp;
    if (!A || IsBadReadPtr(A + 0xd, 8)) return NULL;
    uint32_t idx = h & 0x3ffff, gen = h >> 18;
    uint32_t *gens, *objs;
    memcpy(&gens, A + 9, 4);
    memcpy(&objs, A + 0xd, 4);
    if (!gens || !objs || IsBadReadPtr(gens + idx, 4) || IsBadReadPtr(objs + idx, 4))
        return NULL;
    if (gens[idx] != gen) return NULL;
    return (uint8_t *)objs[idx];
}

/* ---- feature 1: crosshair scale ---- */

static const char *k_pointer_names[] = {
    "CrossHair_02-Normal",
    "CrossHair_02-Normal+Context",
    "CrossHair_02-AutomaticRifles",
    "CrossHair_02-AutomaticRifles+Contex",
    "CrossHair_02-ShotGuns",
    "CrossHair_02-ShotGuns+Contex",
    "CrossHair_02-SmallArms",
    "CrossHair_02-SmallArms+Contex",
};
#define N_POINTERS (sizeof(k_pointer_names)/sizeof(k_pointer_names[0]))
static uint32_t g_ptr_handles[N_POINTERS];

static void winobj_setscale(uint8_t *obj, double s)
{
    double sc[2] = { s, s };
    uint32_t args[2] = { (uint32_t)(uintptr_t)sc, 0 };
    vcall(obj, SETSCALE_SLOT, 2, args);
}

static void crosshair_tick(void)
{
    if (g_xhair == 1.0f) return;
    /* name lookups may scan the whole object db — a few times a second
     * is plenty (scale only resets on level load) */
    static int cooldown;
    if (cooldown-- > 0) return;
    cooldown = 20;
    void *db = obj_db();
    if (!db) return;
    for (unsigned i = 0; i < N_POINTERS; i++) {
        uint32_t h = db_lookup(db, k_pointer_names[i]);
        if (!h) { g_ptr_handles[i] = 0; continue; }
        uint8_t *obj = handle_ptr(h);
        if (!obj || IsBadReadPtr(obj, 4)) { g_ptr_handles[i] = 0; continue; }
        if (*(uint32_t *)obj != dlc_va(ZWINOBJ_VTBL_VA)) {
            if (h != g_ptr_handles[i]) {
                logf_("crosshair: '%s' is not a ZWINOBJ (vt=%08x) — skipped",
                      k_pointer_names[i], *(uint32_t *)obj);
                g_ptr_handles[i] = h;
            }
            continue;
        }
        double cur;
        memcpy(&cur, obj + 0xd7, 8);
        if (h != g_ptr_handles[i] || fabs(cur - (double)g_xhair) > 1e-6) {
            winobj_setscale(obj, (double)g_xhair);
            if (h != g_ptr_handles[i])
                logf_("crosshair: '%s' scaled to %.3f (handle %08x)",
                      k_pointer_names[i], (double)g_xhair, h);
            g_ptr_handles[i] = h;
        }
    }
}

/* ---- feature 2+3: timer + fps state ---- */

static double game_time(void)
{
    double t = 0;
    if (g_si) memcpy(&t, g_si + SI_GAMETIME, 8);
    return t;
}

/* called from LevelControl::Init hook (game thread) */
void __cdecl on_mission_start(void)
{
    g_mission_start = game_time();
    g_in_mission = 1;
    logf_("mission start at t=%.2f", g_mission_start);
}

void __cdecl on_mission_end(void)
{
    g_in_mission = 0;
    logf_("mission end");
}

/* FPS: exponential moving average of the engine frame delta */
static double g_fps;

static void fps_tick(void)
{
    if (!g_si) return;
    double t, p;
    memcpy(&t, g_si + SI_GAMETIME, 8);
    memcpy(&p, g_si + SI_PREVTIME, 8);
    double dt = t - p;
    if (dt > 1e-6 && dt < 1.0) {
        double fps = 1.0 / dt;
        g_fps = g_fps == 0 ? fps : g_fps * 0.95 + fps * 0.05;
    }
}

/* ---- hudtext: render two lines with the in-game font ----
 *
 * Mirrors the engine's own OSD label creator (HitmanDlc RVA 0x53930, the
 * path that makes the top-left Health/Armor header texts): create a
 * ZCHAROBJ child of the in-game HUD window, give it the HUD font, set
 * text/pos/color, show. The HUD window and font are named objects in the
 * level pack: "IngameDisplay" (ZWINDOW, the whole in-game 2D display) and
 * "BankGothic10" (the BNKGOTHL.TTF HUD font). Both die with the level;
 * we detect stale handles and recreate.
 *
 * ZCHAROBJ vtable slots (thiscall, callee-cleans):
 *   +0x444 CreateChild(name, class, 0) -> child ptr   (on window/group!)
 *   +0x294 color/material init
 *   +0x25c register/init(1,0,0,0)
 *   +0x4d4 SetFont(font, 0, 0)
 *   +0x4e0 SetText(str, 0)      — heap-copies, rebuilds glyph quads
 *   +0x14c SetPos(float pos[3]) — virtual GUI pixels, z=1.0
 *   +0x488 SetColor(0x00RRGGBB)
 *   +0x47c alpha/effect(0x12, 0xff) — as the OSD template does
 *   +0x214 Show(1)
 */
#define VA_OSDCOLOR   0x0fee65c0u  /* dword: 'OSDStandardGreen' HUD color */

static uint8_t *g_lbl[2];          /* [0] timer, [1] fps */
static uint32_t g_lbl_h[2];
static uint8_t *g_parent;
static uint32_t g_parent_h;
static char g_lbl_txt[2][32];

static uint32_t obj_handle(uint8_t *obj) { return ((uint32_t *)obj)[-1]; }

static uint8_t *label_create(uint8_t *parent, uint8_t *font,
                             const char *name, float x, float y)
{
    uint32_t a3[3] = { (uint32_t)(uintptr_t)name,
                       (uint32_t)(uintptr_t)"ZCHAROBJ", 0 };
    uint8_t *lbl = (uint8_t *)(uintptr_t)vcall(parent, 0x444, 3, a3);
    if (!lbl) return NULL;
    vcall(lbl, 0x294, 0, NULL);
    *(uint32_t *)(lbl + 0x3c) |= 0x80000;          /* runtime-created */
    { uint32_t a[4] = { 1, 0, 0, 0 }; vcall(lbl, 0x25c, 4, a); }
    { uint32_t a[3] = { (uint32_t)(uintptr_t)font, 0, 0 }; vcall(lbl, 0x4d4, 3, a); }
    { uint32_t a[2] = { (uint32_t)(uintptr_t)"", 0 }; vcall(lbl, 0x4e0, 2, a); }
    {
        float pos[3] = { x, y, 1.0f };
        uint32_t a[1] = { (uint32_t)(uintptr_t)pos };
        vcall(lbl, 0x14c, 1, a);
    }
    {
        uint32_t col = 0x00c0c0c0;
        uint32_t *osd = (uint32_t *)(uintptr_t)dlc_va(VA_OSDCOLOR);
        if (!IsBadReadPtr(osd, 4) && *osd) col = *osd;
        uint32_t a[1] = { col };
        vcall(lbl, 0x488, 1, a);
    }
    { uint32_t a[2] = { 0x12, 0xff }; vcall(lbl, 0x47c, 2, a); }
    { uint32_t a[1] = { 1 }; vcall(lbl, 0x214, 1, a); }
    return lbl;
}

static void label_settext(int i, const char *txt)
{
    if (!g_lbl[i]) return;
    if (strcmp(g_lbl_txt[i], txt) == 0) return;
    strncpy(g_lbl_txt[i], txt, sizeof(g_lbl_txt[i]) - 1);
    uint32_t a[2] = { (uint32_t)(uintptr_t)txt, 0 };
    vcall(g_lbl[i], 0x4e0, 2, a);
}

static void hudtext_drop(void)
{
    g_lbl[0] = g_lbl[1] = NULL;
    g_lbl_h[0] = g_lbl_h[1] = 0;
    g_parent = NULL;
    g_parent_h = 0;
    g_lbl_txt[0][0] = g_lbl_txt[1][0] = 0;
}

static void hudtext_tick(void)
{
    if (!g_show_timer && !g_show_fps) return;

    /* stale detection: level teardown recycles the handles */
    if (g_parent) {
        if (handle_ptr(g_parent_h) != g_parent ||
            (g_lbl[0] && handle_ptr(g_lbl_h[0]) != g_lbl[0]) ||
            (g_lbl[1] && handle_ptr(g_lbl_h[1]) != g_lbl[1])) {
            logf_("hudtext: level GUI gone, labels dropped");
            hudtext_drop();
        }
    }

    if (!g_parent) {
        /* (re)acquire — a few times a second is enough */
        static int cooldown;
        if (cooldown-- > 0) return;
        cooldown = 20;
        void *db = obj_db();
        if (!db) return;
        uint32_t hw = db_lookup(db, "IngameDisplay");
        uint8_t *wnd = handle_ptr(hw);
        if (!wnd) return;                       /* not in a mission */
        uint32_t hf = db_lookup(db, "BankGothic10");
        uint8_t *font = handle_ptr(hf);
        if (!font) {
            logf_("hudtext: 'BankGothic10' not found — overlay disabled this level");
            return;
        }
        float y = g_text_y;
        if (g_show_timer) {
            g_lbl[0] = label_create(wnd, font, "HXTIMER", g_text_x, y);
            if (g_lbl[0]) { g_lbl_h[0] = obj_handle(g_lbl[0]); y += g_line_gap; }
        }
        if (g_show_fps) {
            g_lbl[1] = label_create(wnd, font, "HXFPS", g_text_x, y);
            if (g_lbl[1]) g_lbl_h[1] = obj_handle(g_lbl[1]);
        }
        if (!g_lbl[0] && !g_lbl[1]) return;
        g_parent = wnd;
        g_parent_h = hw;
        logf_("hudtext: labels created (wnd=%p font=%p fontvt=%08x)",
              wnd, font, *(uint32_t *)font);
    }

    if (g_lbl[0]) {
        char buf[32];
        if (g_in_mission) {
            double e = game_time() - g_mission_start;
            if (e < 0) e = 0;
            int s = (int)e;
            if (s >= 3600)
                snprintf(buf, sizeof(buf), "%d:%02d:%02d",
                         s / 3600, (s / 60) % 60, s % 60);
            else
                snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
        } else {
            snprintf(buf, sizeof(buf), "--:--");
        }
        label_settext(0, buf);
    }
    if (g_lbl[1]) {
        static int cooldown;
        if (cooldown-- <= 0) {
            cooldown = 15;                      /* ~4 Hz refresh */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d FPS", (int)(g_fps + 0.5));
            label_settext(1, buf);
        }
    }
}

/* ---- patching ----
 * All three patch sites are 8-byte aligned (System+0xd238, DLC+0x45fc0,
 * DLC+0x46fc0) and shorter than 8 bytes, so each patch is applied with one
 * atomic cmpxchg8b — the game thread may already be executing this code. */

static int patch_atomic(uint8_t *site, const uint8_t *bytes, int n)
{
    if (((uintptr_t)site & 7) || n > 8) return 0;
    DWORD old;
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) return 0;
    volatile long long *q = (volatile long long *)site;
    long long expected = *q;
    for (;;) {
        long long desired = expected;
        memcpy(&desired, bytes, n);
        long long prev = __sync_val_compare_and_swap(q, expected, desired);
        if (prev == expected) break;
        expected = prev;
    }
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    return 1;
}

/* ---- per-frame hook ---- */

void __cdecl frame_tick(void)
{
    fps_tick();
    crosshair_tick();
    hudtext_tick();
}

/* stub: preserve everything, call C, then jmp through the engine's own
 * tick pointer [esi+0x3bdb] (esi = gSys at the patched call site; callee
 * does ret 4 so stack balances exactly as the original call did) */
static uint8_t *g_tickstub;
static void *g_tickstub_ptr;       /* memory cell holding stub addr for FF 15 */

static int hook_frame(void)
{
    uint8_t *site = g_sys + SYS_TICKCALL_RVA;
    if (memcmp(site, SYS_TICKCALL_BYTES, 6) != 0) {
        logf_("frame hook: unexpected bytes at System+0x%x — disabled",
              SYS_TICKCALL_RVA);
        return 0;
    }
    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    uint8_t *p = stub;
    *p++ = 0x9C; *p++ = 0x60;                 /* pushfd; pushad */
    *p++ = 0xE8;                              /* call frame_tick */
    *(int32_t *)p = (int32_t)((uint8_t *)(uintptr_t)frame_tick - (p + 4)); p += 4;
    *p++ = 0x61; *p++ = 0x9D;                 /* popad; popfd */
    /* jmp dword ptr [esi+0x3bdb] */
    *p++ = 0xFF; *p++ = 0xA6;
    *(uint32_t *)p = 0x3bdb; p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    g_tickstub = stub;
    g_tickstub_ptr = stub;

    uint8_t patch[6];
    patch[0] = 0xFF; patch[1] = 0x15;         /* call dword ptr [abs32] */
    uint32_t a = (uint32_t)(uintptr_t)&g_tickstub_ptr;
    memcpy(patch + 2, &a, 4);
    if (!patch_atomic(site, patch, 6)) return 0;
    logf_("frame hook installed at System+0x%x", SYS_TICKCALL_RVA);
    return 1;
}

/* entry hooks for LevelControl init/teardown: 5-byte jmp, relocated prologue */
static int hook_entry(uint8_t *site, const uint8_t *expect, int n,
                      void (__cdecl *fn)(void), const char *what)
{
    if (memcmp(site, expect, n) != 0) {
        logf_("%s hook: unexpected bytes — disabled", what);
        return 0;
    }
    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    uint8_t *p = stub;
    *p++ = 0x9C; *p++ = 0x60;                 /* pushfd; pushad */
    *p++ = 0xE8;
    *(int32_t *)p = (int32_t)((uint8_t *)(uintptr_t)fn - (p + 4)); p += 4;
    *p++ = 0x61; *p++ = 0x9D;                 /* popad; popfd */
    memcpy(p, site, n); p += n;               /* relocated prologue (as in memory) */
    *p++ = 0xE9;                              /* jmp back */
    *(int32_t *)p = (int32_t)((site + n) - (p + 4)); p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, 64);

    uint8_t patch[5];
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)(stub - (site + 5));
    if (!patch_atomic(site, patch, 5)) return 0;
    logf_("%s hook installed", what);
    return 1;
}

/* ---- init ---- */

static int module_ok(uint8_t *base, uint32_t stamp, uint32_t size, const char *name)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != stamp ||
        nt->OptionalHeader.SizeOfImage != size) {
        logf_("%s build mismatch (stamp %08lx size %08lx) — feature(s) disabled",
              name, (unsigned long)nt->FileHeader.TimeDateStamp,
              (unsigned long)nt->OptionalHeader.SizeOfImage);
        return 0;
    }
    return 1;
}

static DWORD WINAPI init_thread(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 2400; i++) {
        HMODULE dlc = GetModuleHandleA("HitmanDlc.dlc");
        HMODULE sys = GetModuleHandleA("System.dll");
        HMODULE glb = GetModuleHandleA("Globals.dll");
        if (dlc && sys && glb) {
            Sleep(500);   /* let static init settle */
            uint8_t **pp = (uint8_t **)(uintptr_t)GetProcAddress(glb,
                "?g_pSysInterface@@3PAVZSysInterface@@A");
            g_si = (pp && *pp) ? *pp : NULL;
            if (!g_si) { Sleep(250); continue; }
            g_dlc = (uint8_t *)dlc;
            g_sys = (uint8_t *)sys;
            g_dlc_delta = (uint32_t)(uintptr_t)g_dlc - DLC_PREF_BASE;
            int dlc_ok = module_ok(g_dlc, DLC_TIMESTAMP, DLC_SIZEOFIMAGE, "HitmanDlc.dlc");
            int sys_ok = module_ok(g_sys, SYS_TIMESTAMP, SYS_SIZEOFIMAGE, "System.dll");
            if (dlc_ok) {
                hook_entry(g_dlc + DLC_LCINIT_RVA, DLC_LCINIT_BYTES, 5,
                           on_mission_start, "mission-start");
                uint8_t lcdone[5];
                lcdone[0] = 0xA1;              /* mov eax,[VA_GAMEPTR] */
                uint32_t gp = dlc_va(VA_GAMEPTR);
                memcpy(lcdone + 1, &gp, 4);
                hook_entry(g_dlc + DLC_LCDONE_RVA, lcdone, 5,
                           on_mission_end, "mission-end");
            }
            if (sys_ok && dlc_ok)
                hook_frame();
            logf_("init done (dlc=%p sys=%p si=%p)", g_dlc, g_sys, g_si);
            return 0;
        }
        Sleep(250);
    }
    logf_("engine modules never appeared");
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
        snprintf(logpath, sizeof(logpath), "%s\\HC47HudExtras.log", g_dir);
        g_log = fopen(logpath, "w");
        read_ini();
        logf_("HC47 HUD Extras loaded: CrosshairScale=%.3f ShowTimer=%d ShowFPS=%d",
              (double)g_xhair, g_show_timer, g_show_fps);
        CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    }
    return TRUE;
}
