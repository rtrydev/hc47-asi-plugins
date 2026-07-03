/* Differential tester: original x87 function vs SSE2 translation.
 *
 * Loads the game module with DONT_RESOLVE_DLL_REFERENCES (no DllMain, no
 * imports needed — we only call leaf functions), maps the translated blob
 * with fixups applied, then calls both versions of each leaf function with
 * identical randomized contexts and compares results.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint32_t version, preferred_base, timedatestamp, size_of_image,
             n_funcs, blob_total;
    char module[32];
} PatchHeader;
typedef struct { uint32_t rva, blob_off, blob_len, fixup_idx, n_fixups; } FuncRec;
typedef struct { uint32_t blob_off, arg, type; } FixupRec;
#pragma pack(pop)
enum { ABS32_MODULE, REL32_MODULE, REL32_HELPER, ABS32_DATA };

typedef struct {
    double zero, one, pi, l2e, l2t, lg2, ln2, _pad;
    uint64_t signmask[2], absmask[2];
    uint32_t shadow_cw, _pad2[3];
} DataArea;
__attribute__((aligned(16)))
static DataArea g_data = {
    0.0, 1.0, 3.14159265358979323846, 1.44269504088896340737,
    3.32192809488736234787, 0.30102999566398119521,
    0.69314718055994530942, 0.0,
    {0x8000000000000000ULL, 0}, {0x7FFFFFFFFFFFFFFFULL, ~0ULL},
    0x027F, {0, 0, 0},
};

extern void hlp_sin(void), hlp_cos(void), hlp_sincos(void), hlp_tan(void),
    hlp_atan2(void), hlp_yl2x(void), hlp_yl2xp1(void), hlp_2xm1(void),
    hlp_scale(void), hlp_rndint(void), hlp_i64tod(void), hlp_dtoi64(void);
static void *g_helpers[12];

/* probe.S */
extern uint32_t call_probe(void *fn, uint32_t ecx, uint32_t edx,
                           uint32_t *args, uint32_t nargs,
                           uint32_t *out_edx, double *out_st0,
                           uint32_t *st0_flag);

/* ---- crash guard ---- */
static jmp_buf g_jb;
static volatile uint32_t g_fault_code;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    g_fault_code = ep->ExceptionRecord->ExceptionCode;
    longjmp(g_jb, 1);
}

/* ---- deterministic prng ---- */
static uint32_t g_seed;
static uint32_t rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

#define SCRATCH_WORDS (16384)
static uint32_t g_scratch_init[SCRATCH_WORDS];
static uint32_t g_scratch[SCRATCH_WORDS];

static float rnd_float(void)
{
    /* mostly small floats, some ints-as-floats, occasional zero */
    uint32_t r = rnd() % 100;
    if (r < 10) return 0.0f;
    if (r < 30) return (float)((int)(rnd() % 2000) - 1000);
    float m = ((float)(rnd() & 0xFFFFFF) / 8388608.0f) * 2.0f - 1.0f;
    int e = (int)(rnd() % 12) - 6;
    return ldexpf(m, e);
}

static void fill_scratch(uint32_t seed)
{
    g_seed = seed;
    for (int i = 0; i < SCRATCH_WORDS; i++) {
        float f = rnd_float();
        memcpy(&g_scratch_init[i], &f, 4);
    }
}

typedef struct {
    uint32_t eax, edx, st0_flag, faulted, fault_code;
    double st0;
    uint32_t mem_hash_exact;
} Result;

