/* HC47 render-interface tracer (32-bit ASI, diagnostic tool).
 *
 * Hooks every entry of the RenderD3D.dll vtables (render class + related
 * classes) with register-preserving logging thunks. Logs per-entry call
 * counts and, for the first few calls of each entry, the caller address
 * (resolved to module+rva), `this`, and the first stack args — enough to
 * identify the 2D/HUD draw path and who calls it.
 *
 * Output: scripts/hc47_render_trace.log
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* vtable runs in RenderD3D.dll (RVA, entry count) — from offline analysis
 * of the retail DLL (timestamp checked at runtime). */
static const struct { uint32_t rva; uint32_t n; } k_vtabs[] = {
    { 0x35190,  21 },
    { 0x351f8, 115 },   /* main ZRenderClass interface */
    { 0x35450,  35 },
    { 0x35aa8,  93 },
    { 0x35c48,  71 },
    { 0x35d70, 114 },   /* base render class interface */
    { 0x35f44, 261 },   /* trailing vtable cluster */
};
#define N_VTABS (sizeof(k_vtabs)/sizeof(k_vtabs[0]))
#define RD3D_TIMESTAMP_EXPECTED 0  /* 0 = accept any, we log it */

#define MAX_HOOKS 1024
#define DETAIL_PER_HOOK 6
#define N_ARGS 10

typedef struct {
    uint32_t caller, ecx;
    uint32_t args[N_ARGS];
} Rec;

typedef struct {
    uint32_t vt_rva;        /* vtable this slot belongs to */
    uint32_t idx;           /* index within that vtable */
    uint32_t target;        /* original function (VA) */
    volatile LONG count;
    volatile LONG detail_n; /* how many detail recs are valid */
    Rec detail[DETAIL_PER_HOOK];
} Hook;

static Hook g_hooks[MAX_HOOKS];
static int g_n_hooks;
static uint8_t *g_thunks;
static FILE *g_log;
static CRITICAL_SECTION g_loglock;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_loglock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_loglock);
}

/* called from thunks; must be fast and reentrant */
void __cdecl trace_entry(uint32_t idx, uint32_t *regs)
{
    Hook *h = &g_hooks[idx];
    LONG n = InterlockedIncrement(&h->count) - 1;
    if (n < DETAIL_PER_HOOK) {
        Rec *r = &h->detail[n];
        r->ecx = regs[6];              /* pushad: EDI ESI EBP ESP EBX EDX ECX EAX */
        r->caller = regs[9];           /* after pushad(8) + pushfd(1) */
        for (int i = 0; i < N_ARGS; i++)
            r->args[i] = regs[10 + i];
        InterlockedIncrement(&h->detail_n);
    }
}

/* thunk layout (24 bytes):
 *   9C            pushfd
 *   60            pushad
 *   54            push esp
 *   68 iiiiiiii   push idx
 *   E8 rrrrrrrr   call trace_entry
 *   83 C4 08      add esp, 8
 *   61            popad
 *   9D            popfd
 *   E9 rrrrrrrr   jmp orig
 */
