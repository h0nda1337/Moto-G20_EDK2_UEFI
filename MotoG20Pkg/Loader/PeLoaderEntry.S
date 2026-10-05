.section .text._loader_entry, "ax"
.align 4

.global _loader_entry
.type _loader_entry, %function

.extern MotoG20PeLoaderMain

_loader_entry:

    //
    // U-Boot handoff:
    //
    // X0 = DTB
    // SP = U-Boot stack
    //
    // EL2, MMU/cache off.
    //

    msr     daifset, #0xf
    isb

    //
    // Preserve original handoff.
    //
    mov     x19, x0
    mov     x20, sp

    //
    // Temporary loader stack:
    //
    // 0x811D0000 - 0x811E0000
    //
    // The loaded PE will later establish its own
    // stack at 0x81200000.
    //
    movz    x9, #0x811e, lsl #16
    mov     sp, x9

    //
    // Loader arguments:
    //
    // X0 = DTB
    // X1 = original U-Boot SP
    //
    mov     x0, x19
    mov     x1, x20

    bl      MotoG20PeLoaderMain

    //
    // X0 == 0 means loader failure.
    // Otherwise X0 is the loaded PE entrypoint.
    //
    cbz     x0, 1f

    mov     x16, x0

    //
    // Restore PE handoff arguments.
    //
    mov     x0, x19
    mov     x1, x20

    dsb     sy
    isb

    br      x16

1:
    wfe
    b       1b

.size _loader_entry, .-_loader_entry