static void run_one(void *fn, uint32_t seed, Result *res, uint32_t *mem_out)
{
    memcpy(g_scratch, g_scratch_init, sizeof(g_scratch));
    g_seed = seed ^ 0x9E3779B9u;
    uint32_t args[16];
    for (int i = 0; i < 16; i++) {
        /* alternate: pointer into scratch / float value / small int */
        uint32_t k = rnd() % 3;
        if (k == 0)
            args[i] = (uint32_t)(uintptr_t)&g_scratch[(rnd() % (SCRATCH_WORDS - 64))];
        else if (k == 1) {
            float f = rnd_float(); memcpy(&args[i], &f, 4);
        } else
            args[i] = rnd() % 64;
    }
    uint32_t ecx = (uint32_t)(uintptr_t)&g_scratch[rnd() % (SCRATCH_WORDS / 2)];
    uint32_t edx = (uint32_t)(uintptr_t)&g_scratch[rnd() % (SCRATCH_WORDS / 2)];

    memset(res, 0, sizeof(*res));
    if (setjmp(g_jb)) {
        res->faulted = 1;
        res->fault_code = g_fault_code;
        return;
    }
    res->eax = call_probe(fn, ecx, edx, args, 16,
                          &res->edx, &res->st0, &res->st0_flag);
    memcpy(mem_out, g_scratch, sizeof(g_scratch));
}

static int dbl_close(double a, double b)
{
    if (a == b) return 1;
    if (isnan(a) && isnan(b)) return 1;
    double d = fabs(a - b), m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return d <= 1e-6 * m + 1e-9;
}

