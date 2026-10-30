; Hand-written stubs: version.dll export forwarders and the generic hook thunk.

EXTERN g_ver:QWORD
EXTERN HaiShouldRun:PROC
EXTERN HaiDbLoad:PROC

.code

; ---------------------------------------------------------------------------
; version.dll exports: each jumps through g_ver[idx], filled from the real
; System32\version.dll at load. Order must match kVersionExports in proxy.cpp.
; ---------------------------------------------------------------------------
PROXY MACRO name, idx
PUBLIC P_&name
P_&name PROC
    jmp qword ptr [g_ver + idx*8]
P_&name ENDP
ENDM

PROXY GetFileVersionInfoA, 0
PROXY GetFileVersionInfoByHandle, 1
PROXY GetFileVersionInfoExA, 2
PROXY GetFileVersionInfoExW, 3
PROXY GetFileVersionInfoSizeA, 4
PROXY GetFileVersionInfoSizeExA, 5
PROXY GetFileVersionInfoSizeExW, 6
PROXY GetFileVersionInfoSizeW, 7
PROXY GetFileVersionInfoW, 8
PROXY VerFindFileA, 9
PROXY VerFindFileW, 10
PROXY VerInstallFileA, 11
PROXY VerInstallFileW, 12
PROXY VerLanguageNameA, 13
PROXY VerLanguageNameW, 14
PROXY VerQueryValueA, 15
PROXY VerQueryValueW, 16

; ---------------------------------------------------------------------------
; HaiThunk: shared body for every patched AI-module vtable slot.
;   Entered from a per-hook stub with r10 = HookCtx* (first field = original fn).
;   Preserves the 4 register args (int and float) across HaiShouldRun(self, ctx, arg2),
;   then either tail-jumps to the original or returns 0 without running it.
; Entry: rsp = 8 mod 16. sub 88h makes it 0 mod 16 for the call.
; ---------------------------------------------------------------------------
PUBLIC HaiThunk
HaiThunk PROC
    sub     rsp, 88h
    mov     [rsp+20h], rcx
    mov     [rsp+28h], rdx
    mov     [rsp+30h], r8
    mov     [rsp+38h], r9
    mov     [rsp+40h], r10
    movups  xmmword ptr [rsp+48h], xmm0
    movups  xmmword ptr [rsp+58h], xmm1
    movups  xmmword ptr [rsp+68h], xmm2
    movups  xmmword ptr [rsp+78h], xmm3

    mov     r8, rdx                 ; HaiShouldRun(self = rcx, ctx = r10, arg2 = rdx)
    mov     rdx, r10
    call    HaiShouldRun
    movzx   r11d, al

    mov     rcx, [rsp+20h]
    mov     rdx, [rsp+28h]
    mov     r8,  [rsp+30h]
    mov     r9,  [rsp+38h]
    mov     r10, [rsp+40h]
    movups  xmm0, xmmword ptr [rsp+48h]
    movups  xmm1, xmmword ptr [rsp+58h]
    movups  xmm2, xmmword ptr [rsp+68h]
    movups  xmm3, xmmword ptr [rsp+78h]
    add     rsp, 88h

    test    r11d, r11d
    jz      skip
    jmp     qword ptr [r10]         ; run the game's original update
skip:
    xor     eax, eax
    ret
HaiThunk ENDP

; ---------------------------------------------------------------------------
; HaiDbThunk: AI database loader hook. Entered from a stub with r10 = DbHook*;
; LoadFiles(db = rcx, path = rdx) becomes HaiDbLoad(db, path, hook).
; ---------------------------------------------------------------------------
PUBLIC HaiDbThunk
HaiDbThunk PROC
    mov     r8, r10
    jmp     HaiDbLoad
HaiDbThunk ENDP

END
