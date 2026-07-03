/* x87 helper stubs for rare transcendental / 64-bit-int operations.
 *
 * Convention (defined by tools/hc47x87/codegen.py):
 *   1-in/1-out : caller puts double at [esp+4] (after the call pushes ret),
 *                helper overwrites it in place.
 *   2-in/1-out : a at [esp+4], b at [esp+12]; result -> [esp+4].
 *
 * These intentionally execute the real x87 instruction: they are rare and
 * cold, and this keeps semantics bit-exact. All GP registers and EFLAGS
 * are preserved (x87 ops touch neither); the real FPU stack is empty in
 * translated code except for these transients.
 */
    .intel_syntax noprefix
    .text

#ifdef HC47_DIAG
/* Diagnostic build: count every helper call; when a double at [esp+off]
 * has an all-ones exponent (NaN/Inf), record (caller, tag) in a ring
 * buffer. tag = id for outputs, id+100 for already-bad inputs.
 * Preserves all registers and EFLAGS. */
.macro DIAGCOUNT id
    pushfd
    inc     dword ptr [_g_diag_calls + \id*4]
    fnstcw  [_g_cw_main]          /* capture the CALLING thread's CW */
    popfd
.endm
.macro DIAGCHK tag, resoff
    push    eax
    pushfd
    mov     eax, [esp + \resoff + 12]     /* high dword of the double */
    and     eax, 0x7FF00000
    cmp     eax, 0x7FF00000
    jne     9f
    /* NaN only: skip legitimate +/-Inf sentinels (mantissa == 0) */
    mov     eax, [esp + \resoff + 12]
    shl     eax, 12                       /* drop sign+exponent */
    or      eax, [esp + \resoff + 8]      /* low dword */
    jz      9f
    push    ebx
    push    ecx
    mov     eax, [_g_diag_widx]
    mov     ecx, eax
    and     ecx, 63
    shl     ecx, 3
    mov     ebx, [esp + 16]               /* caller return address */
    mov     [_g_diag_ring + ecx], ebx
    mov     dword ptr [_g_diag_ring + ecx + 4], \tag
    inc     eax
    mov     [_g_diag_widx], eax
    pop     ecx
    pop     ebx
9:  popfd
    pop     eax
.endm
.macro DIAGREC tag
    push    eax
    pushfd
    push    ebx
    push    ecx
    mov     eax, [_g_diag_widx]
    mov     ecx, eax
    and     ecx, 63
    shl     ecx, 3
    mov     ebx, [esp + 16]               /* caller return address */
    mov     [_g_diag_ring + ecx], ebx
    mov     dword ptr [_g_diag_ring + ecx + 4], \tag
    inc     eax
    mov     [_g_diag_widx], eax
    pop     ecx
    pop     ebx
    popfd
    pop     eax
.endm
#else
.macro DIAGCOUNT id
.endm
.macro DIAGCHK tag, resoff
.endm
.macro DIAGREC tag
.endm
#endif

.macro HLP1 name, insn, id
    .globl _hlp_\name
_hlp_\name:
    DIAGCOUNT \id
    DIAGCHK \id+100, 4
    fld     qword ptr [esp+4]
    \insn
    fstp    qword ptr [esp+4]
    DIAGCHK \id, 4
    ret
.endm

HLP1 sin,    fsin,    0
HLP1 cos,    fcos,    1
HLP1 2xm1,   f2xm1,   7
HLP1 rndint, frndint, 9

    .globl _hlp_tan
_hlp_tan:                       /* tan(x); the pushed 1.0 is caller's job */
    DIAGCOUNT 3
    DIAGCHK 103, 4
    fld     qword ptr [esp+4]
    fptan                       /* st0=1.0, st1=tan */
    fstp    st(0)
    fstp    qword ptr [esp+4]
    DIAGCHK 3, 4
    ret

    .globl _hlp_sincos
_hlp_sincos:                    /* in: x at [esp+4]; out: sin@[esp+4], cos@[esp+12] */
    DIAGCOUNT 2
    DIAGCHK 102, 4
    fld     qword ptr [esp+4]
    fsincos                     /* st0=cos, st1=sin */
    fstp    qword ptr [esp+12]
    fstp    qword ptr [esp+4]
    DIAGCHK 2, 4
    DIAGCHK 2, 12
    ret

    .globl _hlp_atan2
_hlp_atan2:                     /* y=[esp+4] (st1), x=[esp+12] (st0) -> [esp+4] */
    DIAGCOUNT 4
    DIAGCHK 104, 4
    DIAGCHK 104, 12
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fpatan                      /* st1 = atan2(st1, st0), pop */
    fstp    qword ptr [esp+4]
    DIAGCHK 4, 4
    ret

    .globl _hlp_yl2x
_hlp_yl2x:                      /* y=[esp+4], x=[esp+12] -> y*log2(x) */
    DIAGCOUNT 5
    DIAGCHK 105, 4
    DIAGCHK 105, 12
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fyl2x
    fstp    qword ptr [esp+4]
    DIAGCHK 5, 4
    ret

    .globl _hlp_yl2xp1
_hlp_yl2xp1:
    DIAGCOUNT 6
    DIAGCHK 106, 4
    DIAGCHK 106, 12
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fyl2xp1
    fstp    qword ptr [esp+4]
    DIAGCHK 6, 4
    ret

    .globl _hlp_scale
_hlp_scale:                     /* x=[esp+4] (st0), s=[esp+12] (st1) -> [esp+4] */
    DIAGCOUNT 8
    DIAGCHK 108, 4
    DIAGCHK 108, 12
    fld     qword ptr [esp+12]  /* st1 = s */
    fld     qword ptr [esp+4]   /* st0 = x */
    fscale                      /* st0 = x * 2^trunc(s) */
    fstp    qword ptr [esp+4]
    fstp    st(0)
    DIAGCHK 8, 4
    ret

    .globl _hlp_i64tod
_hlp_i64tod:                    /* int64 at [esp+4] -> double in place */
    DIAGCOUNT 10
    fild    qword ptr [esp+4]
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_emptypop
_hlp_emptypop:                  /* an unverified st0-return mark found an
                                   empty FPU stack at runtime (0.0 was
                                   substituted); record in diag builds */
    DIAGREC 13
    ret

    .globl _hlp_diagnan
_hlp_diagnan:                   /* diag-translated code: check [esp+4] for
                                   NaN/Inf at float-returning rets */
    DIAGCHK 12, 4
    ret

    .globl _hlp_dtoi64
_hlp_dtoi64:                    /* double at [esp+4] -> int64 in place (honors CW) */
    DIAGCOUNT 11
    DIAGCHK 111, 4
    fld     qword ptr [esp+4]
    fistp   qword ptr [esp+4]
    ret
