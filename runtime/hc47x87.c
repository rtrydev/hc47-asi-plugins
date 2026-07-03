/* HC47 Reduced-Precision x87 — runtime patch loader (32-bit ASI).
 *
 * Loads .x87 patch files (produced by tools/translate.py) from the
 * "<asi dir>/HC47ReducedX87/" directory. When the matching game module is
 * loaded, allocates the translated SSE2 code, applies fixups and installs
 * a 5-byte jmp hook at every translated function entry.
 *
 * Rationale: under CrossOver/Rosetta 2 every x87 instruction is emulated
 * in software (80-bit), while SSE2 runs on hardware. The offline translator
 * rewrites x87 float code as SSE2 double code ("reduced precision", same
 * idea as FEX-Emu's X87ReducedPrecision).
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- patch file format (matches tools/translate.py) ---- */
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

typedef struct {
    uint32_t blob_off, arg, type;
} FixupRec;
#pragma pack(pop)

enum { ABS32_MODULE = 0, REL32_MODULE = 1, REL32_HELPER = 2, ABS32_DATA = 3,
       TEB_FP = 4, TEB_AH = 5, ABS32_BLOB = 6 };

/* TEB scratch: 3 consecutive TLS slots (8-byte FP shuttle + AH snapshot),
 * addressed as fs:[0xE10 + slot*4]. Thread-safe, no registers needed. */
static uint32_t g_teb_fp_disp, g_teb_ah_disp;

static int alloc_teb_scratch(void)
{
    char owned[64] = {0};
    DWORD slots[64]; int n = 0;
    for (int i = 0; i < 64; i++) {
        DWORD s = TlsAlloc();
        if (s == TLS_OUT_OF_INDEXES) break;
        slots[n++] = s;
        if (s < 64) owned[s] = 1;
    }
    int base = -1;
    for (int i = 0; i + 2 < 64; i++)
        if (owned[i] && owned[i + 1] && owned[i + 2]) { base = i; break; }
    for (int i = 0; i < n; i++)
        if (base < 0 || slots[i] < (DWORD)base || slots[i] > (DWORD)base + 2)
            TlsFree(slots[i]);
    if (base < 0) return 0;
    g_teb_fp_disp = 0xE10 + base * 4;
    g_teb_ah_disp = 0xE10 + (base + 2) * 4;
    return 1;
}

/* ---- constant/data area referenced by translated code (ABS32_DATA) ----
 * Layout must match tools/hc47x87/codegen.py. 16-byte aligned. */
typedef struct {
    double zero;        /* 0x00 */
    double one;         /* 0x08 */
    double pi;          /* 0x10 */
    double l2e;         /* 0x18 */
    double l2t;         /* 0x20 */
    double lg2;         /* 0x28 */
    double ln2;         /* 0x30 */
    double _pad;        /* 0x38 */
    uint64_t signmask[2]; /* 0x40 */
    uint64_t absmask[2];  /* 0x50 */
    uint32_t shadow_cw;   /* 0x60 */
    uint32_t _pad2[3];
} DataArea;

__attribute__((aligned(16)))
static DataArea g_data = {
    .zero = 0.0,
    .one = 1.0,
    .pi = 3.14159265358979323846,
    .l2e = 1.44269504088896340737,   /* log2(e)  */
    .l2t = 3.32192809488736234787,   /* log2(10) */
    .lg2 = 0.30102999566398119521,   /* log10(2) */
    .ln2 = 0.69314718055994530942,   /* ln(2)    */
    .signmask = { 0x8000000000000000ULL, 0 },
    .absmask  = { 0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL },
    .shadow_cw = 0x027F,             /* default: round-to-nearest */
};

/* helpers.S — order must match codegen helper ids */
extern void hlp_sin(void), hlp_cos(void), hlp_sincos(void), hlp_tan(void),
    hlp_atan2(void), hlp_yl2x(void), hlp_yl2xp1(void), hlp_2xm1(void),
    hlp_scale(void), hlp_rndint(void), hlp_i64tod(void), hlp_dtoi64(void);

static void *g_helpers[12];

/* ---- logging ---- */
static FILE *g_log;
static char g_dir[MAX_PATH];    /* directory with patch files */

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

