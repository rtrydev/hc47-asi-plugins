/* call_probe(fn, ecx, edx, args, nargs, out_edx, out_st0, st0_flag)
 * Calls fn with a fabricated context; returns eax; captures edx and, if the
 * x87 stack is non-empty afterwards, st0 (popped). Restores esp via ebp so
 * cdecl/stdcall mismatches don't matter.
 */
    .intel_syntax noprefix
    .text
    .globl _call_probe
_call_probe:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    mov     esi, [ebp+20]       /* args */
    mov     ecx, [ebp+24]       /* nargs */
1:  test    ecx, ecx
    jz      2f
    dec     ecx
    push    dword ptr [esi+ecx*4]
    jmp     1b
2:
    finit
    push    0x027F              /* double precision, round-to-nearest */
    fldcw   word ptr [esp]
    add     esp, 4
    mov     ecx, [ebp+12]
    mov     edx, [ebp+16]
    mov     eax, 0x11111111     /* deterministic: callee may not write eax */
    call    dword ptr [ebp+8]

    mov     ebx, [ebp+28]       /* out_edx */
    mov     [ebx], edx
    mov     ebx, [ebp+36]       /* st0_flag */
    mov     dword ptr [ebx], 0
    fxam
    push    eax
    fnstsw  ax
    and     eax, 0x4500
    cmp     eax, 0x4100         /* C3=1,C0=1: empty */
    pop     eax
    je      3f
    mov     dword ptr [ebx], 1
    mov     ebx, [ebp+32]       /* out_st0 */
    fstp    qword ptr [ebx]
3:
    finit
    lea     esp, [ebp-12]
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret
