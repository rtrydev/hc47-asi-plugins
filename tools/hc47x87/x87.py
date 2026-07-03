"""x87 instruction taxonomy: stack effects and translatability."""

# Net stack-depth effect by mnemonic. Memory-operand size restrictions are
# checked separately (80-bit forms are rejected).
PUSH1 = {
    "fld", "fld1", "fldz", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2",
    "fild", "fptan", "fsincos",
}
POP1 = {
    "fstp", "fistp", "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp",
    "fcomp", "ficomp", "fcomip", "fucomp", "fucomip",
    "fpatan", "fyl2x", "fyl2xp1",
}
POP2 = {"fcompp", "fucompp"}
NET0 = {
    "fst", "fist", "fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr",
    "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr",
    "fcom", "ficom", "fucom", "fucomi", "fcomi",
    "fabs", "fchs", "fsqrt", "frndint", "fxch", "ftst",
    "fscale", "f2xm1", "fsin", "fcos", "fnop",
    "fnstsw", "fstsw", "fnstcw", "fstcw", "fldcw", "wait", "fwait",
}
# fninit resets the stack (depth := 0); allowed.
RESET = {"fninit", "finit"}

X87_ALL = PUSH1 | POP1 | POP2 | NET0 | RESET

# x87 mnemonics we know about but do not translate -> reject function.
UNSUPPORTED = {
    "fbld", "fbstp", "fldenv", "fnstenv", "fsave", "fnsave", "frstor",
    "fincstp", "fdecstp", "ffree", "ffreep", "fxam", "fxtract",
    "fprem", "fprem1", "fisttp",
    "fcmovb", "fcmove", "fcmovbe", "fcmovu", "fcmovnb", "fcmovne",
    "fcmovnbe", "fcmovnu", "fxch4", "fxch7", "fstpnce",
    "fnclex", "fclex", "fdisi", "feni", "fsetpm",
}

# Flag-producing compares that map onto comisd/ucomisd semantics.
COMPARES = {
    "fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp",
    "ficom", "ficomp", "ftst", "fcomi", "fcomip", "fucomi", "fucomip",
}


def is_x87(mnem):
    return mnem in X87_ALL or mnem in UNSUPPORTED


def depth_effect(mnem):
    if mnem in PUSH1:
        return 1
    if mnem in POP1:
        return -1
    if mnem in POP2:
        return -2
    return 0
