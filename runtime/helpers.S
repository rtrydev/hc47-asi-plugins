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

.macro HLP1 name, insn
    .globl _hlp_\name
_hlp_\name:
    fld     qword ptr [esp+4]
    \insn
    fstp    qword ptr [esp+4]
    ret
.endm

HLP1 sin,    fsin
HLP1 cos,    fcos
HLP1 2xm1,   f2xm1
HLP1 rndint, frndint

    .globl _hlp_tan
_hlp_tan:                       /* tan(x); the pushed 1.0 is caller's job */
    fld     qword ptr [esp+4]
    fptan                       /* st0=1.0, st1=tan */
    fstp    st(0)
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_sincos
_hlp_sincos:                    /* in: x at [esp+4]; out: sin@[esp+4], cos@[esp+12] */
    fld     qword ptr [esp+4]
    fsincos                     /* st0=cos, st1=sin */
    fstp    qword ptr [esp+12]
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_atan2
_hlp_atan2:                     /* y=[esp+4] (st1), x=[esp+12] (st0) -> [esp+4] */
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fpatan                      /* st1 = atan2(st1, st0), pop */
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_yl2x
_hlp_yl2x:                      /* y=[esp+4], x=[esp+12] -> y*log2(x) */
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fyl2x
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_yl2xp1
_hlp_yl2xp1:
    fld     qword ptr [esp+4]
    fld     qword ptr [esp+12]
    fyl2xp1
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_scale
_hlp_scale:                     /* x=[esp+4] (st0), s=[esp+12] (st1) -> [esp+4] */
    fld     qword ptr [esp+12]  /* st1 = s */
    fld     qword ptr [esp+4]   /* st0 = x */
    fscale                      /* st0 = x * 2^trunc(s) */
    fstp    qword ptr [esp+4]
    fstp    st(0)
    ret

    .globl _hlp_i64tod
_hlp_i64tod:                    /* int64 at [esp+4] -> double in place */
    fild    qword ptr [esp+4]
    fstp    qword ptr [esp+4]
    ret

    .globl _hlp_dtoi64
_hlp_dtoi64:                    /* double at [esp+4] -> int64 in place (honors CW) */
    fld     qword ptr [esp+4]
    fistp   qword ptr [esp+4]
    ret
