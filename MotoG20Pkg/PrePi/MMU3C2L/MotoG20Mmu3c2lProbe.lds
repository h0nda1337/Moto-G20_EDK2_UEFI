SECTIONS
{
  . = 0x7F78;
  .mmu3a5.pagewalk.failure : {
    KEEP(*(.mmu3a5.pagewalk.failure))
  }

  ASSERT(ADDR(.mmu3a5.pagewalk.failure) == 0x7F78,
         "A5 helper start moved")
  ASSERT(SIZEOF(.mmu3a5.pagewalk.failure) == 0x88,
         "A5 helper size changed")
  ASSERT((ADDR(.mmu3a5.pagewalk.failure) + SIZEOF(.mmu3a5.pagewalk.failure)) <= 0x8000,
         "A5 helper exceeds pre-data padding")
}
