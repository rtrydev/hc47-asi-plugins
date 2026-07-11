/* HudExtras feature of hc47_tweaks.asi — crosshair shrink + mission
 * timer + FPS overlay for Hitman: Codename 47.
 *
 * Three independent features, all driven from a single per-frame hook on the
 * engine's game-tick call site (System.dll), so every engine call we make
 * runs on the game thread between tick and render:
 *
 * 1. CrosshairScale: the aiming crosshair / center dot is a GUI object in
 *    the level pack under Bitmaps/Pointers. Some builds expose it as a
 *    ZWINOBJ, whose native SetScale rescales the already-built quad vertices.
 *    The retail mission HUD exposes the active dot as a ZGEOMREF instead;
 *    for that class we scale only its private generated vertex buffer
 *    (+0x64, count +0x5c). We never touch its source transform at +0x04,
 *    because that matrix is shared with non-HUD geometry.
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
 * Config (hc47_tweaks.ini):
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

#include "common.h"

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
#define ZGEOMREF_VTBL_VA 0x0fec27a4u
#define SETSCALE_SLOT 0x4a4        /* ZWINOBJ vtable: SetScale(double[2], uchar) */

/* ---- hook sites ---- */
#define SYS_TICKCALL_RVA 0xd238    /* call [esi+0x3bdb] — per-frame game tick */
static const uint8_t SYS_TICKCALL_BYTES[6] = { 0xFF, 0x96, 0xDB, 0x3B, 0x00, 0x00 };
/* clock latch (prev=cur; cur=Update()) — runs exactly once per rendered
 * frame (the engine's own FPS math depends on it); the RunFrame call site
 * above turned out not to be on the live path, so this is the primary. */
#define SYS_LATCH_RVA    0xe2e0
static const uint8_t SYS_LATCH_BYTES[9] =
    { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xC5, 0x37, 0x00, 0x00 };
#define DLC_LCINIT_RVA   0x45fc0   /* LevelControl::Init (mission start) */
static const uint8_t DLC_LCINIT_BYTES[5] = { 0x8B, 0x44, 0x24, 0x04, 0x53 };
#define DLC_LCDONE_RVA   0x46fc0   /* LevelControl teardown (mission end) */
/* first insn is `mov eax,[VA_GAMEPTR]` — absolute address, so the expected
 * bytes are built at runtime with the relocation delta applied */

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
    va_list ap;
    va_start(ap, fmt);
    hc47_vlog("hudextras", fmt, ap);
    va_end(ap);
}

