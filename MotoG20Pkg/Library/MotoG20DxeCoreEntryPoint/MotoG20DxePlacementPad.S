/** @file
  Inert placement reserve preserving the physically proven DxeCore envelope.

  The last executed Foundation candidate loaded a 0x49000-byte DxeCore with
  entry RVA 0x1000 at runtime address 0x8748C000.  DXE telemetry otherwise
  reduces that image to 0x46000 and would move the top-down allocation.  This
  linked, non-executable reserve restores the exact historical image size.
**/

  .section .data.MotoG20DxePlacementPad, "aw", %progbits
  .balign 16
  .global gMotoG20DxePlacementPad
  .type gMotoG20DxePlacementPad, %object
gMotoG20DxePlacementPad:
  .space 0x3000, 0
  .size gMotoG20DxePlacementPad, . - gMotoG20DxePlacementPad

