/* HC47 EIP-sampling profiler (32-bit ASI, diagnostic tool).
 *
 * A sampler thread periodically suspends every other thread in the process,
 * reads EIP via GetThreadContext (under CrossOver/Rosetta this is the
 * emulated x86 context) and resumes it. Samples are bucketed by module and
 * by 16-byte EIP bucket, so hot untranslated functions show up directly as
 * module+rva.
 *
 * If HC47ReducedX87 patch files are present next to this ASI, the profiler
 * parses them and follows the installed 5-byte entry hooks to find each
 * translated blob. Samples inside a blob are attributed back to the
 * ORIGINAL function rva ("x87:module+rva"), so translated and untranslated
 * time can be compared like-for-like.
 *
 * Since translated entries are hooked away from the module, module .text
 * samples are (almost) exclusively untranslated code — the top-bucket list
 * is the priority list for extending translation coverage.
 *
 * Output: scripts/HC47Profile.log, a cumulative report every DUMP_MS.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MS   4       /* ~250 Hz per thread */
#define REFRESH_MS  2000    /* module/thread list refresh */
#define DUMP_MS     10000   /* report interval */

/* ---- .x87 patch file format (matches tools/translate.py) ---- */
#pragma pack(push, 1)
typedef struct {
    char     magic[8];          /* "HC47X87P" */
    uint32_t version;
    uint32_t preferred_base;
    uint32_t timedatestamp;
    uint32_t size_of_image;
    uint32_t n_funcs;
    uint32_t blob_total;
    char     module[32];
} PatchHeader;

typedef struct {
    uint32_t rva, blob_off, blob_len, fixup_idx, n_fixups;
} FuncRec;
#pragma pack(pop)

/* ---- logging ---- */
static FILE *g_log;

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

/* ---- address slots: modules, translated blobs, anon fallback ----
 * Persistent for the whole run (never reordered/removed) so counters
 * stay attached to the right name even as modules load later. */
typedef struct {
    uint32_t base, size;
    char name[64];
    uint32_t count, last;   /* cumulative samples, count at previous dump */
} Slot;

#define MAX_SLOTS 160
static Slot g_slots[MAX_SLOTS];    /* slot 0 = "<anon>" */
static int g_n_slots;

static int slot_add(uint32_t base, uint32_t size, const char *name)
{
    if (g_n_slots >= MAX_SLOTS) return 0;
    Slot *s = &g_slots[g_n_slots];
    s->base = base;
    s->size = size;
    strncpy(s->name, name, sizeof(s->name) - 1);
    return g_n_slots++;
}

static int slot_find(uint32_t addr)
{
    for (int i = 1; i < g_n_slots; i++)
        if (addr - g_slots[i].base < g_slots[i].size)
            return i;
    return 0;
}

static void refresh_mods(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            uint32_t base = (uint32_t)(uintptr_t)me.modBaseAddr;
            int known = 0;
            for (int i = 1; i < g_n_slots; i++)
                if (g_slots[i].base == base) { known = 1; break; }
            if (!known && slot_add(base, me.modBaseSize, me.szModule))
                logf_("module: %s base=%08x size=%x",
                      me.szModule, base, (unsigned)me.modBaseSize);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

/* ---- translated-blob attribution from .x87 patch files ---- */
typedef struct {
    char module[32];
    uint32_t n_funcs;
    FuncRec *funcs;         /* file order */
    uint32_t *order;        /* indexes into funcs, sorted by blob_off */
    uint32_t *fcount;       /* samples per func */
    uint32_t blob_total;
    uint32_t blob_base;     /* 0 until hooks located */
    uint32_t other;         /* blob samples outside any func range */
    int slot;               /* slot index once active */
    int dead;               /* gave up (bad file) */
} XPatch;

#define MAX_PATCHES 16
static XPatch g_patches[MAX_PATCHES];
static int g_n_patches;

static const FuncRec *g_sort_funcs;
static int cmp_by_bloboff(const void *a, const void *b)
{
    uint32_t x = g_sort_funcs[*(const uint32_t *)a].blob_off;
    uint32_t y = g_sort_funcs[*(const uint32_t *)b].blob_off;
    return x < y ? -1 : x > y;
}

static void load_patch_files(const char *dir)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\HC47ReducedX87\\*.x87", dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        logf_("no .x87 patch files (translated-code attribution disabled)");
        return;
    }
    do {
        if (g_n_patches >= MAX_PATCHES) break;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\HC47ReducedX87\\%s", dir,
                 fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        PatchHeader h;
        if (fread(&h, sizeof(h), 1, f) != 1 ||
            memcmp(h.magic, "HC47X87P", 8) || h.version != 1 || !h.n_funcs) {
            fclose(f);
            continue;
        }
        XPatch *p = &g_patches[g_n_patches];
        p->n_funcs = h.n_funcs;
        p->blob_total = h.blob_total;
        memcpy(p->module, h.module, 32);
        p->module[31] = 0;
        p->funcs = (FuncRec *)HeapAlloc(GetProcessHeap(), 0,
                                        h.n_funcs * sizeof(FuncRec));
        p->order = (uint32_t *)HeapAlloc(GetProcessHeap(), 0,
                                         h.n_funcs * 4);
        p->fcount = (uint32_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          h.n_funcs * 4);
        if (!p->funcs || !p->order || !p->fcount ||
            fread(p->funcs, sizeof(FuncRec), h.n_funcs, f) != h.n_funcs) {
            fclose(f);
            continue;
        }
        fclose(f);
        for (uint32_t i = 0; i < h.n_funcs; i++) p->order[i] = i;
        g_sort_funcs = p->funcs;
        qsort(p->order, h.n_funcs, 4, cmp_by_bloboff);
        g_n_patches++;
        logf_("patch file: %s (%s, %lu funcs)", fd.cFileName, p->module,
              (unsigned long)h.n_funcs);
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
}

