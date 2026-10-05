.section .mmu3.vector_table, "ax"
.align 12

.global MotoG20Mmu3b1VectorTableStart
.global MotoG20Mmu3b1VectorTableEnd
.global MotoG20Mmu3b1UnexpectedException
.global MotoG20Mmu3b2ControlledException
.global MotoG20Mmu3b2MsrElrInstruction
.global MotoG20Mmu3b2EretInstruction
.type MotoG20Mmu3b1VectorTableStart, %function

.macro VECTOR_SLOT id, target
    mov     x0, #\id
    b       \target
    .rept   30
    nop
    .endr
.endm

MotoG20Mmu3b1VectorTableStart:
    VECTOR_SLOT 0, MotoG20Mmu3b2ControlledException
    VECTOR_SLOT 1, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 2, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 3, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 4, MotoG20Mmu3b2ControlledException
    VECTOR_SLOT 5, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 6, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 7, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 8, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 9, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 10, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 11, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 12, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 13, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 14, MotoG20Mmu3b1UnexpectedException
    VECTOR_SLOT 15, MotoG20Mmu3b1UnexpectedException
MotoG20Mmu3b1VectorTableEnd:

.size MotoG20Mmu3b1VectorTableStart, .-MotoG20Mmu3b1VectorTableStart

.section .mmu3.unexpected_trampoline, "ax"
.align 4
.global MotoG20Mmu3b1UnexpectedException
.type MotoG20Mmu3b1UnexpectedException, %function
.extern MotoG20Mmu3b1CaptureBlock
.extern MotoG20Mmu3b1ExceptionStackTop
.extern MotoG20Mmu3c1UnexpectedHandler

// X0 is the architectural slot identifier supplied by the vector slot.
// This is an unexpected-exception fail-stop path; it never executes ERET.
MotoG20Mmu3b1UnexpectedException:
    msr     daifset, #0xf
    b       MotoG20Mmu3c1UnexpectedHandler
    .rept   22
    nop
    .endr

.size MotoG20Mmu3b1UnexpectedException, .-MotoG20Mmu3b1UnexpectedException

.section .mmu3.controlled_path, "ax"
.align 4
.type MotoG20Mmu3b2ControlledException, %function

// Controlled synchronous handler. X0 is vector ID 0 or 4.
// This path is deliberately stackless: no BL, no SP write, no renderer.
MotoG20Mmu3b2ControlledException:
    msr     daifset, #0xf
    adr     x10, MotoG20Mmu3b1CaptureBlock
    mov     x11, sp
    mov     x12, #0xB2E0
    str     x12, [x10, #8]
    str     x0, [x10, #16]
    str     x11, [x10, #96]
    movz    x12, #0xB2E1
    movk    x12, #0xB2E1, lsl #16
    str     x12, [x10, #128]
    mrs     x1, ESR_EL2
    mrs     x2, ELR_EL2
    mrs     x3, SPSR_EL2
    mrs     x4, FAR_EL2
    mrs     x5, VBAR_EL2
    mrs     x6, SCTLR_EL2
    mrs     x7, CurrentEL
    stp     x1, x2, [x10, #40]
    stp     x3, x4, [x10, #64]
    stp     x5, x6, [x10, #80]
    str     x7, [x10, #32]

    ldr     x12, [x10, #24]
    cmp     x12, #0
    b.eq    1f
    cmp     x12, #1
    b.ne    9f
    mov     x13, #4
    b       2f
1:
    mov     x13, #0
2:
    cmp     x0, x13
    b.ne    9f
    and     x13, x7, #0xc
    cmp     x13, #8
    b.ne    9f
    ldr     x13, [x10, #120]
    cmp     x1, x13
    b.ne    9f
    ldr     x13, [x10, #104]
    cmp     x2, x13
    b.ne    9f
    add     x14, x2, #4
    str     x14, [x10, #56]
    ldr     x13, [x10, #112]
    cmp     x14, x13
    b.ne    9f
    ldr     x13, [x10, #80]
    mov     x15, #0x4000
    movk    x15, #0x8100, lsl #16
    cmp     x13, x15
    b.ne    9f
    mov     x15, #0x1005
    and     x13, x6, x15
    cbnz    x13, 9f
    and     x13, x3, #0xf
    ldr     x12, [x10, #24]
    cmp     x12, #0
    mov     x15, #8
    mov     x12, #9
    csel    x15, x15, x12, eq
    cmp     x13, x15
    b.ne    9f
    mov     x12, #0xB2E2
    str     x12, [x10, #8]
    movz    x12, #0xB2E3
    movk    x12, #0xB2E3, lsl #16
    str     x12, [x10, #136]
MotoG20Mmu3b2MsrElrInstruction:
    msr     ELR_EL2, x14
MotoG20Mmu3b2EretInstruction:
    eret

9:
    mov     x12, #0xB2EF
    str     x12, [x10, #8]
10:
    wfe
    b       10b

.size MotoG20Mmu3b2ControlledException, .-MotoG20Mmu3b2ControlledException

.section .mmu3.controlled_path, "ax"
.align 4
.global MotoG20Mmu3b1WriteVbarReadback
.type MotoG20Mmu3b1WriteVbarReadback, %function

// X0 = validated local vector address; X1 = readback output.
// Returns W0=1 only for exact readback equality.
MotoG20Mmu3b1WriteVbarReadback:
    msr     VBAR_EL2, x0
    isb
    mrs     x2, VBAR_EL2
    str     x2, [x1]
    cmp     x2, x0
    cset    w0, eq
    ret

.size MotoG20Mmu3b1WriteVbarReadback, .-MotoG20Mmu3b1WriteVbarReadback

.section .mmu3.controlled_path, "ax"
.align 4
.global MotoG20Mmu3b2Trigger
.global MotoG20Mmu3b2BrkInstruction
.global MotoG20Mmu3b2PostBrk
.type MotoG20Mmu3b2Trigger, %function

MotoG20Mmu3b2Trigger:
MotoG20Mmu3b2BrkInstruction:
    brk     #0x03b2
MotoG20Mmu3b2PostBrk:
    ret

.size MotoG20Mmu3b2Trigger, .-MotoG20Mmu3b2Trigger

.section .data.MotoG20Mmu3b1TrueCapture, "aw"
.align 6
.global MotoG20Mmu3TrueCaptureBlock
.type MotoG20Mmu3TrueCaptureBlock, %object
MotoG20Mmu3TrueCaptureBlock:
    .zero 0xB0
.size MotoG20Mmu3TrueCaptureBlock, .-MotoG20Mmu3TrueCaptureBlock

.section .data.MotoG20Mmu3b1Capture, "aw"
.align 6
.global MotoG20Mmu3b1CaptureBlock
.type MotoG20Mmu3b1CaptureBlock, %object
MotoG20Mmu3b1CaptureBlock:
    .space 0x98, 0
.size MotoG20Mmu3b1CaptureBlock, .-MotoG20Mmu3b1CaptureBlock

.section .data.MotoG20Mmu3b1ExceptionStack, "aw"
.align 12
.global MotoG20Mmu3b1ExceptionStackBase
.global MotoG20Mmu3b1ExceptionStackTop
.type MotoG20Mmu3b1ExceptionStackBase, %object
MotoG20Mmu3b1ExceptionStackBase:
    .space 0x1000, 0
MotoG20Mmu3b1ExceptionStackTop:
.size MotoG20Mmu3b1ExceptionStackBase, .-MotoG20Mmu3b1ExceptionStackBase
