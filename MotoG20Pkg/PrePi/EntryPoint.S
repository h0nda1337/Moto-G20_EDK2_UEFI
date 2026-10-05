.section .text._ModuleEntryPoint, "ax"
.align 4

.global _ModuleEntryPoint
.type _ModuleEntryPoint, %function

.extern MotoG20EarlyInit
.extern MotoG20Mmu3EnableStub

_ModuleEntryPoint:

    //
    // U-Boot -> MotoG20Pkg:
    //
    //   X0        = FDT
    //   CurrentEL = EL2
    //   MMU       = OFF
    //   D-cache   = OFF
    //   I-cache   = OFF
    //

    msr     daifset, #0xf
    isb

    //
    // Preserve U-Boot handoff state.
    //
    mov     x19, x0
    mov     x20, sp

    //
    // Install our own 64 KiB early stack:
    //
    //   0x811F0000 - 0x81200000
    //                      ^ SP
    //
    movz    x9, #0x8120, lsl #16
    mov     sp, x9

    //
    // C ABI arguments:
    //
    // X0 = DTB
    // X1 = original U-Boot SP
    //
    mov     x0, x19
    mov     x1, x20

    bl      MotoG20EarlyInit

    // MMU3 owns the first SCTLR_EL2.M transition and never returns.
    bl      MotoG20Mmu3EnableStub

1:
    wfe
    b       1b

.size _ModuleEntryPoint, .-_ModuleEntryPoint