static uint32_t memA[SCRATCH_WORDS], memB[SCRATCH_WORDS];

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: difftest <module.dll> <patch.x87> <leaf.txt> [maxfuncs]\n");
        return 2;
    }
    g_helpers[0] = hlp_sin;   g_helpers[1] = hlp_cos;
    g_helpers[2] = hlp_sincos; g_helpers[3] = hlp_tan;
    g_helpers[4] = hlp_atan2; g_helpers[5] = hlp_yl2x;
    g_helpers[6] = hlp_yl2xp1; g_helpers[7] = hlp_2xm1;
    g_helpers[8] = hlp_scale; g_helpers[9] = hlp_rndint;
    g_helpers[10] = hlp_i64tod; g_helpers[11] = hlp_dtoi64;

    HMODULE mod = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) { printf("FAIL: cannot load %s (%lu)\n", argv[1], GetLastError()); return 2; }
    uint8_t *base = (uint8_t *)((uintptr_t)mod & ~0xFFFu);
    printf("module at %p\n", (void *)base);

    FILE *pf = fopen(argv[2], "rb");
    if (!pf) { printf("FAIL: patch file\n"); return 2; }
    fseek(pf, 0, SEEK_END); long psz = ftell(pf); fseek(pf, 0, SEEK_SET);
    uint8_t *pbuf = malloc(psz);
    if (fread(pbuf, 1, psz, pf) != (size_t)psz) { printf("FAIL: read\n"); return 2; }
    fclose(pf);
    PatchHeader *h = (PatchHeader *)pbuf;
    FuncRec *funcs = (FuncRec *)(pbuf + sizeof(PatchHeader));
    uint32_t nfix = *(uint32_t *)(funcs + h->n_funcs);
    FixupRec *fixups = (FixupRec *)((uint8_t *)(funcs + h->n_funcs) + 4);
    uint8_t *blob_src = (uint8_t *)(fixups + nfix);

    uint8_t *blob = VirtualAlloc(NULL, h->blob_total, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
    memcpy(blob, blob_src, h->blob_total);
    int32_t delta = (int32_t)(base - (uint8_t *)h->preferred_base);
    for (uint32_t i = 0; i < nfix; i++) {
        uint8_t *site = blob + fixups[i].blob_off;
        uint32_t *p32 = (uint32_t *)site;
        switch (fixups[i].type) {
        case ABS32_MODULE: *p32 += (uint32_t)delta; break;
        case REL32_MODULE:
            *p32 = (fixups[i].arg + (uint32_t)delta) - (uint32_t)(uintptr_t)(site + 4);
            break;
        case REL32_HELPER:
            *p32 = (uint32_t)(uintptr_t)g_helpers[fixups[i].arg]
                   - (uint32_t)(uintptr_t)(site + 4);
            break;
        case ABS32_DATA:
            *p32 = (uint32_t)(uintptr_t)((uint8_t *)&g_data + fixups[i].arg);
            break;
        }
    }
    printf("blob mapped at %p, delta %+d, %lu funcs\n", blob, delta,
           (unsigned long)h->n_funcs);

    /* leaf manifest */
    FILE *lf = fopen(argv[3], "r");
    if (!lf) { printf("FAIL: leaf manifest\n"); return 2; }
    static uint32_t leaf[65536]; int nleaf = 0;
    char line[64];
    while (fgets(line, sizeof(line), lf) && nleaf < 65536)
        leaf[nleaf++] = (uint32_t)strtoul(line, NULL, 16);
    fclose(lf);
    int maxf = argc > 4 ? atoi(argv[4]) : nleaf;

    AddVectoredExceptionHandler(1, veh);

    int tested = 0, passed = 0, mism = 0, faults = 0, skipped = 0, ax_junk = 0;
    for (int i = 0; i < nleaf && tested < maxf; i++) {
        /* find func rec */
        FuncRec *fr = NULL;
        for (uint32_t j = 0; j < h->n_funcs; j++)
            if (funcs[j].rva == leaf[i]) { fr = &funcs[j]; break; }
        if (!fr) { skipped++; continue; }
        void *orig = base + fr->rva;
        void *xlat = blob + fr->blob_off;
        int func_ok = 1, func_ran = 0;
        for (int round = 0; round < 6 && func_ok; round++) {
            uint32_t seed = leaf[i] * 2654435761u + round;
            fill_scratch(seed);
            Result ra, rb;
            run_one(orig, seed, &ra, memA);
            run_one(xlat, seed, &rb, memB);
            if (ra.faulted || rb.faulted) {
                if (ra.faulted != rb.faulted) {
                    printf("MISMATCH %05x r%d: fault orig=%u(%08x) xlat=%u(%08x)\n",
                           leaf[i], round, ra.faulted, ra.fault_code,
                           rb.faulted, rb.fault_code);
                    func_ok = 0;
                }
                continue;  /* both faulted: inconclusive round */
            }
            func_ran = 1;
            /* eax: a trailing fnstsw leaves junk in AX that our lahf-based
             * translation reproduces only partially. Semantic compare bits
             * are C0(8), C2(10), C3(14); AL (exception flags), C1(9) and
             * TOP(11-13) are junk. Differences confined to junk bits are
             * warnings, not failures. */
            if (ra.eax != rb.eax) {
                uint32_t diff = ra.eax ^ rb.eax;
                if (diff & ~0x3AFFu) {
                    printf("MISMATCH %05x r%d: eax %08x vs %08x\n",
                           leaf[i], round, ra.eax, rb.eax);
                    func_ok = 0;
                } else {
                    ax_junk++;
                }
            }
            if (ra.edx != rb.edx) {
                printf("MISMATCH %05x r%d: edx %08x vs %08x\n",
                       leaf[i], round, ra.edx, rb.edx);
                func_ok = 0;
            }
            if (ra.st0_flag != rb.st0_flag ||
                (ra.st0_flag && !dbl_close(ra.st0, rb.st0))) {
                printf("MISMATCH %05x r%d: st0 [%d]%g vs [%d]%g\n",
                       leaf[i], round, ra.st0_flag, ra.st0,
                       rb.st0_flag, rb.st0);
                func_ok = 0;
            }
            /* memory: float-tolerant compare */
            for (int w = 0; w < SCRATCH_WORDS; w++) {
                if (memA[w] == memB[w]) continue;
                float fa, fb;
                memcpy(&fa, &memA[w], 4); memcpy(&fb, &memB[w], 4);
                if (dbl_close(fa, fb)) continue;
                printf("MISMATCH %05x r%d: mem[%d] %08x(%g) vs %08x(%g)\n",
                       leaf[i], round, w, memA[w], fa, memB[w], fb);
                func_ok = 0;
                break;
            }
        }
        tested++;
        if (!func_ran) { faults++; continue; }
        if (func_ok) passed++;
        else mism++;
    }
    printf("\n=== %d tested: %d passed, %d mismatched, %d all-fault, %d no-blob"
           " (%d fnstsw-junk rounds tolerated) ===\n",
           tested, passed, mism, faults, skipped, ax_junk);
    return mism ? 1 : 0;
}
