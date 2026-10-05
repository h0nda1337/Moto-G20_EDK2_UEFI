typedef unsigned char       U8;
typedef unsigned short      U16;
typedef unsigned int        U32;
typedef unsigned long long  U64;
typedef unsigned long       UINTN;

#define PE_SOURCE_BASE      0x800A0000ULL
#define PE_LOAD_BASE        0x81000000ULL

#define MAX_PE_FILE_SIZE    0x00100000U
#define MAX_PE_IMAGE_SIZE   0x00100000U
#define MAX_PE_SECTIONS     32U

#define FRAMEBUFFER_BASE    0x9E000000ULL
#define DISPLAY_WIDTH       720U

#define IMAGE_FILE_MACHINE_ARM64  0xAA64U
#define PE32_PLUS_MAGIC           0x020BU

#define IMAGE_REL_BASED_ABSOLUTE  0U
#define IMAGE_REL_BASED_DIR64     10U

static U16
Read16 (
    const U8 *p
    )
{
    return
        ((U16)p[0]) |
        ((U16)p[1] << 8);
}

static U32
Read32 (
    const U8 *p
    )
{
    return
        ((U32)p[0]) |
        ((U32)p[1] << 8) |
        ((U32)p[2] << 16) |
        ((U32)p[3] << 24);
}

static U64
Read64 (
    const U8 *p
    )
{
    return
        ((U64)Read32(p)) |
        ((U64)Read32(p + 4) << 32);
}

static void
Write64 (
    U8  *p,
    U64 Value
    )
{
    p[0] = (U8)(Value);
    p[1] = (U8)(Value >> 8);
    p[2] = (U8)(Value >> 16);
    p[3] = (U8)(Value >> 24);
    p[4] = (U8)(Value >> 32);
    p[5] = (U8)(Value >> 40);
    p[6] = (U8)(Value >> 48);
    p[7] = (U8)(Value >> 56);
}

static void
CopyBytes (
    U8       *Destination,
    const U8 *Source,
    U32       Size
    )
{
    U32 Index;

    for (Index = 0; Index < Size; Index++) {
        Destination[Index] = Source[Index];
    }
}

static void
ZeroBytes (
    U8  *Destination,
    U32  Size
    )
{
    U32 Index;

    for (Index = 0; Index < Size; Index++) {
        Destination[Index] = 0;
    }
}

static void
DrawStatus (
    U32 X,
    U32 Y,
    U32 Color
    )
{
    volatile U32 *Framebuffer;
    U32 Px;
    U32 Py;

    Framebuffer =
        (volatile U32 *)(UINTN)FRAMEBUFFER_BASE;

    for (Py = Y; Py < Y + 24U; Py++) {
        for (Px = X; Px < X + 24U; Px++) {
            Framebuffer[
                (Py * DISPLAY_WIDTH) + Px
            ] = Color;
        }
    }
}

static U64
LoaderFail (
    U32 Stage
    )
{
    //
    // Red failure block. Horizontal position also
    // indicates approximately which stage failed.
    //
    DrawStatus (
        Stage * 32U,
        160U,
        0x00FF0000U
        );

    return 0;
}

