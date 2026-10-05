PHDRS {
  text      PT_LOAD     FLAGS(5);
  data      PT_LOAD     FLAGS(6);
  late      PT_LOAD     FLAGS(5);
}

SECTIONS {
  . = PECOFF_HEADER_SIZE;

  .text : ALIGN(CONSTANT(COMMONPAGESIZE)) {
    KEEP(*(.entry))
    *MotoG20Mmu3c2iIntegration.lib:EntryPoint.obj(.text .text.* .stub .gnu.linkonce.t.*)
    . = 0x3000;
  } :text

  .mmu3.vector_table 0x4000 : ALIGN(16) {
    KEEP(*(.mmu3.vector_table))
  } :text
  ASSERT(ADDR(.mmu3.vector_table) == 0x4000, "MMU3C2F vector base moved")
  ASSERT(SIZEOF(.mmu3.vector_table) == 0x800, "MMU3C2F vector table changed size")
  ASSERT((MotoG20Mmu3b1VectorTableEnd - MotoG20Mmu3b1VectorTableStart) == 0x800, "MMU3C2F vector table symbol size changed")

  .mmu3.unexpected_trampoline 0x4800 : ALIGN(4) {
    KEEP(*(.mmu3.unexpected_trampoline))
  } :text
  ASSERT(ADDR(.mmu3.unexpected_trampoline) == 0x4800, "MMU3C2F trampoline moved")
  ASSERT(SIZEOF(.mmu3.unexpected_trampoline) == 0x60, "MMU3C2F trampoline changed size")
  ASSERT(ADDR(.mmu3.unexpected_trampoline) + SIZEOF(.mmu3.unexpected_trampoline) == 0x4860, "MMU3C2F trampoline escaped carve-out")

  .mmu3.controlled_path 0x4860 : ALIGN(4) {
    KEEP(*(.mmu3.controlled_path))
  } :text
  ASSERT(ADDR(.mmu3.controlled_path) == 0x4860, "MMU3B2 controlled handler moved")
  ASSERT(SIZEOF(.mmu3.controlled_path) == 0x148, "MMU3B2 frozen controlled path changed size")
  ASSERT(ADDR(.mmu3.controlled_path) + SIZEOF(.mmu3.controlled_path) == 0x49A8, "MMU3B2 frozen range end moved")

  .text.pre 0x49B0 : ALIGN(16) {
    KEEP(*(.mmu3true.text))
    KEEP(*(.mmu3c1.checkpoint.text))
    KEEP(*(.mmu3c1.exception.text))
    *(EXCLUDE_FILE (*MotoG20EarlyInitMmu3c2f.obj *MotoG20Mmu3c2cTelemetryMmu3c2h.obj *MotoG20PreOsTelemetryLib.obj *MotoG20PostMmuTelemetryLib.obj) .text .text.* .stub .gnu.linkonce.t.*)
    *(EXCLUDE_FILE (*MotoG20EarlyInitMmu3c2f.obj *MotoG20Mmu3c2cTelemetryMmu3c2h.obj *MotoG20PreOsTelemetryLib.obj *MotoG20PostMmuTelemetryLib.obj) .rodata .rodata.* .gnu.linkonce.r.*)
    KEEP(*(.mmu3true.rodata))
  } :text
  ASSERT(ADDR(.text.pre) >= 0x49A8, "MMU3C2F pre-data text overlaps hard freeze")
  ASSERT(ADDR(.text.pre) + SIZEOF(.text.pre) <= 0x8000, "MMU3C2F pre-data content exceeds envelope")

  ASSERT(MotoG20Mmu3b1UnexpectedException == 0x4800, "MMU3C2F trampoline symbol moved")
  ASSERT(MotoG20Mmu3b2ControlledException == 0x4860, "MMU3B2 controlled handler symbol moved")
  ASSERT(MotoG20Mmu3b2MsrElrInstruction == 0x4968, "MMU3B2 MSR ELR_EL2 moved")
  ASSERT(MotoG20Mmu3b2EretInstruction == 0x496C, "MMU3B2 ERET moved")
  ASSERT(MotoG20Mmu3b2BrkInstruction == 0x49A0, "MMU3B2 BRK moved")
  ASSERT(MotoG20Mmu3b2PostBrk == 0x49A4, "MMU3B2 post-BRK moved")
  ASSERT((MotoG20Mmu3b2ControlledException - MotoG20Mmu3b1UnexpectedException) == 0x60, "MMU3C2F trampoline exceeds carve-out")

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
    *(EXCLUDE_FILE (*LocalEl2VectorsMmu3c2h.obj) .data .data.* .gnu.linkonce.d.*)
    KEEP(*(.prmexportdescriptor))
  } :data

  .bss.movable (NOLOAD) : ALIGN(16) {
    *MotoG20HobListPointerLib.obj(.bss .bss.*)
    *MotoG20EarlyInitMmu3c2f.obj(.bss.gMotoG20Mmu3PreEnableOk)
    *MotoG20EarlyInitMmu3c2f.obj(.bss.gMotoG20Mmu3AfterEnableSctlr)
    *MotoG20EarlyInitMmu3c2f.obj(.bss.gMotoG20Mmu3DataProbe)
    *MotoG20EarlyInitMmu3c2f.obj(.bss.mMotoG20Mmu2aSeenTables)
    *MotoG20EarlyInitMmu3c2f.obj(.bss.mMotoG20Mmu2aUsedEntries)
    . = 0x8E08;
    __Mmu3c2fHexScratchStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.bss.mMotoG20HexDigits)
    __Mmu3c2fHexScratchEnd = .;
    *(EXCLUDE_FILE (*MotoG20HobListPointerLib.obj *MotoG20EarlyInitMmu3c2f.obj) .bss .bss.*)
  } :data
  ASSERT(gMotoG20Mmu3PreEnableOk == 0x81C8, "MMU3C2F pre-enable flag moved")
  ASSERT(gMotoG20Mmu3DataProbe == 0x81D0, "MMU3C2F data probe moved")
  ASSERT(__Mmu3c2fHexScratchStart == 0x8E08, "MMU3C2F hexadecimal scratch moved")
  ASSERT(__Mmu3c2fHexScratchEnd == 0x8E19, "MMU3C2F hexadecimal scratch size changed")
  ASSERT(ADDR(.bss.movable) + SIZEOF(.bss.movable) <= 0x9000, "movable data/BSS collides with fixed stack")

  .data.stack 0x9000 : ALIGN(CONSTANT(COMMONPAGESIZE)) {
    KEEP(*(.data.MotoG20Mmu3b1ExceptionStack))
  } :data
  ASSERT(MotoG20Mmu3b1ExceptionStackBase == 0x9000, "MMU3B2 exception stack moved")
  ASSERT(MotoG20Mmu3b1ExceptionStackTop == 0xA000, "MMU3B2 exception stack top moved")
  ASSERT(SIZEOF(.data.stack) == 0x1000, "MMU3B2 exception stack size changed")

  /*
   * Keep every unmodified MMU3C2C member at its historical RVA.  Only the
   * generated map functions/data occupy the three historical map/self-test
   * envelopes.  The enlarged self-test and post-MMU writer are appended only
   * after the frozen pre-MMU telemetry identity at 0xC5C0.
   */
  .text.mmu3c1_late 0xA000 : ALIGN(16) {
    __Mmu3c2fEarlyInitMainStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20EarlyInit)
    __Mmu3c2fEarlyInitMainEnd = .;
    ASSERT(__Mmu3c2fEarlyInitMainStart == 0xA000, "MMU3C2F EarlyInit main moved")
    ASSERT(__Mmu3c2fEarlyInitMainEnd == 0xAD80, "MMU3C2F EarlyInit main size changed")

    __Mmu3c2fLookupStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20Mmu2aLookup)
    __Mmu3c2fLookupEnd = .;
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20Mmu2aMemoryTable)
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20Mmu2aExpectedTablePages)
    ASSERT(__Mmu3c2fLookupStart == 0xAD80, "MMU3C2F lookup envelope moved")
    ASSERT(__Mmu3c2fLookupEnd == 0xAE28, "MMU3C2F lookup size changed")
    . = 0xAF5C;

    __Mmu3c2fPreservedRenderStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.text.FillRectangle)
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20DrawText)
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20DrawHex64)
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20DrawHexString)
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20DrawMmu3b1Diagnostics)
    __Mmu3c2fPreservedRenderEnd = .;
    ASSERT(__Mmu3c2fPreservedRenderStart == 0xAF5C, "MMU3C2F preserved renderer block moved")
    ASSERT(__Mmu3c2fPreservedRenderEnd == 0xB3F4, "MMU3C2F preserved renderer block changed size")

    __Mmu3c2fWalkStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20Mmu2aWalkTableGraph)
    __Mmu3c2fWalkEnd = .;
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20Mmu2aGuardAddresses)
    ASSERT(__Mmu3c2fWalkStart == 0xB3F4, "MMU3C2F table walker envelope moved")
    ASSERT(__Mmu3c2fWalkEnd == 0xB538, "MMU3C2F table walker size changed")
    . = 0xB5B8;

    __Mmu3c2fPreservedDiagnosticsStart = .;
    KEEP(*(.mmu3b2_diagnostics))
    __Mmu3c2fPreservedDiagnosticsEnd = .;
    ASSERT(__Mmu3c2fPreservedDiagnosticsStart == 0xB5B8, "MMU3C2F diagnostics moved")
    ASSERT(__Mmu3c2fPreservedDiagnosticsEnd == 0xB8C8, "MMU3C2F diagnostics changed size")

    __Mmu3c2fPreservedRodataStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20RealHobGuid)
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.str1.1)
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20DramMap)
    __Mmu3c2fPreservedRodataEnd = .;
    ASSERT(__Mmu3c2fPreservedRodataStart == 0xB8C8, "MMU3C2F preserved rodata moved")
    ASSERT(__Mmu3c2fPreservedRodataEnd == 0xBC60, "MMU3C2F preserved rodata changed size")

    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20Mmu2aCriticalAddresses)
    . = 0xBDF0;
    __Mmu3c2fPreservedTailRodataStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.rodata.mMotoG20HexFont)
    *MotoG20EarlyInitMmu3c2f.obj(.rodata..Lswitch.table.MotoG20EarlyInit)
    __Mmu3c2fPreservedTailRodataEnd = .;
    ASSERT(__Mmu3c2fPreservedTailRodataStart == 0xBDF0, "MMU3C2F tail rodata moved")
    ASSERT(__Mmu3c2fPreservedTailRodataEnd == 0xBE8C, "MMU3C2F tail rodata changed size")

    . = ALIGN(16);
    __Mmu3c2cTelemetryTextStart = .;
    KEEP(*(.mmu3c2c.telemetry.text))
    *MotoG20Mmu3c2cTelemetryMmu3c2h.obj(.text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20PreOsTelemetryLib.obj(.text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20Mmu3c2cTelemetryMmu3c2h.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    *MotoG20PreOsTelemetryLib.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(16);
    __Mmu3c2cTelemetryIdentityStart = .;
    KEEP(*(.mmu3c2c.telemetry.identity))
    __Mmu3c2cTelemetryIdentityEnd = .;
    __Mmu3c2cTelemetryTextEnd = .;

    __Mmu3c2fSelfTestStart = .;
    *MotoG20EarlyInitMmu3c2f.obj(.text.MotoG20Mmu2aSelfTest)
    __Mmu3c2fSelfTestEnd = .;
    . = ALIGN(16);
    __Mmu3c2fPostMmuWriterStart = .;
    *MotoG20PostMmuTelemetryLib.obj(.text .text.* .stub .gnu.linkonce.t.*)
    *MotoG20PostMmuTelemetryLib.obj(.rodata .rodata.* .gnu.linkonce.r.*)
    __Mmu3c2fPostMmuWriterEnd = .;
    . = ALIGN(4);
    __Mmu3c2hDispatcherStart = .;
    KEEP(*(.mmu3c2h.telemetry.dispatcher))
    __Mmu3c2hDispatcherEnd = .;
  } :late

  ASSERT(ADDR(.text.mmu3c1_late) == 0xA000, "MMU3C2F late text start moved")
  ASSERT(__Mmu3c2cTelemetryTextStart == 0xBE90, "MMU3C2C pre-MMU telemetry moved")
  ASSERT(__Mmu3c2cTelemetryIdentityStart == 0xC5A0, "MMU3C2C identity moved")
  ASSERT(__Mmu3c2cTelemetryIdentityEnd == 0xC5C0, "MMU3C2C identity end moved")
  ASSERT((__Mmu3c2cTelemetryIdentityEnd - __Mmu3c2cTelemetryIdentityStart) == 0x20, "MMU3C2C identity field is not exactly 32 bytes")
  ASSERT(__Mmu3c2fSelfTestStart == 0xC5C0, "MMU3C2F expanded self-test moved")
  ASSERT((__Mmu3c2fSelfTestEnd - __Mmu3c2fSelfTestStart) == 0x20C, "MMU3C2F expanded self-test size changed")
  ASSERT(__Mmu3c2fPostMmuWriterStart >= __Mmu3c2fSelfTestEnd, "MMU3C2F post-MMU writer overlaps self-test")
  ASSERT((__Mmu3c2fPostMmuWriterEnd - __Mmu3c2fPostMmuWriterStart) <= 0x420, "MMU3C2F post-MMU writer exceeds capacity")
  ASSERT((__Mmu3c2fPostMmuWriterEnd - __Mmu3c2fPostMmuWriterStart) > 0, "MMU3C2F post-MMU writer missing")
  ASSERT(__Mmu3c2fPostMmuWriterEnd == 0xCBE8, "MMU3C2H frozen post-MMU writer end moved")
  ASSERT(__Mmu3c2hDispatcherStart == 0xCBE8, "MMU3C2H dispatcher start moved")
  ASSERT(__Mmu3c2hDispatcherEnd == 0xCBF4, "MMU3C2H dispatcher end moved")
  ASSERT((__Mmu3c2hDispatcherEnd - __Mmu3c2hDispatcherStart) == 0x0C, "MMU3C2H dispatcher size changed")
  ASSERT((ADDR(.text.mmu3c1_late) + SIZEOF(.text.mmu3c1_late)) == 0xCBF4, "MMU3C2H late end moved")
  ASSERT((0xCE00 - __Mmu3c2hDispatcherEnd) == 0x20C, "MMU3C2H strict late reserve changed")
  ASSERT(ADDR(.text.mmu3c1_late) + SIZEOF(.text.mmu3c1_late) <= 0xCE00, "MMU3C2F late content lacks minimum 0x200 spare")
  ASSERT(0xD000 - (ADDR(.text.mmu3c1_late) + SIZEOF(.text.mmu3c1_late)) >= 0x200, "MMU3C2F Option C virtual spare below 0x200")

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
