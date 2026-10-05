.section .mmu3true.text, "ax"
.align 4

.global MotoG20EnableMmuEl2MOnly
.global MotoG20Mmu3PostEnableIsb
.global MotoG20Mmu3FirstPostEnableInstruction
.type MotoG20EnableMmuEl2MOnly, %function
.type MotoG20Mmu3PostEnableIsb, %function
.type MotoG20Mmu3FirstPostEnableInstruction, %function
.extern MotoG20Mmu3TrueCaptureBlock

// x17 is the only state pointer.  This routine is deliberately stackless:
// no BL, no SP writes, and no stack loads/stores from entry through return.
.macro LOAD64 reg, value
    movz    \reg, #((\value) & 0xffff)
    movk    \reg, #(((\value) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\value) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\value) >> 48) & 0xffff), lsl #48
.endm

.macro LOAD32 reg, value
    movz    \reg, #((\value) & 0xffff)
    .if (((\value) >> 16) & 0xffff)
    movk    \reg, #(((\value) >> 16) & 0xffff), lsl #16
    .endif
.endm

MotoG20EnableMmuEl2MOnly:
    adrp    x17, MotoG20Mmu3TrueCaptureBlock
    add     x17, x17, :lo12:MotoG20Mmu3TrueCaptureBlock

    mrs     x0, CurrentEL
    str     x0, [x17, #16]
    cmp     x0, #8
    b.ne    .Ltrue_fail_pre

    mrs     x0, HCR_EL2
    str     x0, [x17, #24]
    LOAD32  x1, 0x2
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, ID_AA64MMFR0_EL1
    str     x0, [x17, #32]
    LOAD32  x1, 0x101122
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, SCTLR_EL2
    str     x0, [x17, #40]
    LOAD32  x1, 0x30C50830
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, TCR_EL2
    str     x0, [x17, #48]
    LOAD32  x1, 0x80823518
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, TTBR0_EL2
    str     x0, [x17, #56]
    LOAD32  x1, 0x81981000
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, MAIR_EL2
    str     x0, [x17, #64]
    LOAD32  x1, 0xFFBB4400
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    mrs     x0, VBAR_EL2
    str     x0, [x17, #72]
    LOAD32  x1, 0x81004000
    cmp     x0, x1
    b.ne    .Ltrue_fail_pre

    LOAD64  x0, 0x54525545334D5052       // "TRUE3MPR"
    str     x0, [x17, #80]              // PRE_ENABLE_COOKIE
    mov     x0, #1
    str     x0, [x17, #8]               // PHASE: pre-enable complete

    // STRIPE 3: direct pre-MSR magenta checkpoint, 48x8 pixels.
    movz    x2, #0x8C40
    movk    x2, #0x9e23, lsl #16         // FB x656,y808
    movz    w3, #0x00ff, lsl #16
    movk    w3, #0x00ff
    mov     x4, #8
.Ltrue_pre_msr_row:
    mov     x6, #0
.Ltrue_pre_msr_pixel:
    str     w3, [x2, x6]
    add     x6, x6, #4
    cmp     x6, #192
    b.lo    .Ltrue_pre_msr_pixel
    add     x2, x2, #0xB40
    subs    x4, x4, #1
    b.ne    .Ltrue_pre_msr_row
    dsb     sy

    // Preload the first post-MSR store operands and do not clobber them
    // before MSR/ISB.  Stripe 4 starts at x656,y816 and is cyan.
    movz    x2, #0xE640
    movk    x2, #0x9E23, lsl #16
    movz    w3, #0xFFFF
    mov     x4, #7

    dsb     sy
    tlbi    alle2
    dsb     sy
    isb

    LOAD32  x5, 0x30C50831
    msr     SCTLR_EL2, x5
    isb

MotoG20Mmu3PostEnableIsb:
MotoG20Mmu3FirstPostEnableInstruction:
    stp     w3, w3, [x2]                 // FIRST POST-MSR INSTRUCTION/STORE
    dsb     sy
.Ltrue_stripe4_first_row:
    stp     w3, w3, [x2, #8]!
    subs    x4, x4, #1
    b.ne    .Ltrue_stripe4_first_row
    add     x2, x2, #0xB08
    mov     x5, #3
.Ltrue_stripe4_next_row:
    mov     x4, #8
.Ltrue_stripe4_next_pair:
    stp     w3, w3, [x2], #8
    subs    x4, x4, #1
    b.ne    .Ltrue_stripe4_next_pair
    add     x2, x2, #0xB00
    subs    x5, x5, #1
    b.ne    .Ltrue_stripe4_next_row
    dsb     sy

    // Stripe 4 is now photographically complete; continue post-M validation.
    mrs     x4, SCTLR_EL2
    str     x4, [x17, #96]
    LOAD32  x5, 0x30C50831
    cmp     x4, x5
    b.ne    .Ltrue_fail_post
    tst     x4, #1
    b.eq    .Ltrue_fail_post
    mov     x5, #0x1004
    tst     x4, x5
    b.ne    .Ltrue_fail_post

    LOAD64  x0, 0x54525545334D5054       // "TRUE3MPT"
    str     x0, [x17, #88]              // FIRST_POST_ENABLE_COOKIE
    mov     x0, #2
    str     x0, [x17, #8]               // PHASE: first post-enable passed

    // STRIPE 5: first successful capture validation has completed.
    movz    x2, #0x4040
    movk    x2, #0x9E24, lsl #16         // FB x656,y824
    movz    w3, #0x80FF
    mov     x4, #8
.Ltrue_m1_store_row:
    mov     x6, #0
.Ltrue_m1_store_pixel:
    str     w3, [x2, x6]
    add     x6, x6, #4
    cmp     x6, #192
    b.lo    .Ltrue_m1_store_pixel
    add     x2, x2, #0xB40
    subs    x4, x4, #1
    b.ne    .Ltrue_m1_store_row
    dsb     sy

    mrs     x0, TCR_EL2
    str     x0, [x17, #104]
    LOAD32  x1, 0x80823518
    cmp     x0, x1
    b.ne    .Ltrue_fail_post

    mrs     x0, TTBR0_EL2
    str     x0, [x17, #112]
    LOAD32  x1, 0x81981000
    cmp     x0, x1
    b.ne    .Ltrue_fail_post

    mrs     x0, MAIR_EL2
    str     x0, [x17, #120]
    LOAD32  x1, 0xFFBB4400
    cmp     x0, x1
    b.ne    .Ltrue_fail_post

    mrs     x0, VBAR_EL2
    str     x0, [x17, #128]
    LOAD32  x1, 0x81004000
    cmp     x0, x1
    b.ne    .Ltrue_fail_post

    LOAD64  x0, 0x5452554533444154       // "TRUE3DAT"
    str     x0, [x17, #136]             // POST_ENABLE_DATA_COOKIE
    mov     x0, #3
    str     x0, [x17, #8]               // phase: assembly success
    mov     x0, #1
    ret

.Ltrue_fail_pre:
    LOAD32  x0, 0xE3000001
    str     x0, [x17, #168]
    mov     x0, #0
    str     x0, [x17, #8]
.Ltrue_fail_pre_spin:
    wfe
    b       .Ltrue_fail_pre_spin

.Ltrue_fail_post:
    LOAD32  x0, 0xE3000002
    str     x0, [x17, #168]
    mov     x0, #0
    str     x0, [x17, #8]
.Ltrue_fail_post_spin:
    wfe
    b       .Ltrue_fail_post_spin

.size MotoG20EnableMmuEl2MOnly, .-MotoG20EnableMmuEl2MOnly