U64
MotoG20PeLoaderMain (
    U64 DtbAddress,
    U64 OriginalStack
    )
{
    const U8 *Source;
    U8       *Destination;

    U32 PeOffset;
    U32 FileHeader;
    U32 OptionalHeader;
    U32 SectionTable;

    U16 Machine;
    U16 NumberOfSections;
    U16 OptionalHeaderSize;
    U16 OptionalMagic;

    U32 EntryRva;
    U64 ImageBase;
    U32 SizeOfImage;
    U32 SizeOfHeaders;

    U32 NumberOfRvaAndSizes;
    U32 RelocRva;
    U32 RelocSize;

    U32 Index;
    U64 Delta;

    (void)DtbAddress;
    (void)OriginalStack;

    Source =
        (const U8 *)(UINTN)PE_SOURCE_BASE;

    Destination =
        (U8 *)(UINTN)PE_LOAD_BASE;

    //
    // Marker 0: loader itself executed.
    // ORANGE
    //
    DrawStatus (
        0U,
        160U,
        0x00FF8000U
        );

    //
    // DOS signature.
    //
    if ((Source[0] != 'M') ||
        (Source[1] != 'Z')) {
        return LoaderFail(1U);
    }

    PeOffset = Read32(Source + 0x3CU);

    if (PeOffset > (MAX_PE_FILE_SIZE - 0x100U)) {
        return LoaderFail(2U);
    }

    //
    // PE\0\0
    //
    if ((Source[PeOffset + 0] != 'P') ||
        (Source[PeOffset + 1] != 'E') ||
        (Source[PeOffset + 2] != 0) ||
        (Source[PeOffset + 3] != 0)) {
        return LoaderFail(3U);
    }

    FileHeader =
        PeOffset + 4U;

    Machine =
        Read16(Source + FileHeader + 0U);

    NumberOfSections =
        Read16(Source + FileHeader + 2U);

    OptionalHeaderSize =
        Read16(Source + FileHeader + 16U);

    if (Machine != IMAGE_FILE_MACHINE_ARM64) {
        return LoaderFail(4U);
    }

    if ((NumberOfSections == 0U) ||
        (NumberOfSections > MAX_PE_SECTIONS)) {
        return LoaderFail(5U);
    }

    OptionalHeader =
        FileHeader + 20U;

    OptionalMagic =
        Read16(Source + OptionalHeader);

    if (OptionalMagic != PE32_PLUS_MAGIC) {
        return LoaderFail(6U);
    }

    if (OptionalHeaderSize < 0xF0U) {
        return LoaderFail(7U);
    }

    EntryRva =
        Read32(Source + OptionalHeader + 16U);

    ImageBase =
        Read64(Source + OptionalHeader + 24U);

    SizeOfImage =
        Read32(Source + OptionalHeader + 56U);

    SizeOfHeaders =
        Read32(Source + OptionalHeader + 60U);

    NumberOfRvaAndSizes =
        Read32(Source + OptionalHeader + 108U);

    if ((SizeOfImage == 0U) ||
        (SizeOfImage > MAX_PE_IMAGE_SIZE)) {
        return LoaderFail(8U);
    }

    if ((SizeOfHeaders == 0U) ||
        (SizeOfHeaders > SizeOfImage) ||
        (SizeOfHeaders > MAX_PE_FILE_SIZE)) {
        return LoaderFail(9U);
    }

    if (EntryRva >= SizeOfImage) {
        return LoaderFail(10U);
    }

    //
    // Marker 1: PE headers validated.
    // YELLOW
    //
    DrawStatus (
        32U,
        160U,
        0x00FFFF00U
        );

    //
    // Start from a completely zeroed destination.
    //
    ZeroBytes (
        Destination,
        SizeOfImage
        );

    //
    // Copy PE headers.
    //
    CopyBytes (
        Destination,
        Source,
        SizeOfHeaders
        );

    SectionTable =
        OptionalHeader +
        (U32)OptionalHeaderSize;

    //
    // Load each section by RVA.
    //
    for (Index = 0;
         Index < (U32)NumberOfSections;
         Index++) {

        U32 Section;
        U32 VirtualSize;
        U32 VirtualAddress;
        U32 RawSize;
        U32 RawPointer;
        U32 Span;

        Section =
            SectionTable +
            (Index * 40U);

        if (Section >
            (MAX_PE_FILE_SIZE - 40U)) {
            return LoaderFail(11U);
        }

        VirtualSize =
            Read32(Source + Section + 8U);

        VirtualAddress =
            Read32(Source + Section + 12U);

        RawSize =
            Read32(Source + Section + 16U);

        RawPointer =
            Read32(Source + Section + 20U);

        Span =
            (VirtualSize > RawSize)
            ? VirtualSize
            : RawSize;

        if (VirtualAddress > SizeOfImage) {
            return LoaderFail(12U);
        }

        if (Span >
            (SizeOfImage - VirtualAddress)) {
            return LoaderFail(13U);
        }

        if (RawSize != 0U) {
            if (RawPointer > MAX_PE_FILE_SIZE) {
                return LoaderFail(14U);
            }

            if (RawSize >
                (MAX_PE_FILE_SIZE - RawPointer)) {
                return LoaderFail(15U);
            }

            CopyBytes (
                Destination + VirtualAddress,
                Source + RawPointer,
                RawSize
                );
        }

        //
        // Destination was pre-zeroed, therefore:
        //
        // VirtualSize > RawSize
        //
        // is already correctly zero-filled.
        //
    }

    //
    // Marker 2: all sections loaded.
    // MAGENTA
    //
    DrawStatus (
        64U,
        160U,
        0x00FF00FFU
        );

    //
    // Base relocation directory.
    //
    RelocRva  = 0U;
    RelocSize = 0U;

    if (NumberOfRvaAndSizes > 5U) {
        RelocRva =
            Read32(
                Source +
                OptionalHeader +
                112U +
                (5U * 8U)
                );

        RelocSize =
            Read32(
                Source +
                OptionalHeader +
                112U +
                (5U * 8U) +
                4U
                );
    }

    Delta =
        PE_LOAD_BASE - ImageBase;

    if ((Delta != 0ULL) &&
        (RelocSize != 0U)) {

        U32 Consumed;

        if (RelocRva >= SizeOfImage) {
            return LoaderFail(16U);
        }

        if (RelocSize >
            (SizeOfImage - RelocRva)) {
            return LoaderFail(17U);
        }

        Consumed = 0U;

        while (Consumed < RelocSize) {

            U8  *Block;
            U32 PageRva;
            U32 BlockSize;
            U32 EntryCount;
            U32 EntryIndex;

            if ((RelocSize - Consumed) < 8U) {
                return LoaderFail(18U);
            }

            Block =
                Destination +
                RelocRva +
                Consumed;

            PageRva =
                Read32(Block + 0U);

            BlockSize =
                Read32(Block + 4U);

            if (BlockSize < 8U) {
                return LoaderFail(19U);
            }

            if (BlockSize >
                (RelocSize - Consumed)) {
                return LoaderFail(20U);
            }

            if ((BlockSize & 1U) != 0U) {
                return LoaderFail(21U);
            }

            EntryCount =
                (BlockSize - 8U) >> 1;

            for (EntryIndex = 0U;
                 EntryIndex < EntryCount;
                 EntryIndex++) {

                U16 Reloc;
                U32 Type;
                U32 Offset;
                U32 PatchRva;
                U8 *Patch;
                U64 Value;

                Reloc =
                    Read16(
                        Block +
                        8U +
                        (EntryIndex * 2U)
                        );

                Type =
                    ((U32)Reloc) >> 12;

                Offset =
                    ((U32)Reloc) & 0xFFFU;

                if (Type ==
                    IMAGE_REL_BASED_ABSOLUTE) {
                    continue;
                }

                if (Type !=
                    IMAGE_REL_BASED_DIR64) {
                    return LoaderFail(22U);
                }

                PatchRva =
                    PageRva + Offset;

                if (PatchRva >
                    (SizeOfImage - 8U)) {
                    return LoaderFail(23U);
                }

                Patch =
                    Destination +
                    PatchRva;

                Value =
                    Read64(Patch);

                Write64(
                    Patch,
                    Value + Delta
                    );
            }

            Consumed += BlockSize;
        }

    } else if ((Delta != 0ULL) &&
               (RelocSize == 0U)) {

        //
        // Moving an image without relocation information
        // is unsafe for a generic loader.
        //
        return LoaderFail(24U);
    }

    //
    // Marker 3: relocations applied.
    // WHITE
    //
    DrawStatus (
        96U,
        160U,
        0x00FFFFFFU
        );

    //
    // Loaded PE entrypoint.
    //
    return
        PE_LOAD_BASE +
        (U64)EntryRva;
}
