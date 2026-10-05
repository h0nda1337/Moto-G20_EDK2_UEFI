#if !defined(MOTO_G20_MMU3C2C_TELEMETRY)
#error "MMU3C2H unexpected handler requires telemetry and the X18 guard"
#endif

.section .mmu3c1.exception.text, "ax"
.align 4

.global MotoG20Mmu3c1UnexpectedHandler
.type MotoG20Mmu3c1UnexpectedHandler, %function

.extern MotoG20Mmu3b1ExceptionStackTop
.extern MotoG20Mmu3c2cPublishUnexpectedTelemetry

// X0 is the vector-slot identifier supplied by the vector multiplexer.
// X18 is reserved by every C translation unit. The recursion sentinel is
// tested and installed before the first framebuffer or capture memory access.
MotoG20Mmu3c1UnexpectedHandler:
    movz    x11, #0xc2c2
    movk    x11, #0x4d33, lsl #16       // 0x000000004D33C2C2
    cmp     x18, x11
    b.eq    23f
    mov     x18, x11

    movz    x10, #0x7dc0
    movk    x10, #0x9e22, lsl #16       // FB x624,y784
    mov     x12, #0x2d00                 // four framebuffer rows per slot
    madd    x10, x0, x12, x10
    cmp     x0, #1
    b.eq    1f
    cmp     x0, #2
    b.eq    2f
    cmp     x0, #3
    b.eq    3f
    cmp     x0, #5
    b.eq    5f
    cmp     x0, #6
    b.eq    6f
    cmp     x0, #7
    b.eq    7f
    cmp     x0, #8
    b.eq    8f
    cmp     x0, #9
    b.eq    9f
    cmp     x0, #10
    b.eq    10f
    cmp     x0, #11
    b.eq    11f
    cmp     x0, #12
    b.eq    12f
    cmp     x0, #13
    b.eq    13f
    cmp     x0, #14
    b.eq    14f
    cmp     x0, #15
    b.eq    15f
    movz    w11, #0xff00, lsl #16        // default red: slot 0 or 4
    b       20f
1:  movz    w11, #0x8000, lsl #16        // slot 1 orange-red
    movk    w11, #0x8000
    b       20f
2:  movz    w11, #0x8080, lsl #16        // slot 2 olive
    movk    w11, #0x8000
    b       20f
3:  movz    w11, #0x8000, lsl #16        // slot 3 dark red
    movk    w11, #0x00ff
    b       20f
5:  movz    w11, #0x8000, lsl #16        // slot 5 dark magenta
    movk    w11, #0x0080
    b       20f
6:  movz    w11, #0x0080, lsl #16        // slot 6 violet
    movk    w11, #0x00ff
    b       20f
7:  movz    w11, #0x00ff, lsl #16        // slot 7 green
    b       20f
8:  movz    w11, #0x0080, lsl #16        // slot 8 teal
    movk    w11, #0x0080
    b       20f
9:  movz    w11, #0x0000, lsl #16        // slot 9 blue
    movk    w11, #0x00ff
    b       20f
10: movz    w11, #0x0040, lsl #16        // slot 10 purple
    movk    w11, #0x0080
    b       20f
11: movz    w11, #0x0000, lsl #16        // slot 11 cyan
    movk    w11, #0x8080
    b       20f
12: movz    w11, #0x0040, lsl #16        // slot 12 amber
    movk    w11, #0x00ff
    b       20f
13: movz    w11, #0x00ff, lsl #16        // slot 13 yellow
    movk    w11, #0xffff
    b       20f
14: movz    w11, #0x8000, lsl #16        // slot 14 orange
    movk    w11, #0xff00
    b       20f
15: movz    w11, #0x4000, lsl #16        // slot 15 violet-dark
    movk    w11, #0x00c0
20:
    str     w11, [x10]                   // first permitted memory access
    dsb     sy
    mov     x13, #4
21:
    mov     x14, #23
22:
    stp     w11, w11, [x10, #8]!
    subs    x14, x14, #1
    b.ne    22b
    add     x10, x10, #0xa80
    subs    x13, x13, #1
    b.ne    21b
    dsb     sy

    // Preserve the architectural slot and interrupted SP across the sole C
    // telemetry entry. A nested exception observes X18 and cannot enter C.
    mov     x19, x0
    mov     x20, sp
    adrp    x11, MotoG20Mmu3b1ExceptionStackTop
    add     x11, x11, :lo12:MotoG20Mmu3b1ExceptionStackTop
    mov     sp, x11
    mov     x0, x19
    mov     x1, x20
    bl      MotoG20Mmu3c2cPublishUnexpectedTelemetry
    mov     sp, x20
    mov     x0, x19

    movz    x15, #0x8040
    movk    x15, #0x8100, lsl #16        // MMU3B1 capture block
    mov     x12, #0xe001
    str     x12, [x15, #8]
    str     x0, [x15, #16]
    mrs     x1, ESR_EL2
    mrs     x2, ELR_EL2
    mrs     x3, SPSR_EL2
    mrs     x4, FAR_EL2
    mrs     x5, VBAR_EL2
    mrs     x6, SCTLR_EL2
    mrs     x7, CurrentEL
    stp     x1, x2, [x15, #40]
    stp     x3, x4, [x15, #64]
    stp     x5, x6, [x15, #80]
    mov     x9, sp
    stp     x9, x7, [x15, #96]

    movz    x15, #0x8100
    movk    x15, #0x8100, lsl #16        // TRUE capture block
    mov     x12, #0xe001
    str     x12, [x15, #168]
    str     xzr, [x15, #8]               // TRUE phase = exception/fail-stop
23:
    wfe
    b       23b

.size MotoG20Mmu3c1UnexpectedHandler, .-MotoG20Mmu3c1UnexpectedHandler
