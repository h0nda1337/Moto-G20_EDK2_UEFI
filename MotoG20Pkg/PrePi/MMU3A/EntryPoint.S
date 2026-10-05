.section .text._ModuleEntryPoint, "ax"
.align 4

.global _ModuleEntryPoint
.type _ModuleEntryPoint, %function

.extern MotoG20EarlyInit
.extern MotoG20Mmu3aVbarSpProbe

_ModuleEntryPoint:
    msr     daifset, #0xf
    isb
    mov     x19, x0
    mov     x20, sp
    movz    x9, #0x8120, lsl #16
    mov     sp, x9
    mov     x0, x19
    mov     x1, x20
    bl      MotoG20EarlyInit
    bl      MotoG20Mmu3aVbarSpProbe
1:
    wfe
    b       1b

.size _ModuleEntryPoint, .-_ModuleEntryPoint