static void read_ini(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", hc47_dir, HC47_INI);
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

/* service container: [gSys+0x59]; vt+0x30 = object db, vt+0x2c = the
 * config/named-pointer registry ("pIngameDisplay" etc.) */
static void *svc_get(uint32_t slot)
{
    uint8_t **root = *(uint8_t ***)(uintptr_t)dlc_va(VA_GAMEPTR);
    if (!root || IsBadReadPtr(root, 4)) return NULL;
    uint8_t *p = (uint8_t *)*root;
    if (!p || IsBadReadPtr(p + 0x59, 4)) return NULL;
    uint8_t *owner;
    memcpy(&owner, p + 0x59, 4);
    if (!owner || IsBadReadPtr(owner, 4)) return NULL;
    void **vt = *(void ***)owner;
    if (IsBadReadPtr(vt + slot / 4, 4)) return NULL;
    vfn_this_t get = (vfn_this_t)vt[slot / 4];
    return get(owner, NULL);
}

static void *obj_db(void) { return svc_get(0x30); }

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

static double game_time(void);

/* ---- GUI tree walk ----
 * Group-branch classes (ZGROUP/ZWINDOWS/ZListBox2) share FindChild impl
 * DLC+0xaecd0 at vtable slot +0x3fc and keep a direct-pointer child list:
 * [grp+0x70] first child, [node+0x48] next sibling, [node+0x34] name,
 * [node+0x44] parent. The raw walk sees hidden nodes too (FindChild
 * skips flags 0x802 — the inactive crosshair variants carry those). */
#define GROUP_FINDCHILD_RVA 0xaecd0
#define OBJ_FIRSTCHILD 0x70
#define OBJ_NEXT       0x48
#define OBJ_NAME       0x34

static int node_is_group(uint8_t *n)
{
    void **vt = *(void ***)n;
    if (IsBadReadPtr(vt + 0x3fc / 4, 4)) return 0;
    return (uint32_t)(uintptr_t)vt[0x3fc / 4] ==
           (uint32_t)(uintptr_t)(g_dlc + GROUP_FINDCHILD_RVA);
}

static const char *node_name(uint8_t *n)
{
    char *nm = *(char **)(n + OBJ_NAME);
    return (nm && !IsBadReadPtr(nm, 2)) ? nm : "";
}

/* depth-first search: find first descendant with the given name.
 * `want_group` filters group-branch (1) / any (0). */
static uint8_t *tree_find(uint8_t *grp, const char *name, int want_group,
                          int depth)
{
    if (depth > 16) return NULL;
    for (uint8_t *n = *(uint8_t **)(grp + OBJ_FIRSTCHILD);
         n && !IsBadReadPtr(n, 0x7c);
         n = *(uint8_t **)(n + OBJ_NEXT)) {
        int isgrp = node_is_group(n);
        if ((!want_group || isgrp) && strcmp(node_name(n), name) == 0)
            return n;
        if (isgrp) {
            uint8_t *r = tree_find(n, name, want_group, depth + 1);
            if (r) return r;
        }
    }
    return NULL;
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

struct geom_scale_state {
    uint8_t *obj;
    float *verts;
    int count;
    float scale;
    float metric;
};
static struct geom_scale_state g_geom_scales[32];
static unsigned g_geom_scale_next;

static void winobj_setscale(uint8_t *obj, double s)
{
    double sc[2] = { s, s };
    uint32_t args[2] = { (uint32_t)(uintptr_t)sc, 0 };
    vcall(obj, SETSCALE_SLOT, 2, args);
}

static int scale_one_winobj(uint8_t *obj)
{
    if (!obj || IsBadReadPtr(obj, 0xe7)) return 0;
    if (*(uint32_t *)obj != dlc_va(ZWINOBJ_VTBL_VA)) return 0;
    double cur;
    memcpy(&cur, obj + 0xd7, 8);
    if (fabs(cur - (double)g_xhair) <= 1e-6) return 0;
    winobj_setscale(obj, (double)g_xhair);
    return 1;
}

static struct geom_scale_state *geom_scale_state(uint8_t *obj, float *verts,
                                                 int count)
{
    struct geom_scale_state *empty = NULL;
    for (unsigned i = 0; i < sizeof(g_geom_scales) / sizeof(g_geom_scales[0]); i++) {
        if (g_geom_scales[i].obj == obj) {
            if (g_geom_scales[i].verts != verts || g_geom_scales[i].count != count) {
                g_geom_scales[i].verts = verts;
                g_geom_scales[i].count = count;
                g_geom_scales[i].scale = 1.0f;
                g_geom_scales[i].metric = 0.0f;
            }
            return &g_geom_scales[i];
        }
        if (!g_geom_scales[i].obj && !empty)
            empty = &g_geom_scales[i];
    }
    if (!empty)
        empty = &g_geom_scales[(g_geom_scale_next++) %
                               (sizeof(g_geom_scales) / sizeof(g_geom_scales[0]))];
    empty->obj = obj;
    empty->verts = verts;
    empty->count = count;
    empty->scale = 1.0f;
    empty->metric = 0.0f;
    return empty;
}

static float geom_metric(float *verts, int count)
{
    float m = 0.0f;
    for (int i = 0; i < count; i++) {
        float *v = verts + i * 3;
        float ax = fabsf(v[0]);
        float ay = fabsf(v[1]);
        if (ax > m) m = ax;
        if (ay > m) m = ay;
    }
    return m;
}

static int scale_one_geomref(uint8_t *obj)
{
    if (!obj || IsBadReadPtr(obj, 0xa3)) return 0;
    if (*(uint32_t *)obj != dlc_va(ZGEOMREF_VTBL_VA)) return 0;

    int count = *(int *)(obj + 0x5c);
    float *verts = *(float **)(obj + 0x64);
    if (count <= 0 || count > 4096 ||
        !verts || IsBadReadPtr(verts, (UINT_PTR)count * 3 * sizeof(float)))
        return 0;

    struct geom_scale_state *st = geom_scale_state(obj, verts, count);
    float metric = geom_metric(verts, count);
    if (st->metric > 0.001f && st->scale == g_xhair &&
        metric > st->metric * 1.25f)
        st->scale = 1.0f;
    float old = st->scale > 0.001f ? st->scale : 1.0f;
    float ratio = g_xhair / old;
    if (fabs((double)ratio - 1.0) <= 1e-6)
        return 0;

    vcall(obj, 0x224, 0, NULL);
    *(uint32_t *)(obj + 0x3c) |= 0x60000;
    for (int i = 0; i < count; i++) {
        float *v = verts + i * 3;
        v[0] *= ratio;
        v[1] *= ratio;
    }
    st->scale = g_xhair;
    st->metric = geom_metric(verts, count);
    vcall(obj, 0x228, 0, NULL);
    return 1;
}

static int scale_one_crosshair(uint8_t *obj)
{
    if (!obj || IsBadReadPtr(obj, 4)) return 0;
    uint32_t vt = *(uint32_t *)obj;
    if (vt == dlc_va(ZWINOBJ_VTBL_VA))
        return scale_one_winobj(obj);
    if (vt == dlc_va(ZGEOMREF_VTBL_VA))
        return scale_one_geomref(obj);
    return 0;
}

static int plausible_matrix(float *m)
{
    if (!m || IsBadReadPtr(m, 9 * sizeof(float))) return 0;
    for (int i = 0; i < 9; i++) {
        float v = m[i];
        if (v != v) return 0;
        if (v < -10000.0f || v > 10000.0f) return 0;
    }
    return 1;
}

/* scale every CrossHair* drawable in this subtree */
static int scale_crosshairs(uint8_t *grp, int depth)
{
    if (depth > 16) return 0;
    int n_scaled = 0;
    for (uint8_t *n = *(uint8_t **)(grp + OBJ_FIRSTCHILD);
         n && !IsBadReadPtr(n, 0xe7);
         n = *(uint8_t **)(n + OBJ_NEXT)) {
        if (node_is_group(n)) {
            n_scaled += scale_crosshairs(n, depth + 1);
            continue;
        }
        if (strncmp(node_name(n), "CrossHair", 9) != 0) continue;
        if (scale_one_crosshair(n))
            n_scaled++;
    }
    return n_scaled;
}

static int scale_named_crosshairs(void *db)
{
    int n_scaled = 0;
    for (unsigned i = 0; i < N_POINTERS; i++) {
        uint32_t h = db_lookup(db, k_pointer_names[i]);
        if (!h) { g_ptr_handles[i] = 0; continue; }
        uint8_t *obj = handle_ptr(h);
        if (!obj || IsBadReadPtr(obj, 0xe7)) { g_ptr_handles[i] = 0; continue; }
        if (*(uint32_t *)obj != dlc_va(ZWINOBJ_VTBL_VA) &&
            *(uint32_t *)obj != dlc_va(ZGEOMREF_VTBL_VA)) {
            if (h != g_ptr_handles[i]) {
                logf_("crosshair: '%s' is not a scalable GUI object (vt=%08x) — skipped",
                      k_pointer_names[i], *(uint32_t *)obj);
                g_ptr_handles[i] = h;
            }
            continue;
        }
        if (h != g_ptr_handles[i] || scale_one_crosshair(obj)) {
            int changed = 1;
            if (h != g_ptr_handles[i])
                changed = scale_one_crosshair(obj);
            n_scaled++;
            g_ptr_handles[i] = h;
            if (!changed)
                n_scaled--;
        }
    }
    return n_scaled;
}

static int count_named_crosshairs(void *db)
{
    int n = 0;
    for (unsigned i = 0; i < N_POINTERS; i++) {
        uint32_t h = db_lookup(db, k_pointer_names[i]);
        uint8_t *obj = handle_ptr(h);
        if (obj && !IsBadReadPtr(obj, 4) &&
            (*(uint32_t *)obj == dlc_va(ZWINOBJ_VTBL_VA) ||
             *(uint32_t *)obj == dlc_va(ZGEOMREF_VTBL_VA)))
            n++;
    }
    return n;
}

static void dump_crosshair_tree(uint8_t *grp, uint8_t *wnd)
{
    static int done;
    if (done || !grp) return;
    done = 1;
    logf_("crosshair dump: grp=%p vt=%08x isgrp=%d flags=%08x first=%p "
          "wnd=%p vt=%08x isgrp=%d first=%p",
          grp,
          grp && !IsBadReadPtr(grp, 0x74) ? *(uint32_t *)grp : 0,
          grp && !IsBadReadPtr(grp, 0x74) ? node_is_group(grp) : 0,
          grp && !IsBadReadPtr(grp + 0x3c, 4) ? *(uint32_t *)(grp + 0x3c) : 0,
          grp && !IsBadReadPtr(grp + OBJ_FIRSTCHILD, 4) ? *(uint8_t **)(grp + OBJ_FIRSTCHILD) : NULL,
          wnd,
          wnd && !IsBadReadPtr(wnd, 0x74) ? *(uint32_t *)wnd : 0,
          wnd && !IsBadReadPtr(wnd, 0x74) ? node_is_group(wnd) : 0,
          wnd && !IsBadReadPtr(wnd + OBJ_FIRSTCHILD, 4) ? *(uint8_t **)(wnd + OBJ_FIRSTCHILD) : NULL);
    if (!grp || IsBadReadPtr(grp + OBJ_FIRSTCHILD, 4)) return;
    uint8_t *n = *(uint8_t **)(grp + OBJ_FIRSTCHILD);
    for (int i = 0; i < 12 && n && !IsBadReadPtr(n, 0xe7); i++) {
        float *mat = !IsBadReadPtr(n + 0x04, 4) ? *(float **)(n + 0x04) : NULL;
        logf_("crosshair child[%d]: p=%p vt=%08x flags=%08x isgrp=%d "
              "name='%s' next=%p first=%p scale=%.3f/%.3f mat=%p "
              "m=%.3f,%.3f,%.3f r90=%08x r94=%08x b98=%02x b99=%02x "
              "r9f=%08x rac=%08x vcnt=%d vtx=%p",
              i, n, *(uint32_t *)n,
              !IsBadReadPtr(n + 0x3c, 4) ? *(uint32_t *)(n + 0x3c) : 0,
              node_is_group(n), node_name(n),
              !IsBadReadPtr(n + OBJ_NEXT, 4) ? *(uint8_t **)(n + OBJ_NEXT) : NULL,
              !IsBadReadPtr(n + OBJ_FIRSTCHILD, 4) ? *(uint8_t **)(n + OBJ_FIRSTCHILD) : NULL,
              *(uint32_t *)n == dlc_va(ZWINOBJ_VTBL_VA) ? *(double *)(n + 0xd7) : 0.0,
              *(uint32_t *)n == dlc_va(ZWINOBJ_VTBL_VA) ? *(double *)(n + 0xdf) : 0.0,
              mat,
              plausible_matrix(mat) ? mat[0] : 0.0f,
              plausible_matrix(mat) ? mat[4] : 0.0f,
              plausible_matrix(mat) ? mat[8] : 0.0f,
              !IsBadReadPtr(n + 0x90, 4) ? *(uint32_t *)(n + 0x90) : 0,
              !IsBadReadPtr(n + 0x94, 4) ? *(uint32_t *)(n + 0x94) : 0,
              !IsBadReadPtr(n + 0x98, 1) ? n[0x98] : 0,
              !IsBadReadPtr(n + 0x99, 1) ? n[0x99] : 0,
              !IsBadReadPtr(n + 0x9f, 4) ? *(uint32_t *)(n + 0x9f) : 0,
              !IsBadReadPtr(n + 0xac, 4) ? *(uint32_t *)(n + 0xac) : 0,
              !IsBadReadPtr(n + 0x5c, 4) ? *(int *)(n + 0x5c) : 0,
              !IsBadReadPtr(n + 0x64, 4) ? *(float **)(n + 0x64) : NULL);
        n = !IsBadReadPtr(n + OBJ_NEXT, 4) ? *(uint8_t **)(n + OBJ_NEXT) : NULL;
    }
}

static void crosshair_tick(void)
{
    if (g_xhair == 1.0f) return;
    static int refresh;
    static uint32_t grp_h, wnd_h;
    void *db = NULL;
    uint8_t *grp = handle_ptr(grp_h);
    uint8_t *wnd = handle_ptr(wnd_h);
    int total = 0;

    if (refresh-- <= 0 || !grp || !wnd) {
        db = obj_db();
        if (!db && !grp && !wnd) return;
        refresh = (grp || wnd) ? 20 : 0;
        if (db) {
            grp_h = db_lookup(db, "CrossHair");
            wnd_h = db_lookup(db, "rWindows");
            grp = handle_ptr(grp_h);
            wnd = handle_ptr(wnd_h);
        }
    }

    if (scale_one_crosshair(grp))
        total++;
    if (grp && !IsBadReadPtr(grp, 0x7c) && node_is_group(grp))
        total += scale_crosshairs(grp, 0);
    dump_crosshair_tree(grp, wnd);
    if (wnd && !IsBadReadPtr(wnd, 0x7c) && node_is_group(wnd))
        total += scale_crosshairs(wnd, 0);
    /* direct DB lookups are kept as a fallback for builds/levels whose GUI
     * list layout differs from the verified tree path. */
    if (db)
        total += scale_named_crosshairs(db);
    if (total) {
        static int announced;
        if (!announced++)
            logf_("crosshair: %d sprite(s) scaled to %.3f", total,
                  (double)g_xhair);
    } else {
        static int diag;
        if ((diag++ % 40) == 0)
            logf_("crosshair probe: no sprites scaled (grp=%p wnd=%p named=%d)",
                  grp, wnd, db ? count_named_crosshairs(db) : -1);
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
static uint32_t g_last_wnd_h;      /* mission-start detector */
static char g_lbl_txt[2][32];
static float g_lbl_x[2], g_lbl_y[2];

static uint32_t obj_handle(uint8_t *obj) { return ((uint32_t *)obj)[-1]; }

static uint8_t *label_create(uint8_t *parent, uint8_t *font,
                             const char *name, float x, float y)
{
    uint32_t a3[3] = { (uint32_t)(uintptr_t)name,
                       (uint32_t)(uintptr_t)"ZCHAROBJ", 0 };
    uint8_t *lbl = (uint8_t *)(uintptr_t)vcall(parent, 0x444, 3, a3);
    if (!lbl) return NULL;
    { uint32_t a[4] = { 1, 0, 0, 0 }; vcall(lbl, 0x25c, 4, a); }
    *(uint32_t *)(lbl + 0x3c) |= 0x80000;          /* runtime-created */
    { uint32_t a[3] = { (uint32_t)(uintptr_t)font, 0, 0 }; vcall(lbl, 0x4d4, 3, a); }
    { uint32_t a[1] = { 0 }; vcall(lbl, 0x214, 1, a); }
    { uint32_t a[1] = { 1 }; vcall(lbl, 0x4ac, 1, a); }
    { uint32_t a[2] = { (uint32_t)(uintptr_t)"", 0 }; vcall(lbl, 0x4e0, 2, a); }
    {
        uint32_t col = 0x00c0c0c0;
        uint32_t *osd = (uint32_t *)(uintptr_t)dlc_va(VA_OSDCOLOR);
        if (!IsBadReadPtr(osd, 4) && *osd) col = *osd;
        uint32_t a[1] = { col };
        vcall(lbl, 0x488, 1, a);
    }
    { uint32_t a[2] = { 0x12, 0xff }; vcall(lbl, 0x47c, 2, a); }
    { uint32_t a[3] = { 0x40800000u, 0, 0 }; vcall(lbl, 0x4b0, 3, a); }
    {
        float pos[3] = { x, y, 1.0f };
        uint32_t a[1] = { (uint32_t)(uintptr_t)pos };
        vcall(lbl, 0x14c, 1, a);
    }
    return lbl;
}

static void label_setpos(int i)
{
    if (!g_lbl[i]) return;
    float pos[3] = { g_lbl_x[i], g_lbl_y[i], 1.0f };
    uint32_t a[1] = { (uint32_t)(uintptr_t)pos };
    vcall(g_lbl[i], 0x14c, 1, a);
}

static void label_settext(int i, const char *txt)
{
    if (!g_lbl[i]) return;
    if (strcmp(g_lbl_txt[i], txt) == 0) return;
    strncpy(g_lbl_txt[i], txt, sizeof(g_lbl_txt[i]) - 1);
    uint32_t a[2] = { (uint32_t)(uintptr_t)txt, 0 };
    vcall(g_lbl[i], 0x4e0, 2, a);
    label_setpos(i);
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
        if (handle_ptr(g_parent_h) != g_parent) {
            logf_("hudtext: level GUI gone, labels dropped");
            hudtext_drop();
        }
    }

    if (!g_parent) {
        /* (re)acquire — a few times a second is enough */
        static int cooldown, diag;
        if (cooldown-- > 0) return;
        cooldown = 20;
        void *db = obj_db();
        if (!db) return;
        int noisy = (diag++ % 40) == 0;

        /* HUD font: the engine's own in-mission route is
         * rWindows->GetFontByName("BankGothic10") (vt+0x4b0 = 0x847d0) */
        uint8_t *wnd = handle_ptr(db_lookup(db, "rWindows"));
        uint8_t *font = NULL;
        if (wnd && !IsBadReadPtr(wnd, 4)) {
            void **wvt = *(void ***)wnd;
            if (!IsBadReadPtr(wvt + 0x4b0 / 4, 4) &&
                (uint8_t *)wvt[0x4b0 / 4] == g_dlc + 0x847d0) {
                uint32_t a[1] = { (uint32_t)(uintptr_t)"BankGothic10" };
                font = (uint8_t *)(uintptr_t)vcall(wnd, 0x4b0, 1, a);
            }
        }
        if (!font)
            font = handle_ptr(db_lookup(db, "BankGothic10"));

        /* HUD parent group: the IngameDisplay impl (registered as
         * "pIngameDisplay" in the config registry, vtable 0x1f3988)
         * keeps the OSD label group handle at +0x33 and the HUD font
         * at +0x184 */
        uint8_t *impl = NULL;
        void *reg = svc_get(0x2c);
        if (reg) {
            uint32_t out = 0;
            uint32_t a[2] = { (uint32_t)(uintptr_t)"pIngameDisplay",
                              (uint32_t)(uintptr_t)&out };
            vcall(reg, 0x9c, 2, a);
            uint8_t *cand = (uint8_t *)(uintptr_t)out;
            for (int hop = 0; hop < 2 && cand && !IsBadReadPtr(cand, 4); hop++) {
                if (*(uint32_t *)cand == (uint32_t)(uintptr_t)(g_dlc + 0x1f3988)) {
                    impl = cand;
                    break;
                }
                cand = *(uint8_t **)cand;
            }
        }
        uint8_t *parent = NULL;
        uint32_t ph = 0;
        if (impl) {
            memcpy(&ph, impl + 0x33, 4);
            parent = handle_ptr(ph);
            if (!font && !IsBadReadPtr(impl + 0x184, 4))
                font = *(uint8_t **)(impl + 0x184);
        }
        if (!parent && wnd && node_is_group(wnd)) {
            parent = tree_find(wnd, "IngameDisplay", 1, 0);
            if (parent) ph = obj_handle(parent);
        }
        if (!parent) {
            ph = db_lookup(db, "IngameDisplay");
            parent = handle_ptr(ph);
        }
        if (noisy)
            logf_("probe: wnd=%p font=%p impl=%p parent=%p(h=%08x) t=%.1f",
                  wnd, font, impl, parent, ph, game_time());
        if (!parent || !font) return;           /* not in a mission yet */

        /* creation is only valid on window/group classes (vt+0x444
         * must be the shared CreateChild impl 0xac600) */
        void **pvt = *(void ***)parent;
        if (IsBadReadPtr(pvt + 0x444 / 4, 4) ||
            (uint8_t *)pvt[0x444 / 4] != g_dlc + 0xac600) {
            if (noisy)
                logf_("hudtext: parent vt+0x444=%p not CreateChild — skipped",
                      pvt[0x444 / 4]);
            return;
        }

        /* the HUD (re)appearing is the mission-start signal —
         * LevelControl::Init turned out to fire only at boot */
        if (ph != g_last_wnd_h) {
            g_last_wnd_h = ph;
            g_mission_start = game_time();
            g_in_mission = 1;
            logf_("mission start (HUD group %08x) at t=%.2f",
                  ph, g_mission_start);
        }
        float y = g_text_y;
        if (g_show_timer) {
            g_lbl[0] = label_create(parent, font, "HXTIMER", g_text_x, y);
            if (g_lbl[0]) {
                g_lbl_h[0] = obj_handle(g_lbl[0]);
                g_lbl_x[0] = g_text_x; g_lbl_y[0] = y;
                y += g_line_gap;
            }
        }
        if (g_show_fps) {
            g_lbl[1] = label_create(parent, font, "HXFPS", g_text_x, y);
            if (g_lbl[1]) {
                g_lbl_h[1] = obj_handle(g_lbl[1]);
                g_lbl_x[1] = g_text_x; g_lbl_y[1] = y;
            }
        }
        if (!g_lbl[0] && !g_lbl[1]) return;
        g_parent = parent;
        g_parent_h = ph;
        logf_("hudtext: labels created (parent=%p font=%p fontvt=%08x "
              "timer=%p/%08x fps=%p/%08x)",
              parent, font, *(uint32_t *)font,
              g_lbl[0], g_lbl[0] ? *(uint32_t *)(g_lbl[0] + 0x3c) : 0,
              g_lbl[1], g_lbl[1] ? *(uint32_t *)(g_lbl[1] + 0x3c) : 0);
    }

    if (g_lbl[0]) {
        char buf[32];
        if (g_in_mission) {
            double now = game_time();
            if (now < g_mission_start) {
                /* engine clock was reset (seen at level transitions) */
                g_mission_start = now;
                logf_("clock went backwards — mission start re-latched at t=%.2f", now);
            }
            double e = now - g_mission_start;
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
    /* several hooks may fire per frame (RunFrame tick call, renderer
     * end-of-frame slots) — run the work once per engine-clock value */
    static double last = -1.0;
    double t = game_time();
    if (t == last) return;
    last = t;
    static LONG n;
    if (InterlockedIncrement(&n) == 1)
        logf_("frame tick running (t=%.2f)", t);
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

/* ---- renderer end-of-frame vtable hook ----
 * The RunFrame tick call site (System+0xd238) only runs during boot; the
 * in-game loop reaches the renderer's end-of-frame function every frame
 * instead. It sits in several RenderD3D vtables (RVA 0x24b90: slot +0x100
 * of vtable 0x35190, +0xbc of 0x35aa8, +0x98 of 0x35d70). We swap those
 * slots (atomic 4-byte data writes, same mechanism as the rtrace tool)
 * to stubs that run frame_tick then jump to the original. */
#define RD3D_EOF_RVA 0x24b90
static const struct { uint32_t vt_rva, slot; } k_eof_slots[] = {
    { 0x35190, 0x100 },
    { 0x35aa8, 0x0bc },
    { 0x35d70, 0x098 },
};
#define N_EOF (sizeof(k_eof_slots)/sizeof(k_eof_slots[0]))
static void *g_eof_orig[N_EOF];    /* jmp targets for the stubs */

static int hook_render_eof(uint8_t *rd3d)
{
    uint8_t *stubs = (uint8_t *)VirtualAlloc(NULL, 32 * N_EOF,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return 0;
    int hooked = 0;
    for (unsigned i = 0; i < N_EOF; i++) {
        uint32_t *slot = (uint32_t *)(rd3d + k_eof_slots[i].vt_rva
                                           + k_eof_slots[i].slot);
        if (IsBadReadPtr(slot, 4)) continue;
        uint32_t tgt = *slot;
        if (tgt != (uint32_t)(uintptr_t)(rd3d + RD3D_EOF_RVA)) {
            logf_("render-eof: vt%05x+%03x points at %08x (expected rd3d+%x) — skipped",
                  k_eof_slots[i].vt_rva, k_eof_slots[i].slot, tgt, RD3D_EOF_RVA);
            continue;
        }
        g_eof_orig[i] = (void *)(uintptr_t)tgt;
        uint8_t *p = stubs + 32 * i;
        uint8_t *stub = p;
        *p++ = 0x9C; *p++ = 0x60;             /* pushfd; pushad */
        *p++ = 0xE8;                          /* call frame_tick */
        *(int32_t *)p = (int32_t)((uint8_t *)(uintptr_t)frame_tick - (p + 4)); p += 4;
        *p++ = 0x61; *p++ = 0x9D;             /* popad; popfd */
        *p++ = 0xFF; *p++ = 0x25;             /* jmp dword ptr [orig] */
        *(uint32_t *)p = (uint32_t)(uintptr_t)&g_eof_orig[i]; p += 4;
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) continue;
        *(volatile uint32_t *)slot = (uint32_t)(uintptr_t)stub;
        VirtualProtect(slot, 4, old, &old);
        hooked++;
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 32 * N_EOF);
    logf_("render-eof hook: %d/%d slots", hooked, (int)N_EOF);
    return hooked;
}

/* OpenGL renderer path: RenderOpenGL.dll presents through GDI32 SwapBuffers.
 * Hooking its IAT gives us the same once-per-present anchor as the D3D
 * end-of-frame slots without needing renderer-specific vtable RVAs. */
typedef BOOL (WINAPI *swapbuffers_t)(HDC);
static swapbuffers_t g_swapbuffers_orig;

BOOL WINAPI hook_swapbuffers(HDC hdc)
{
    frame_tick();
    return g_swapbuffers_orig(hdc);
}

static int hook_iat(HMODULE mod, const char *dll, const char *name,
                    void *hook, void **orig)
{
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    IMAGE_IMPORT_DESCRIPTOR *imp =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    for (; !IsBadReadPtr(imp, sizeof(*imp)) && imp->Name; imp++) {
        char *modname = (char *)(base + imp->Name);
        if (IsBadStringPtrA(modname, 256) || _stricmp(modname, dll) != 0)
            continue;
        IMAGE_THUNK_DATA32 *names = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA32 *)(base + imp->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA32 *funcs =
            (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        for (; !IsBadReadPtr(names, sizeof(*names)) && names->u1.AddressOfData;
             names++, funcs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME *byn =
                (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (IsBadStringPtrA((char *)byn->Name, 256) ||
                strcmp((char *)byn->Name, name) != 0)
                continue;
            if ((void *)(uintptr_t)funcs->u1.Function == hook) return 1;
            *orig = (void *)(uintptr_t)funcs->u1.Function;
            DWORD old;
            if (!VirtualProtect(&funcs->u1.Function, 4, PAGE_READWRITE, &old))
                return 0;
            funcs->u1.Function = (uint32_t)(uintptr_t)hook;
            VirtualProtect(&funcs->u1.Function, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), &funcs->u1.Function, 4);
            return 1;
        }
    }
    return 0;
}

static int hook_render_opengl(HMODULE ogl)
{
    int ok = hook_iat(ogl, "GDI32.dll", "SwapBuffers",
                      hook_swapbuffers, (void **)&g_swapbuffers_orig);
    logf_("render-opengl SwapBuffers hook: %s", ok ? "installed" : "not found");
    return ok;
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
            if (sys_ok && dlc_ok) {
                hook_entry(g_sys + SYS_LATCH_RVA, SYS_LATCH_BYTES, 9,
                           frame_tick, "frame-latch");
                hook_frame();   /* boot-time only; in-game loop skips it */
            }
            logf_("init done (dlc=%p sys=%p si=%p)", g_dlc, g_sys, g_si);
            /* renderer loads at display-mode setup — wait and hook its
             * end-of-frame path (the real in-game frame anchor) */
            if (dlc_ok) {
                int hooked = 0;
                int tried_d3d = 0, tried_ogl = 0;
                for (int j = 0; j < 2400; j++) {
                    HMODULE rd3d = GetModuleHandleA("RenderD3D.dll");
                    if (rd3d && !tried_d3d) {
                        Sleep(500);
                        tried_d3d = 1;
                        hooked = hook_render_eof((uint8_t *)rd3d);
                        if (hooked) break;
                    }
                    HMODULE ogl = GetModuleHandleA("RenderOpenGL.dll");
                    if (ogl && !tried_ogl) {
                        Sleep(500);
                        tried_ogl = 1;
                        hooked = hook_render_opengl(ogl);
                        if (hooked) break;
                    }
                    if (tried_d3d && tried_ogl) break;
                    Sleep(250);
                }
                if (!hooked)
                    logf_("render hook: no supported renderer hook installed");
            }
            return 0;
        }
        Sleep(250);
    }
    logf_("engine modules never appeared");
    return 0;
}

void hudextras_init(void)
{
    read_ini();
    logf_("CrosshairScale=%.3f ShowTimer=%d ShowFPS=%d",
          (double)g_xhair, g_show_timer, g_show_fps);
    CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
}
