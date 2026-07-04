/* HC47 window-mode diagnostic (32-bit ASI, diagnostic tool — not part of
 * `all`). Plants one-shot INT3 probes on the RenderD3D.dll D3D-environment
 * init path (device create, cooperative level, surfaces, viewport) and
 * logs which step fails with what HRESULT via a vectored exception
 * handler. Used to pinpoint why windowed mode dies at desktop-sized
 * resolutions on modern Windows.
 *
 * Output: scripts/HC47WinModeDiag.log
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define RD3D_TIMESTAMP   0x3a3e1338
#define RD3D_SIZEOFIMAGE 0x4d000

static const struct probe {
    uint32_t rva;
    const char *what;
} PROBES[] = {
    { 0x1cc1, "mode-select: loop setup" },
    { 0x1ddb, "mode-select: NO 32bpp mode (16bit-colors error)" },
    { 0x1e07, "mode-select: NO 16bpp mode" },
    { 0x1e55, "mode-select: mode chosen (ecx=si)" },
    { 0x1ee6, "init FAILED -> 'Unable to initialize Direct3D' (edi=hr)" },
    { 0x1f07, "init OK" },
    { 0x6b70, "enter create-ddraw/coop" },
    { 0x6b93, "DirectDrawCreateEx FAILED (eax=hr)" },
    { 0x6bfe, "SetCooperativeLevel FAILED (eax=hr)" },
    { 0x6c70, "enter fullscreen SetDisplayMode path" },
    { 0x6cf8, "SetDisplayMode FAILED (eax=hr)" },
    { 0x7060, "enter windowed surface path" },
    { 0x70ff, "windowed: primary CreateSurface FAILED (edi=hr)" },
    { 0x7169, "windowed: CreateClipper FAILED (eax=hr)" },
    { 0x7218, "windowed: backbuffer CreateSurface FAILED (esi=hr)" },
    { 0x72a0, "enter d3d-device create" },
    { 0x72c2, "QueryInterface(IDirect3D7) FAILED (eax=hr)" },
    { 0x7313, "CreateDevice FAILED (eax=hr)" },
    { 0x7384, "SetViewport FAILED (eax=hr)" },
    { 0x73c0, "enter zbuffer/extras path" },
    { 0x29460, "win-create: write snapped res to si+0x19 (eax=w ecx=h)" },
    { 0x29873, "mode-set epilogue: si cur=req, dirty=1" },
    { 0x298d0, "set-current-mode(esp[4]=&mode)" },
};
#define N_PROBES (sizeof(PROBES)/sizeof(PROBES[0]))

static FILE *g_log;
static CRITICAL_SECTION g_lock;
static uint8_t *g_base;
static uint8_t g_orig[N_PROBES];
static volatile LONG g_armed[N_PROBES];

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_lock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *xp)
{
    if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !g_base)
        return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *addr = (uint8_t *)xp->ExceptionRecord->ExceptionAddress;
    for (unsigned i = 0; i < N_PROBES; i++) {
        if (addr != g_base + PROBES[i].rva) continue;
        if (!InterlockedExchange(&g_armed[i], 0))
            return EXCEPTION_CONTINUE_SEARCH;   /* already disarmed */
        CONTEXT *c = xp->ContextRecord;
        logf_("t=%lu [%05x] %s", (unsigned long)GetTickCount(),
              PROBES[i].rva, PROBES[i].what);
        logf_("        eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx "
              "edi=%08lx esp=%08lx",
              c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Esp);
        DWORD old;
        VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old);
        *addr = g_orig[i];
        VirtualProtect(addr, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), addr, 1);
        c->Eip = (DWORD)(uintptr_t)addr;    /* re-run the original byte */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm(HMODULE m)
{
    uint8_t *base = (uint8_t *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != RD3D_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != RD3D_SIZEOFIMAGE) {
        logf_("RenderD3D.dll build mismatch (stamp %08lx) — probes skipped",
              (unsigned long)nt->FileHeader.TimeDateStamp);
        return;
    }
    g_base = base;
    for (unsigned i = 0; i < N_PROBES; i++) {
        uint8_t *site = base + PROBES[i].rva;
        DWORD old;
        if (!VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
        g_orig[i] = *site;
        g_armed[i] = 1;
        *site = 0xCC;
        VirtualProtect(site, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 1);
    }
    logf_("armed %u probes at RenderD3D base %p", (unsigned)N_PROBES, base);
}

/* ntdll loader notification fires before the loaded DLL's DllMain, so
 * probes are in place before RenderD3D initializes (same mechanism as
 * hc47x87.c); a polling thread is the fallback. */
typedef VOID (CALLBACK *LDR_NOTIFY_FN)(ULONG reason, const void *data, void *ctx);
typedef LONG (NTAPI *LdrRegisterDllNotification_t)(ULONG, LDR_NOTIFY_FN, void *, void **);

static VOID CALLBACK on_dll_notify(ULONG reason, const void *data, void *ctx)
{
    (void)data; (void)ctx;
    if (reason != 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */) return;
    static LONG dlc_seen;
    if (GetModuleHandleA("HitmanDlc.dlc") &&
        !InterlockedExchange(&dlc_seen, 1))
        logf_("t=%lu HitmanDlc.dlc loaded", (unsigned long)GetTickCount());
    if (!g_base) {
        HMODULE m = GetModuleHandleA("RenderD3D.dll");
        if (m) arm(m);
    }
}

static DWORD WINAPI wait_thread(LPVOID arg)
{
    (void)arg;
    for (int i = 0; i < 4800 && !g_base; i++) {
        HMODULE m = GetModuleHandleA("RenderD3D.dll");
        if (m) { arm(m); return 0; }
        Sleep(25);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, sizeof(path));
        char *sl = strrchr(path, '\\');
        if (sl) *sl = 0;
        strncat(path, "\\HC47WinModeDiag.log", sizeof(path) - strlen(path) - 1);
        g_log = fopen(path, "w");
        logf_("HC47 window-mode diag loaded");
        AddVectoredExceptionHandler(1, on_exception);
        LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)
            (uintptr_t)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                      "LdrRegisterDllNotification");
        void *cookie = NULL;
        if (!reg || reg(0, on_dll_notify, NULL, &cookie) != 0)
            CreateThread(NULL, 0, wait_thread, NULL, 0, NULL);
    }
    return TRUE;
}