/* ---- pending patches ---- */
#define MAX_PATCHES 16
typedef struct {
    char path[MAX_PATH];
    char module[32];
    int applied;
} Pending;
static Pending g_pending[MAX_PATCHES];
static int g_n_pending;
static CRITICAL_SECTION g_lock;

static int apply_patch(Pending *p, HMODULE mod)
{
    HANDLE f = CreateFileA(p->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        logf_("[%s] cannot open patch file", p->module);
        return 0;
    }
    DWORD size = GetFileSize(f, NULL), rd = 0;
    uint8_t *buf = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, size);
    ReadFile(f, buf, size, &rd, NULL);
    CloseHandle(f);
    if (rd != size) goto fail;

    PatchHeader *h = (PatchHeader *)buf;
    if (memcmp(h->magic, "HC47X87P", 8) || h->version != 1) {
        logf_("[%s] bad patch header", p->module);
        goto fail;
    }
    FuncRec *funcs = (FuncRec *)(buf + sizeof(PatchHeader));
    uint32_t n_fix_total = *(uint32_t *)(funcs + h->n_funcs);
    FixupRec *fixups = (FixupRec *)((uint8_t *)(funcs + h->n_funcs) + 4);
    uint8_t *blob_src = (uint8_t *)(fixups + n_fix_total);

    /* verify module identity */
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != h->timedatestamp ||
        nt->OptionalHeader.SizeOfImage != h->size_of_image) {
        logf_("[%s] version mismatch (stamp %08lx vs %08lx) — skipping",
              p->module, (unsigned long)nt->FileHeader.TimeDateStamp,
              (unsigned long)h->timedatestamp);
        goto fail;
    }

    uint8_t *blob = (uint8_t *)VirtualAlloc(NULL, h->blob_total,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blob) goto fail;
    memcpy(blob, blob_src, h->blob_total);

    int32_t delta = (int32_t)(base - (uint8_t *)h->preferred_base);
    for (uint32_t i = 0; i < n_fix_total; i++) {
        uint8_t *site = blob + fixups[i].blob_off;
        uint32_t *p32 = (uint32_t *)site;
        switch (fixups[i].type) {
        case ABS32_MODULE:
            *p32 += (uint32_t)delta;
            break;
        case REL32_MODULE:
            *p32 = (fixups[i].arg + (uint32_t)delta)
                   - (uint32_t)(uintptr_t)(site + 4);
            break;
        case REL32_HELPER:
            *p32 = (uint32_t)(uintptr_t)g_helpers[fixups[i].arg]
                   - (uint32_t)(uintptr_t)(site + 4);
            break;
        case ABS32_DATA:
            *p32 = (uint32_t)(uintptr_t)((uint8_t *)&g_data + fixups[i].arg);
            break;
        case TEB_FP:
            *p32 = g_teb_fp_disp;
            break;
        case TEB_AH:
            *p32 = g_teb_ah_disp;
            break;
        case ABS32_BLOB:
            *p32 = (uint32_t)(uintptr_t)(blob + fixups[i].arg);
            break;
        }
    }

    /* install entry hooks */
    unsigned installed = 0;
    for (uint32_t i = 0; i < h->n_funcs; i++) {
        uint8_t *entry = base + funcs[i].rva;
        uint8_t *target = blob + funcs[i].blob_off;
        DWORD old;
        if (!VirtualProtect(entry, 5, PAGE_EXECUTE_READWRITE, &old))
            continue;
        entry[0] = 0xE9;
        *(int32_t *)(entry + 1) =
            (int32_t)(target - (entry + 5));
        VirtualProtect(entry, 5, old, &old);
        installed++;
    }
    FlushInstructionCache(GetCurrentProcess(), base,
                          nt->OptionalHeader.SizeOfImage);
    FlushInstructionCache(GetCurrentProcess(), blob, h->blob_total);
    logf_("[%s] applied: %u/%lu hooks, blob %lu KB at %p (delta %+d)",
          p->module, installed, (unsigned long)h->n_funcs,
          (unsigned long)h->blob_total / 1024, blob, delta);
    HeapFree(GetProcessHeap(), 0, buf);
    return 1;