/* Follow the 5-byte jmp the x87 ASI installed at each translated entry to
 * derive the blob base; a few entries must agree before we trust it. */
static void try_activate_patch(XPatch *p)
{
    HMODULE m = GetModuleHandleA(p->module);
    if (!m) return;
    uint8_t *base = (uint8_t *)m;
    uint32_t blob_base = 0;
    uint32_t checked = 0, want = p->n_funcs < 8 ? p->n_funcs : 8;
    for (uint32_t i = 0; i < p->n_funcs && checked < want; i++) {
        uint8_t *entry = base + p->funcs[i].rva;
        if (entry[0] != 0xE9) return;   /* not hooked (yet) */
        uint32_t target = (uint32_t)(uintptr_t)(entry + 5)
                          + *(int32_t *)(entry + 1);
        uint32_t bb = target - p->funcs[i].blob_off;
        if (!checked) blob_base = bb;
        else if (bb != blob_base) {
            logf_("[%s] inconsistent hooks; ignoring blob", p->module);
            p->dead = 1;
            return;
        }
        checked++;
    }
    char name[64];
    snprintf(name, sizeof(name), "x87:%s", p->module);
    p->slot = slot_add(blob_base, p->blob_total, name);
    if (!p->slot) { p->dead = 1; return; }
    p->blob_base = blob_base;
    logf_("[%s] translated blob at %08x (+%lu KB)", p->module, blob_base,
          (unsigned long)p->blob_total / 1024);
}