#define THUNK_SIZE 32
static void make_thunk(uint8_t *t, uint32_t idx, uint32_t target)
{
    uint8_t *p = t;
    *p++ = 0x9C; *p++ = 0x60; *p++ = 0x54;
    *p++ = 0x68; *(uint32_t *)p = idx; p += 4;
    *p++ = 0xE8; *(int32_t *)p = (int32_t)((uint32_t)(uintptr_t)trace_entry
                                           - (uint32_t)(uintptr_t)(p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x61; *p++ = 0x9D;
    *p++ = 0xE9; *(int32_t *)p = (int32_t)(target - (uint32_t)(uintptr_t)(p + 4)); p += 4;
}

/* ---- module map for caller resolution ---- */
typedef struct { uint32_t base, size; char name[64]; } ModInfo;
static ModInfo g_mods[128];
static int g_n_mods;

static void refresh_mods(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    int n = 0;
    if (Module32First(snap, &me)) {
        do {
            if (n >= 128) break;
            g_mods[n].base = (uint32_t)(uintptr_t)me.modBaseAddr;
            g_mods[n].size = me.modBaseSize;
            strncpy(g_mods[n].name, me.szModule, 63);
            n++;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    g_n_mods = n;
}

static void resolve(uint32_t addr, char *out, size_t sz)
{
    for (int i = 0; i < g_n_mods; i++) {
        if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size) {
            snprintf(out, sz, "%s+0x%x", g_mods[i].name, addr - g_mods[i].base);
            return;
        }
    }
    snprintf(out, sz, "0x%08x", addr);
}

static int plausible_float(uint32_t v, float *f)
{
    float x; memcpy(&x, &v, 4);
    if (x != x) return 0;
    if (x == 0.0f) { *f = 0; return 1; }
    float a = x < 0 ? -x : x;
    if (a >= 1e-4f && a <= 1e5f) { *f = x; return 1; }
    return 0;
}

static void dump_sysiface(void)
{
    HMODULE g = GetModuleHandleA("Globals.dll");
    if (!g) return;
    uint8_t **pp = (uint8_t **)(uintptr_t)GetProcAddress(g,
        "?g_pSysInterface@@3PAVZSysInterface@@A");
    if (!pp || !*pp) return;
    uint8_t *si = *pp;
    float *f = (float *)(si + 0xa95);
    logf_("sysiface: res=%dx%d cur=%dx%d win=%d  a95=%.2f a99=%.2f a9d=%.2f "
          "aa1=%.2f aa5=%.4g aa9=%.4g aad=%.4g",
          *(int32_t *)(si + 0x19), *(int32_t *)(si + 0x1d),
          *(int32_t *)(si + 0x21), *(int32_t *)(si + 0x25),
          si[0x12], f[0], f[1], f[2], f[3],
          (double)*(float *)(si + 0xaa5), (double)*(float *)(si + 0xaa9),
          (double)*(float *)(si + 0xaad));
}

static DWORD WINAPI dump_thread(LPVOID arg)
{
    (void)arg;
    static LONG last_count[MAX_HOOKS];
    static LONG last_detail[MAX_HOOKS];
    for (;;) {
        Sleep(2500);
        refresh_mods();
        logf_("---- tick t=%lu ----", (unsigned long)GetTickCount());
        dump_sysiface();
        for (int i = 0; i < g_n_hooks; i++) {
            Hook *h = &g_hooks[i];
            LONG c = h->count;
            if (c == last_count[i]) continue;
            logf_("vt%05x[%3lu] fn=0x%08x calls=%ld (+%ld)",
                  (unsigned)h->vt_rva, (unsigned long)h->idx,
                  (unsigned)h->target, (long)c, (long)(c - last_count[i]));
            last_count[i] = c;
            LONG dn = h->detail_n;
            if (dn > DETAIL_PER_HOOK) dn = DETAIL_PER_HOOK;
            for (LONG d = last_detail[i]; d < dn; d++) {
                Rec *r = &h->detail[d];
                char who[96];
                resolve(r->caller, who, sizeof(who));
                char argbuf[512]; int off = 0;
                for (int a = 0; a < N_ARGS; a++) {
                    float f;
                    if (plausible_float(r->args[a], &f))
                        off += snprintf(argbuf + off, sizeof(argbuf) - off,
                                        " %08x(%.1f)", r->args[a], f);
                    else
                        off += snprintf(argbuf + off, sizeof(argbuf) - off,
                                        " %08x", r->args[a]);
                }
                logf_("    from %s this=%08x args:%s", who, r->ecx, argbuf);
            }
            last_detail[i] = dn;
        }
    }
    return 0;
}

static int install(HMODULE rd3d)
{
    uint8_t *base = (uint8_t *)rd3d;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    logf_("RenderD3D.dll base=%p timestamp=%08lx sizeofimage=%08lx",
          base, (unsigned long)nt->FileHeader.TimeDateStamp,
          (unsigned long)nt->OptionalHeader.SizeOfImage);

    uint32_t text_lo = 0x1000, text_hi = 0x35000;   /* .text range (RVA) */

    int total = 0;
    for (unsigned v = 0; v < N_VTABS; v++) total += k_vtabs[v].n;
    g_thunks = (uint8_t *)VirtualAlloc(NULL, total * THUNK_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_thunks) return 0;

    for (unsigned v = 0; v < N_VTABS; v++) {
        uint32_t *slots = (uint32_t *)(base + k_vtabs[v].rva);
        DWORD old;
        if (!VirtualProtect(slots, k_vtabs[v].n * 4, PAGE_READWRITE, &old)) {
            logf_("vt %05x: VirtualProtect failed", (unsigned)k_vtabs[v].rva);
            continue;
        }
        for (uint32_t i = 0; i < k_vtabs[v].n; i++) {
            if (g_n_hooks >= MAX_HOOKS) break;
            uint32_t tgt = slots[i];
            uint32_t rva = tgt - (uint32_t)(uintptr_t)base;
            if (rva < text_lo || rva >= text_hi) continue;  /* safety */
            Hook *h = &g_hooks[g_n_hooks];
            h->vt_rva = k_vtabs[v].rva;
            h->idx = i;
            h->target = tgt;
            uint8_t *t = g_thunks + g_n_hooks * THUNK_SIZE;
            make_thunk(t, g_n_hooks, tgt);
            slots[i] = (uint32_t)(uintptr_t)t;
            g_n_hooks++;
        }
        VirtualProtect(slots, k_vtabs[v].n * 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), g_thunks, total * THUNK_SIZE);
    logf_("installed %d vtable hooks", g_n_hooks);
    return 1;
}

static DWORD WINAPI wait_thread(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 2400; i++) {   /* up to 10 min */
        HMODULE m = GetModuleHandleA("RenderD3D.dll");
        if (m) {
            /* give its static init a moment, then hook */
            Sleep(500);
            install(m);
            CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
            return 0;
        }
        Sleep(250);
    }
    logf_("RenderD3D.dll never appeared");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_loglock);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, sizeof(path));
        char *sl = strrchr(path, '\\');
        if (sl) *sl = 0;
        strncat(path, "\\hc47_render_trace.log", sizeof(path) - strlen(path) - 1);
        g_log = fopen(path, "w");
        logf_("HC47 render tracer loaded");
        CreateThread(NULL, 0, wait_thread, NULL, 0, NULL);
    }
    return TRUE;
}
