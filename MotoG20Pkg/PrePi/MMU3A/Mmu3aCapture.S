.section .text.MotoG20Mmu3aCapture, "ax"
.align 4

.global MotoG20Mmu3aCapture
.global MotoG20Mmu3aPcAnchor
.type MotoG20Mmu3aCapture, %function

// X0 = SP output, X1 = SPSel output, X2 = PC-anchor output.
// No stack adjustment and no state-changing system instruction.
MotoG20Mmu3aCapture:
    mov     x3, sp
    mrs     x4, SPSel
    adr     x5, MotoG20Mmu3aPcAnchor
    str     x3, [x0]
    str     x4, [x1]
    str     x5, [x2]
MotoG20Mmu3aPcAnchor:
    ret

.size MotoG20Mmu3aCapture, .-MotoG20Mmu3aCapture