/* blob sample -> func index, or -1 */
static int patch_func_at(XPatch *p, uint32_t off)
{
    uint32_t lo = 0, hi = p->n_funcs;
    while (lo < hi) {                    /* first order[] with blob_off > off */
        uint32_t mid = (lo + hi) / 2;
        if (p->funcs[p->order[mid]].blob_off <= off) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return -1;
    FuncRec *fr = &p->funcs[p->order[lo - 1]];
    if (off - fr->blob_off < fr->blob_len) return (int)p->order[lo - 1];
    return -1;
}

/* ---- EIP bucket hash (16-byte buckets, open addressing) ---- */
#define HASH_BITS 15
#define HASH_SIZE (1u << HASH_BITS)
typedef struct { uint32_t key, count; } Bucket;   /* key = eip >> 4 */
static Bucket g_hash[HASH_SIZE];
static uint32_t g_hash_used, g_hash_dropped;

static void hash_hit(uint32_t eip)
{
    uint32_t key = eip >> 4;
    uint32_t h = (key * 2654435761u) >> (32 - HASH_BITS);
    for (int probe = 0; probe < 32; probe++) {
        Bucket *b = &g_hash[(h + probe) & (HASH_SIZE - 1)];
        if (b->key == key) { b->count++; return; }
        if (!b->key) {
            if (g_hash_used >= HASH_SIZE - (HASH_SIZE >> 3)) break;
            b->key = key;
            b->count = 1;
            g_hash_used++;
            return;
        }
    }
    g_hash_dropped++;
}

/* ---- threads ---- */
#define MAX_THREADS 64
typedef struct {
    DWORD tid;
    HANDLE h;
    uint32_t samples;
    uint64_t cpu_last;      /* kernel+user 100ns at previous dump */
    uint64_t cpu_delta;
    uint64_t cpu_ref;       /* at previous refresh, for the idle filter */
    int active;             /* consumed cpu during the last refresh window */
    int seen;
} Thr;

static uint64_t thread_cpu(HANDLE h)
{
    FILETIME cr, ex, kt, ut;
    if (!GetThreadTimes(h, &cr, &ex, &kt, &ut)) return (uint64_t)-1;
    return ((uint64_t)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
         + ((uint64_t)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
}
static Thr g_thr[MAX_THREADS];
static int g_n_thr;
static DWORD g_self_tid;

/* The idle filter only engages once GetThreadTimes has proven to move at
 * all: under Wine it can succeed while reporting static/zero cpu times,
 * which would otherwise mark every thread idle and stop sampling. */
static int g_cpu_works;

static void refresh_threads(void)
{
    for (int i = 0; i < g_n_thr; i++) g_thr[i].seen = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_self_tid)
                continue;
            int found = 0;
            for (int i = 0; i < g_n_thr; i++)
                if (g_thr[i].tid == te.th32ThreadID)
                    { g_thr[i].seen = 1; found = 1; break; }
            if (found) continue;
            int free_i = -1;
            for (int i = 0; i < g_n_thr; i++)
                if (!g_thr[i].tid) { free_i = i; break; }
            if (free_i < 0) {
                if (g_n_thr >= MAX_THREADS) continue;
                free_i = g_n_thr++;
            }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            Thr *t = &g_thr[free_i];
            memset(t, 0, sizeof(*t));
            t->tid = te.th32ThreadID;
            t->h = h;
            t->seen = 2;     /* new this refresh: sample the first window */
            t->active = 1;
            t->cpu_ref = thread_cpu(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    for (int i = 0; i < g_n_thr; i++) {
        Thr *t = &g_thr[i];
        if (!t->tid) continue;
        if (!t->seen) {
            CloseHandle(t->h);
            t->tid = 0;
            t->h = NULL;
        } else if (t->seen == 1) {
            /* skip threads that consumed no cpu over the last window;
             * fully idle wine service threads would otherwise flood the
             * histogram with ntdll wait samples. (uint64_t)-1 means
             * GetThreadTimes failed — fail open and keep sampling. */
            uint64_t cpu = thread_cpu(t->h);
            t->active = cpu == (uint64_t)-1 || cpu != t->cpu_ref;
            if (cpu != (uint64_t)-1 && cpu != t->cpu_ref)
                g_cpu_works = 1;
            t->cpu_ref = cpu;
        }
    }
}

/* ---- sampling ---- */
static uint32_t g_total, g_failed;

static void record(uint32_t eip)
{
    g_total++;
    for (int i = 0; i < g_n_patches; i++) {
        XPatch *p = &g_patches[i];
        if (!p->blob_base) continue;
        uint32_t off = eip - p->blob_base;
        if (off < p->blob_total) {
            g_slots[p->slot].count++;
            int fi = patch_func_at(p, off);
            if (fi >= 0) p->fcount[fi]++;
            else p->other++;
            return;
        }
    }
    g_slots[slot_find(eip)].count++;
    hash_hit(eip);
}

static void sample_all(void)
{
    for (int i = 0; i < g_n_thr; i++) {
        Thr *t = &g_thr[i];
        if (!t->tid || (g_cpu_works && !t->active)) continue;
        if (SuspendThread(t->h) == (DWORD)-1) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(t->h, &c);
        ResumeThread(t->h);
        /* nothing above allocates, locks or logs: the suspended thread may
         * hold the loader or heap lock */
        if (!ok) { g_failed++; continue; }
        t->samples++;
        record(c.Eip);
    }
}

/* ---- reporting ---- */
static void resolve(uint32_t addr, char *out, size_t sz)
{
    int s = slot_find(addr);
    if (s)
        snprintf(out, sz, "%s+0x%x", g_slots[s].name, addr - g_slots[s].base);
    else
        snprintf(out, sz, "0x%08x", addr);
}

typedef struct { uint32_t count, key; } Top;
static Top g_top[HASH_SIZE];

static int cmp_top(const void *a, const void *b)
{
    uint32_t x = ((const Top *)a)->count, y = ((const Top *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_slotidx(const void *a, const void *b)
{
    uint32_t x = g_slots[*(const uint32_t *)a].count;
    uint32_t y = g_slots[*(const uint32_t *)b].count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void dump(uint32_t elapsed_ms)
{
    logf_("");
    int n_live = 0, n_active = 0;
    for (int i = 0; i < g_n_thr; i++)
        if (g_thr[i].tid) { n_live++; n_active += g_thr[i].active; }
    logf_("==== t=%lus  samples=%lu (%lu/s)  failed=%lu  buckets=%lu  "
          "threads=%d/%d %s%s ====",
          (unsigned long)(elapsed_ms / 1000), (unsigned long)g_total,
          (unsigned long)(g_total * 1000ull / (elapsed_ms ? elapsed_ms : 1)),
          (unsigned long)g_failed, (unsigned long)g_hash_used,
          n_active, n_live,
          g_cpu_works ? "idle-filter" : "no-filter",
          g_hash_dropped ? " (hash full!)" : "");
    if (!g_total) return;

    /* threads with any samples, plus cpu-time delta since last dump */
    for (int i = 0; i < g_n_thr; i++) {
        Thr *t = &g_thr[i];
        if (!t->tid || !t->samples) continue;
        FILETIME cr, ex, kt, ut;
        if (GetThreadTimes(t->h, &cr, &ex, &kt, &ut)) {
            uint64_t cpu = ((uint64_t)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
                         + ((uint64_t)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
            t->cpu_delta = cpu - t->cpu_last;
            t->cpu_last = cpu;
        }
        logf_("thread %5lu: %8lu samples  cpu+%.2fs",
              (unsigned long)t->tid, (unsigned long)t->samples,
              (double)t->cpu_delta / 1e7);
    }

    /* per-slot table (modules + translated blobs + anon) */
    uint32_t idx[MAX_SLOTS], n = 0;
    strcpy(g_slots[0].name, "<anon exec/other>");
    for (uint32_t i = 0; i < (uint32_t)g_n_slots; i++)
        if (g_slots[i].count) idx[n++] = i;
    qsort(idx, n, 4, cmp_slotidx);
    logf_("-- by module --");
    for (uint32_t i = 0; i < n; i++) {
        Slot *s = &g_slots[idx[i]];
        logf_("%-28s %8lu  %5.1f%%  (+%lu)", s->name, (unsigned long)s->count,
              100.0 * s->count / g_total, (unsigned long)(s->count - s->last));
        s->last = s->count;
    }

    /* hottest untranslated 16-byte buckets */
    uint32_t nt = 0;
    for (uint32_t i = 0; i < HASH_SIZE; i++)
        if (g_hash[i].count) {
            g_top[nt].count = g_hash[i].count;
            g_top[nt].key = g_hash[i].key;
            nt++;
        }
    qsort(g_top, nt, sizeof(Top), cmp_top);
    logf_("-- top buckets (16B, module code only) --");
    for (uint32_t i = 0; i < nt && i < 40; i++) {
        char where[96];
        resolve(g_top[i].key << 4, where, sizeof(where));
        logf_("%8lu  %5.2f%%  %s", (unsigned long)g_top[i].count,
              100.0 * g_top[i].count / g_total, where);
    }

    /* hottest translated functions, by original rva */
    for (int pi = 0; pi < g_n_patches; pi++) {
        XPatch *p = &g_patches[pi];
        if (!p->blob_base || !g_slots[p->slot].count) continue;
        uint32_t nf = 0;
        for (uint32_t i = 0; i < p->n_funcs; i++)
            if (p->fcount[i]) {
                g_top[nf].count = p->fcount[i];
                g_top[nf].key = p->funcs[i].rva;
                nf++;
            }
        qsort(g_top, nf, sizeof(Top), cmp_top);
        logf_("-- top translated funcs (%s, orig rva) --", p->module);
        for (uint32_t i = 0; i < nf && i < 20; i++)
            logf_("%8lu  %5.2f%%  %s+0x%x", (unsigned long)g_top[i].count,
                  100.0 * g_top[i].count / g_total, p->module,
                  (unsigned)g_top[i].key);
        if (p->other)
            logf_("%8lu           %s blob (between funcs)",
                  (unsigned long)p->other, p->module);
    }
}

static DWORD WINAPI sampler_thread(LPVOID arg)
{
    (void)arg;
    g_self_tid = GetCurrentThreadId();
    Sleep(3000);   /* let the game reach its loop and patches apply */
    DWORD start = GetTickCount(), last_refresh = start, last_dump = start;
    refresh_mods();
    refresh_threads();
    for (;;) {
        DWORD now = GetTickCount();
        if (now - last_refresh >= REFRESH_MS) {
            last_refresh = now;
            refresh_mods();
            refresh_threads();
            for (int i = 0; i < g_n_patches; i++)
                if (!g_patches[i].blob_base && !g_patches[i].dead)
                    try_activate_patch(&g_patches[i]);
        }
        sample_all();
        if (now - last_dump >= DUMP_MS) {
            last_dump = now;
            dump(now - start);
        }
        Sleep(SAMPLE_MS);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char dir[MAX_PATH];
        GetModuleFileNameA(inst, dir, sizeof(dir));
        char *sl = strrchr(dir, '\\');
        if (sl) *sl = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\HC47Profile.log", dir);
        g_log = fopen(path, "w");
        logf_("HC47 EIP profiler loaded (%d ms period)", SAMPLE_MS);
        g_n_slots = 1;   /* slot 0 = anon */
        load_patch_files(dir);
        CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    }
    return TRUE;
}
