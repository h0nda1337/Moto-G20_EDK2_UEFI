/*
  MMU3C2L A5 pre-M page-walk provenance route.

  This is entered by a single surgical branch replacement at the existing
  common failure writer (RVA 0x5CD8).  It deliberately preserves the visible
  24x8 red lateral failure stripe for every pre/post-M failure.

  On the pre-M page-walk path only, W0 is 800 and the live registers still
  identify the first failed leaf:

    * PE-page loop: X19 is -0x1000 + (leaf * 0x1000), leaves 0..13.
    * Data-point loop: X19 is the data VA and X23 is (leaf * 8),
      leaves 14..46.

  The helper writes no normal RAM.  After drawing the existing red stripe via
  the frozen FillStripe routine, it places a 3x2 white tag in S2:

    S2 origin = (656, 800)
    column    = leaf & 15
    row       = leaf >> 4
    tag       = (656 + 3 * column, 800 + 2 * row), 3x2

  Thus 47 leaves map one-to-one to cells 0..46; cell 47 is intentionally
  unused.  The code remains in the A4 pre-data padding and must be exactly
  0x88 bytes.  The final A5 linker contract defines MotoG20A5FillStripe at
  frozen RVA 0x5134 without changing the historical local C symbol.
*/

        .section .mmu3a5.pagewalk.failure, "ax", %progbits
        .align  2
        .global MotoG20Mmu3A5PageWalkFailure
        .type   MotoG20Mmu3A5PageWalkFailure, %function
        .extern MotoG20A5FillStripe

MotoG20Mmu3A5PageWalkFailure:
        /* Compute both candidate leaf indices before choosing one. */
        add     x8, x19, #0x1000
        lsr     x8, x8, #12
        lsr     x9, x23, #3
        add     x9, x9, #14
        cmp     x19, #0x10, lsl #12
        csel    x8, x8, x9, lt

        /* Convert the selected leaf to a 16x3 S2 tag cell. */
        lsr     x9, x8, #4
        and     x10, x8, #15
        add     x9, x9, x9
        add     x9, x9, #800
        add     x10, x10, x10, lsl #1
        add     x10, x10, #656
        movz    x11, #0x9e00, lsl #16
        mov     x12, #720
        madd    x9, x9, x12, x10
        add     x19, x11, x9, lsl #2

        /*
          A non-page-walk failure arrives here with a different Y in W0.  Its
          computed pointer is discarded before the framebuffer call.
        */
        cmp     w0, #800
        csel    x19, x19, xzr, eq

        /* Reproduce the frozen FillStripe(Y, 624, 24, red) convention. */
        mov     w1, #624
        mov     w2, #24
        mov     w3, #0xff0000
        bl      MotoG20A5FillStripe

        cbz     x19, .Lwait
        mov     w8, #0xffff
        movk    w8, #0x00ff, lsl #16
        str     w8, [x19]
        str     w8, [x19, #4]
        str     w8, [x19, #8]
        add     x19, x19, #0xb40
        str     w8, [x19]
        str     w8, [x19, #4]
        str     w8, [x19, #8]

.Lwait:
        wfe
        b       .Lwait

        .size   MotoG20Mmu3A5PageWalkFailure, . - MotoG20Mmu3A5PageWalkFailure
