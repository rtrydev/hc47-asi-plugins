/* HC47 ASI Loader — minimal dsound.dll proxy that loads every .asi from
 * the scripts directory (32-bit, built with mingw like the plugins).
 *
 * Drop-in replacement for the third-party Ultimate ASI Loader binary,
 * scoped to exactly what this game needs:
 *
 *  - Placed as dsound.dll in the game directory. The game pulls it in
 *    through its sound stack (EAX.dll imports dsound ordinal 1, Sound.dll
 *    ordinal 2, a3d.dll by name). Under Wine/CrossOver this requires the
 *    DLL override "dsound=native,builtin" for the bottle (already the
 *    case for any setup where an ASI loader ever worked).
 *  - On process attach it loads every *.asi from the scripts/ directory
 *    next to it, and logs to scripts/hc47_asi_loader.log.
 *  - All 12 documented dsound.dll exports are provided at their real
 *    ordinals (see dsound.def) and forwarded to the system dsound.dll,
 *    resolved lazily on first call. Getting the ordinals right matters:
 *    EAX.dll/Sound.dll import by ordinal, and a proxy with a different
 *    export layout silently misbinds them.
 *  - If the system dsound.dll cannot be resolved (or resolves back to
 *    this module), the stubs return DSERR_NODRIVER and the game falls
 *    back to its no-sound path instead of crashing.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define DSERR_NODRIVER 0x88780078u

static FILE *g_log;
static char g_dir[MAX_PATH];        /* game directory (location of this dll) */
static HINSTANCE g_self;
static HMODULE g_real;              /* system dsound.dll, resolved lazily */

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

static FARPROC resolve(const char *name)
{
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 12);
        if (n == 0 || n >= MAX_PATH - 12) return NULL;
        strcat(path, "\\dsound.dll");
        HMODULE h = LoadLibraryA(path);
        if (!h || h == (HMODULE)g_self) {
            logf_("system dsound.dll unavailable (h=%p self=%p) — "
                  "sound stubs return DSERR_NODRIVER", h, g_self);
            g_real = (HMODULE)(uintptr_t)-1;
        } else {
            g_real = h;
            logf_("forwarding to system dsound.dll at %p", h);
        }
    }
    if (g_real == (HMODULE)(uintptr_t)-1) return NULL;
    FARPROC f = GetProcAddress(g_real, name);
    if (!f) logf_("system dsound.dll lacks %s", name);
    return f;
}

/* proxy exports; generic pointer-sized args keep this header-free.
 * All dsound entry points are stdcall and return HRESULT. */
#define PROXY2(name) \
    __declspec(dllexport) uint32_t WINAPI name(void *a, void *b) { \
        uint32_t (WINAPI *f)(void *, void *) = (void *)resolve(#name); \
        return f ? f(a, b) : DSERR_NODRIVER; }
#define PROXY3(name) \
    __declspec(dllexport) uint32_t WINAPI name(void *a, void *b, void *c) { \
        uint32_t (WINAPI *f)(void *, void *, void *) = (void *)resolve(#name); \
        return f ? f(a, b, c) : DSERR_NODRIVER; }

PROXY3(DirectSoundCreate)
PROXY2(DirectSoundEnumerateA)
PROXY2(DirectSoundEnumerateW)
PROXY3(DirectSoundCaptureCreate)
PROXY2(DirectSoundCaptureEnumerateA)
PROXY2(DirectSoundCaptureEnumerateW)
PROXY2(GetDeviceID)
PROXY3(DirectSoundCreate8)
PROXY3(DirectSoundCaptureCreate8)

/* the two COM entry points are declared in objbase.h — match it */
HRESULT WINAPI DllCanUnloadNow(void)
{
    HRESULT (WINAPI *f)(void) = (void *)resolve("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    HRESULT (WINAPI *f)(REFCLSID, REFIID, LPVOID *) =
        (void *)resolve("DllGetClassObject");
    return f ? f(rclsid, riid, ppv) : (HRESULT)0x80040111; /* CLASS_E_... */
}

__declspec(dllexport) uint32_t WINAPI DirectSoundFullDuplexCreate(
    void *a, void *b, void *c, void *d, void *e,
    void *f, void *g, void *h, void *i, void *j)
{
    uint32_t (WINAPI *fn)(void *, void *, void *, void *, void *,
                          void *, void *, void *, void *, void *) =
        (void *)resolve("DirectSoundFullDuplexCreate");
    return fn ? fn(a, b, c, d, e, f, g, h, i, j) : DSERR_NODRIVER;
}

static void load_asis(void)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\scripts\\*.asi", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("no ASI plugins found (%s)", pat);
        return;
    }
    int n = 0, fail = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\scripts\\%s", g_dir, fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        if (m) {
            n++;
            logf_("loaded %s at %p", fd.cFileName, m);
        } else {
            fail++;
            logf_("FAILED to load %s (error %lu)", fd.cFileName,
                  (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf_("%d plugin(s) loaded%s", n, fail ? ", some failed!" : "");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        char *sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\scripts\\hc47_asi_loader.log",
                 g_dir);
        g_log = fopen(logpath, "w");
        logf_("HC47 ASI Loader (dsound.dll proxy) attached");
        load_asis();
    }
    return TRUE;
}
