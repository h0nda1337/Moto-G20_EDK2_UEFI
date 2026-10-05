PHDRS {
  text      PT_LOAD     FLAGS(5);
  data      PT_LOAD     FLAGS(6);
  late      PT_LOAD     FLAGS(5);
}

SECTIONS {
  . = PECOFF_HEADER_SIZE;

  .text : ALIGN(CONSTANT(COMMONPAGESIZE)) {
    KEEP(*(.entry))
    *MotoG20Mmu3c2cTelemetry.lib:EntryPoint.obj(.text .text.* .stub .gnu.linkonce.t.*)
    . = 0x3000;
  } :text

  .mmu3.vector_table 0x4000 : ALIGN(16) {
    KEEP(*(.mmu3.vector_table))
  } :text
  ASSERT(ADDR(.mmu3.vector_table) == 0x4000, "MMU3C2C vector base moved")
  ASSERT(SIZEOF(.mmu3.vector_table) == 0x800, "MMU3C2C vector table changed size")
  ASSERT((MotoG20Mmu3b1VectorTableEnd - MotoG20Mmu3b1VectorTableStart) == 0x800, "MMU3C2C vector table symbol size changed")

  .mmu3.unexpected_trampoline 0x4800 : ALIGN(4) {
    KEEP(*(.mmu3.unexpected_trampoline))
  } :text
  ASSERT(ADDR(.mmu3.unexpected_trampoline) == 0x4800, "MMU3C2C trampoline moved")
  ASSERT(SIZEOF(.mmu3.unexpected_trampoline) == 0x60, "MMU3C2C trampoline changed size")
  ASSERT(ADDR(.mmu3.unexpected_trampoline) + SIZEOF(.mmu3.unexpected_trampoline) == 0x4860, "MMU3C2C trampoline escaped carve-out")

  .mmu3.controlled_path 0x4860 : ALIGN(4) {
    KEEP(*(.mmu3.controlled_path))
  } :text
  ASSERT(ADDR(.mmu3.controlled_path) == 0x4860, "MMU3B2 controlled handler moved")
  ASSERT(SIZEOF(.mmu3.controlled_path) == 0x148, "MMU3B2 frozen controlled path changed size")
  ASSERT(ADDR(.mmu3.controlled_path) + SIZEOF(.mmu3.controlled_path) == 0x49A8, "MMU3B2 frozen range end moved")

  /*
   * Preserve the original early RX topology.  The telemetry bridge and
   * writer are excluded here so the 0x49B0-0x8000 envelope retains room for
   * the post-evidence callsite added to the unexpected handler.
   */
  .text.pre 0x49B0 : ALIGN(16) {
    KEEP(*(.mmu3true.text))
    KEEP(*(.mmu3c1.checkpoint.text))
    KEEP(*(.mmu3c1.exception.text))
    *(EXCLUDE_FILE (*MotoG20EarlyInit.obj *MotoG20Mmu3c2cTelemetry.obj *MotoG20PreOsTelemetryLib.obj) .text .text.* .stub .gnu.linkonce.t.*)
    *(EXCLUDE_FILE (*MotoG20EarlyInit.obj *MotoG20Mmu3c2cTelemetry.obj *MotoG20PreOsTelemetryLib.obj) .rodata .rodata.* .gnu.linkonce.r.*)
    KEEP(*(.mmu3true.rodata))
  } :text
  ASSERT(ADDR(.text.pre) >= 0x49A8, "MMU3C2C pre-data text overlaps hard freeze")
  ASSERT(ADDR(.text.pre) + SIZEOF(.text.pre) <= 0x8000, "MMU3C2C pre-data content exceeds envelope")

  ASSERT(MotoG20Mmu3b1UnexpectedException == 0x4800, "MMU3C2C trampoline symbol moved")
  ASSERT(MotoG20Mmu3b2ControlledException == 0x4860, "MMU3B2 controlled handler symbol moved")
  ASSERT(MotoG20Mmu3b2MsrElrInstruction == 0x4968, "MMU3B2 MSR ELR_EL2 moved")
  ASSERT(MotoG20Mmu3b2EretInstruction == 0x496C, "MMU3B2 ERET moved")
  ASSERT(MotoG20Mmu3b2BrkInstruction == 0x49A0, "MMU3B2 BRK moved")
  ASSERT(MotoG20Mmu3b2PostBrk == 0x49A4, "MMU3B2 post-BRK moved")
  ASSERT((MotoG20Mmu3b2ControlledException - MotoG20Mmu3b1UnexpectedException) == 0x60, "MMU3C2C trampoline exceeds carve-out")

  .data.fixed 0x8000 : ALIGN(64) {
    __Mmu3RelocCookieAddress = .;
    KEEP(*(.data.mRelocCookie))
    __Mmu3RelocCookiePointerAddress = .;
    KEEP(*(.data.mRelocCookiePointer))
    . = ALIGN(64);
    __Mmu3CaptureBlockAddress = .;
    KEEP(*(.data.MotoG20Mmu3b1Capture))
    . = ALIGN(64);
    __Mmu3TrueCaptureBlockAddress = .;
    KEEP(*(.data.MotoG20Mmu3b1TrueCapture))
  } :data
  ASSERT(__Mmu3RelocCookieAddress == 0x8000, "relocation cookie moved")
  ASSERT(__Mmu3RelocCookiePointerAddress == 0x8008, "relocation cookie pointer moved")
  ASSERT(__Mmu3CaptureBlockAddress == 0x8040, "MMU3B2 capture input section moved")
  ASSERT(__Mmu3TrueCaptureBlockAddress == 0x8100, "TRUE capture input section moved")
  ASSERT(MotoG20Mmu3b1CaptureBlock == 0x8040, "MMU3B2 capture moved")
  ASSERT(MotoG20Mmu3TrueCaptureBlock == 0x8100, "TRUE capture moved")
  ASSERT(ADDR(.data.fixed) + SIZEOF(.data.fixed) <= 0x81B0, "fixed data exceeds reserved prefix")

  .data.movable 0x81B0 : ALIGN(16) {
    *(EXCLUDE_FILE (*LocalEl2Vectors.obj) .data .data.* .gnu.linkonce.d.*)
    KEEP(*(.prmexportdescriptor))
  } :data

  .bss.movable (NOLOAD) : ALIGN(16) {
    *(.bss .bss.*)
  } :data
  ASSERT(ADDR(.bss.movable) + SIZEOF(.bss.movable) <= 0x9000, "movable data/BSS collides with fixed stack")

  .data.stack 0x9000 : ALIGN(CONSTANT(COMMONPAGESIZE)) {
    KEEP(*(.data.MotoG20Mmu3b1ExceptionStack))
  } :data
  ASSERT(MotoG20Mmu3b1ExceptionStackBase == 0x9000, "MMU3B2 exception stack moved")
  ASSERT(MotoG20Mmu3b1ExceptionStackTop == 0xA000, "MMU3B2 exception stack top moved")
  ASSERT(SIZEOF(.data.stack) == 0x1000, "MMU3B2 exception stack size changed")

  /*
   * Preserve every original Option-C late member at its historical address,
   * then append the bridge, generic writer, and the sole 32-byte zero identity
   * field.  The minimum 0x200 spare remains a hard assertion.
   */
  .text.mmu3c1_late 0xA000 : ALIGN(16) {
    *MotoG20Mmu3c2cTelemetry.lib:MotoG20EarlyInit.obj(.text .text.* .stub .gnu.linkonce.t.*)
    KEEP(*(.mmu3b2_diagnostics))
    *(EXCLUDE_FILE (*MotoG20Mmu3c2cTelemetry.obj *MotoG20PreOsTelemetryLib.obj) .text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20Mmu3c2cTelemetry.lib:MotoG20EarlyInit.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    *(EXCLUDE_FILE (*MotoG20Mmu3c2cTelemetry.obj *MotoG20PreOsTelemetryLib.obj) .rodata .rodata.* .gnu.linkonce.r.*)

    . = ALIGN(16);
    __Mmu3c2cTelemetryTextStart = .;
    KEEP(*(.mmu3c2c.telemetry.text))
    *MotoG20Mmu3c2cTelemetry.obj(.text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20PreOsTelemetryLib.obj(.text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20Mmu3c2cTelemetry.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    *MotoG20PreOsTelemetryLib.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(16);
    __Mmu3c2cTelemetryIdentityStart = .;
    KEEP(*(.mmu3c2c.telemetry.identity))
    __Mmu3c2cTelemetryIdentityEnd = .;
    __Mmu3c2cTelemetryTextEnd = .;
  } :late
  ASSERT(ADDR(.text.mmu3c1_late) == 0xA000, "MMU3C2C late text start moved")
  ASSERT(__Mmu3c2cTelemetryTextStart >= 0xBE8C, "MMU3C2C telemetry overlaps frozen Option-C late payload")
  ASSERT((__Mmu3c2cTelemetryIdentityEnd - __Mmu3c2cTelemetryIdentityStart) == 0x20, "MMU3C2C identity field is not exactly 32 bytes")
  ASSERT(__Mmu3c2cTelemetryTextEnd <= 0xCE00, "MMU3C2C telemetry lacks minimum 0x200 spare")
  ASSERT(ADDR(.text.mmu3c1_late) + SIZEOF(.text.mmu3c1_late) <= 0xCE00, "MMU3C2C late content lacks minimum 0x200 spare")
  ASSERT(0xD000 - (ADDR(.text.mmu3c1_late) + SIZEOF(.text.mmu3c1_late)) >= 0x200, "MMU3C2C Option C virtual spare below 0x200")

  .eh_frame (INFO) : {
    KEEP(*(.eh_frame))
  }

  .rela (INFO) : {
    *(.rela .rela.*)
  }

  .hii : ALIGN(CONSTANT(COMMONPAGESIZE)) {
    KEEP(*(.hii))
  } :data

  .got : {
    *(.got)
  }
  ASSERT(SIZEOF(.got) == 0, "Unexpected GOT entries detected!")

  .got.plt (INFO) : {
    *(.got.plt)
  }
  ASSERT(SIZEOF(.got.plt) == 0 || SIZEOF(.got.plt) == 0xc || SIZEOF(.got.plt) == 0x18, "Unexpected GOT entries detected!")

  .build-id (INFO) : {
    *(.note.gnu.build-id)
  }

  /DISCARD/ : {
    *(.note.GNU-stack)
    *(.gnu_debuglink)
    *(.interp)
    *(.dynsym)
    *(.dynstr)
    *(.hash .gnu.hash)
    *(.comment)
  }
}