fail:
    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

static void check_pending(void)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_n_pending; i++) {
        if (g_pending[i].applied) continue;
        HMODULE m = GetModuleHandleA(g_pending[i].module);
        if (m) {
            g_pending[i].applied = 1;   /* one attempt only */
            apply_patch(&g_pending[i], m);
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* ---- ntdll loader notifications (fires before DllMain of the module) ---- */
typedef VOID (CALLBACK *LDR_NOTIFY_FN)(ULONG reason, const void *data, void *ctx);
typedef LONG (NTAPI *LdrRegisterDllNotification_t)(ULONG, LDR_NOTIFY_FN, void *, void **);

static VOID CALLBACK on_dll_notify(ULONG reason, const void *data_, void *ctx)
{
    (void)ctx;
    if (reason != 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */) return;
    check_pending();
}

static DWORD WINAPI poll_thread(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 1200; i++) {   /* up to 5 minutes */
        check_pending();
        int done = 1;
        for (int j = 0; j < g_n_pending; j++)
            if (!g_pending[j].applied) done = 0;
        if (done) break;
        Sleep(250);
    }
    return 0;
}

static void init(HMODULE self)
{
    InitializeCriticalSection(&g_lock);
    g_helpers[0] = (void *)hlp_sin;    g_helpers[1] = (void *)hlp_cos;
    g_helpers[2] = (void *)hlp_sincos; g_helpers[3] = (void *)hlp_tan;
    g_helpers[4] = (void *)hlp_atan2;  g_helpers[5] = (void *)hlp_yl2x;
    g_helpers[6] = (void *)hlp_yl2xp1; g_helpers[7] = (void *)hlp_2xm1;
    g_helpers[8] = (void *)hlp_scale;  g_helpers[9] = (void *)hlp_rndint;
    g_helpers[10] = (void *)hlp_i64tod; g_helpers[11] = (void *)hlp_dtoi64;

    GetModuleFileNameA(self, g_dir, sizeof(g_dir));
    char *sl = strrchr(g_dir, '\\');
    if (sl) *sl = 0;

    char logpath[MAX_PATH];
    snprintf(logpath, sizeof(logpath), "%s\\HC47ReducedX87.log", g_dir);
    g_log = fopen(logpath, "w");
    logf_("HC47 Reduced-Precision x87 loaded");

    /* initialize shadow CW from the real control word */
    {
        uint16_t cw = 0;
        __asm__ volatile ("fnstcw %0" : "=m"(cw));
        g_data.shadow_cw = cw;
    }

    /* enumerate patch files */
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\HC47ReducedX87\\*.x87", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (g_n_pending >= MAX_PATCHES) break;
            Pending *p = &g_pending[g_n_pending];
            snprintf(p->path, sizeof(p->path), "%s\\HC47ReducedX87\\%s",
                     g_dir, fd.cFileName);
            /* module name = header field */
            FILE *f = fopen(p->path, "rb");
            if (!f) continue;
            PatchHeader h;
            if (fread(&h, sizeof(h), 1, f) == 1 &&
                !memcmp(h.magic, "HC47X87P", 8)) {
                memcpy(p->module, h.module, 32);
                p->module[31] = 0;
                p->applied = 0;
                g_n_pending++;
                logf_("registered patch: %s (%s)", fd.cFileName, p->module);
            }
            fclose(f);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    if (!g_n_pending) {
        logf_("no patch files found in %s\\HC47ReducedX87", g_dir);
        return;
    }

    if (!alloc_teb_scratch()) {
        logf_("FATAL: no 3 consecutive TLS slots; not patching");
        g_n_pending = 0;
        return;
    }
    logf_("TEB scratch at fs:[0x%X]", g_teb_fp_disp);

    check_pending();  /* modules that are already loaded */

    LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)
        (uintptr_t)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                  "LdrRegisterDllNotification");
    void *cookie = NULL;
    if (reg && reg(0, on_dll_notify, NULL, &cookie) == 0) {
        logf_("using loader notifications");
    } else {
        logf_("loader notifications unavailable; polling");
        CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init((HMODULE)inst);
    }
    return TRUE;
}
