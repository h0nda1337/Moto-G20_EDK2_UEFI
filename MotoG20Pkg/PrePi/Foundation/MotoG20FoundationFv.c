/** @file
  Fail-closed FDT initrd discovery, FV validation, SHA-256 verification and
  bounded pre-MMU staging for the Moto G20 foundation bootstrap.

  This file is intentionally scalar and allocation-free.  It does not touch
  system registers or enable the MMU, caches, clocks, buses or devices.
**/

#include <Base.h>
#include <PiPei.h>
#include <Pi/PiFirmwareVolume.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>

#include "MotoG20FoundationBootstrap.h"

#define FOUNDATION_TEXT    __attribute__((section (".foundation.text")))
#define FOUNDATION_RODATA  __attribute__((section (".foundation.rodata")))

typedef struct {
  UINT32  State[8];
  UINT64  TotalBytes;
  UINT8   Buffer[64];
  UINTN   BufferUsed;
} MOTO_G20_SHA256_CONTEXT;

STATIC CONST UINT32 mMotoG20Sha256K[64] FOUNDATION_RODATA = {
  0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
  0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
  0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
  0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
  0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
  0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
  0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
  0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
  0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
  0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
  0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
  0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
  0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
  0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
  0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
  0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

STATIC
UINT32
FOUNDATION_TEXT
MotoG20RotateRight32 (
  IN UINT32  Value,
  IN UINTN   Count
  )
{
  return (Value >> Count) | (Value << (32U - Count));
}

STATIC
UINT32
FOUNDATION_TEXT
MotoG20ReadBigEndian32 (
  IN CONST UINT8  *Data
  )
{
  return ((UINT32)Data[0] << 24) |
         ((UINT32)Data[1] << 16) |
         ((UINT32)Data[2] << 8) |
         (UINT32)Data[3];
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20WriteBigEndian32 (
  OUT UINT8   *Data,
  IN  UINT32  Value
  )
{
  Data[0] = (UINT8)(Value >> 24);
  Data[1] = (UINT8)(Value >> 16);
  Data[2] = (UINT8)(Value >> 8);
  Data[3] = (UINT8)Value;
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20Sha256Transform (
  IN OUT MOTO_G20_SHA256_CONTEXT  *Context,
  IN     CONST UINT8                Block[64]
  )
{
  UINT32  W[64];
  UINT32  A;
  UINT32  B;
  UINT32  C;
  UINT32  D;
  UINT32  E;
  UINT32  F;
  UINT32  G;
  UINT32  H;
  UINT32  S0;
  UINT32  S1;
  UINT32  Ch;
  UINT32  Maj;
  UINT32  T1;
  UINT32  T2;
  UINTN   Index;

  for (Index = 0; Index < 16; Index++) {
    W[Index] = MotoG20ReadBigEndian32 (&Block[Index * 4]);
  }

  for (Index = 16; Index < 64; Index++) {
    S0 = MotoG20RotateRight32 (W[Index - 15], 7) ^
         MotoG20RotateRight32 (W[Index - 15], 18) ^
         (W[Index - 15] >> 3);
    S1 = MotoG20RotateRight32 (W[Index - 2], 17) ^
         MotoG20RotateRight32 (W[Index - 2], 19) ^
         (W[Index - 2] >> 10);
    W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
  }

  A = Context->State[0];
  B = Context->State[1];
  C = Context->State[2];
  D = Context->State[3];
  E = Context->State[4];
  F = Context->State[5];
  G = Context->State[6];
  H = Context->State[7];

  for (Index = 0; Index < 64; Index++) {
    S1  = MotoG20RotateRight32 (E, 6) ^ MotoG20RotateRight32 (E, 11) ^ MotoG20RotateRight32 (E, 25);
    Ch  = (E & F) ^ ((~E) & G);
    T1  = H + S1 + Ch + mMotoG20Sha256K[Index] + W[Index];
    S0  = MotoG20RotateRight32 (A, 2) ^ MotoG20RotateRight32 (A, 13) ^ MotoG20RotateRight32 (A, 22);
    Maj = (A & B) ^ (A & C) ^ (B & C);
    T2  = S0 + Maj;
    H   = G;
    G   = F;
    F   = E;
    E   = D + T1;
    D   = C;
    C   = B;
    B   = A;
    A   = T1 + T2;
  }

  Context->State[0] += A;
  Context->State[1] += B;
  Context->State[2] += C;
  Context->State[3] += D;
  Context->State[4] += E;
  Context->State[5] += F;
  Context->State[6] += G;
  Context->State[7] += H;
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20Sha256Init (
  OUT MOTO_G20_SHA256_CONTEXT  *Context
  )
{
  SetMem (Context, sizeof (*Context), 0);
  Context->State[0] = 0x6A09E667U;
  Context->State[1] = 0xBB67AE85U;
  Context->State[2] = 0x3C6EF372U;
  Context->State[3] = 0xA54FF53AU;
  Context->State[4] = 0x510E527FU;
  Context->State[5] = 0x9B05688CU;
  Context->State[6] = 0x1F83D9ABU;
  Context->State[7] = 0x5BE0CD19U;
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20Sha256Update (
  IN OUT MOTO_G20_SHA256_CONTEXT  *Context,
  IN     CONST UINT8               *Data,
  IN     UINTN                      DataSize
  )
{
  UINTN  CopySize;

  Context->TotalBytes += DataSize;
  while (DataSize != 0) {
    CopySize = MIN (DataSize, sizeof (Context->Buffer) - Context->BufferUsed);
    CopyMem (&Context->Buffer[Context->BufferUsed], Data, CopySize);
    Context->BufferUsed += CopySize;
    Data                += CopySize;
    DataSize            -= CopySize;

    if (Context->BufferUsed == sizeof (Context->Buffer)) {
      MotoG20Sha256Transform (Context, Context->Buffer);
      Context->BufferUsed = 0;
    }
  }
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20Sha256Final (
  IN OUT MOTO_G20_SHA256_CONTEXT  *Context,
  OUT    UINT8                     Digest[MOTO_G20_FOUNDATION_SHA256_SIZE]
  )
{
  UINT64  BitCount;
  UINTN   Index;

  BitCount = Context->TotalBytes * 8ULL;
  Context->Buffer[Context->BufferUsed++] = 0x80;
  if (Context->BufferUsed > 56) {
    SetMem (&Context->Buffer[Context->BufferUsed], 64 - Context->BufferUsed, 0);
    MotoG20Sha256Transform (Context, Context->Buffer);
    Context->BufferUsed = 0;
  }

  SetMem (&Context->Buffer[Context->BufferUsed], 56 - Context->BufferUsed, 0);
  for (Index = 0; Index < 8; Index++) {
    Context->Buffer[63 - Index] = (UINT8)(BitCount >> (Index * 8));
  }

  MotoG20Sha256Transform (Context, Context->Buffer);
  for (Index = 0; Index < 8; Index++) {
    MotoG20WriteBigEndian32 (&Digest[Index * 4], Context->State[Index]);
  }

  SetMem (Context, sizeof (*Context), 0);
}

STATIC
VOID
FOUNDATION_TEXT
MotoG20Sha256All (
  IN  CONST VOID  *Data,
  IN  UINTN        DataSize,
  OUT UINT8        Digest[MOTO_G20_FOUNDATION_SHA256_SIZE]
  )
{
  MOTO_G20_SHA256_CONTEXT  Context;

  MotoG20Sha256Init (&Context);
  MotoG20Sha256Update (&Context, Data, DataSize);
  MotoG20Sha256Final (&Context, Digest);
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20RangeWithin (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  ContainerBase,
  IN UINT64  ContainerEnd
  )
{
  return (BOOLEAN)(
                    (Size != 0) &&
                    (Base >= ContainerBase) &&
                    (Base < ContainerEnd) &&
                    (Size <= (ContainerEnd - Base))
                    );
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20RangesOverlap (
  IN UINT64  BaseA,
  IN UINT64  SizeA,
  IN UINT64  BaseB,
  IN UINT64  SizeB
  )
{
  return (BOOLEAN)((BaseA < (BaseB + SizeB)) && (BaseB < (BaseA + SizeA)));
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20DigestIsProvisioned (
  IN CONST UINT8  Digest[MOTO_G20_FOUNDATION_SHA256_SIZE]
  )
{
  UINT8  Combined;
  UINTN  Index;

  Combined = 0;
  for (Index = 0; Index < MOTO_G20_FOUNDATION_SHA256_SIZE; Index++) {
    Combined |= Digest[Index];
  }

  return (BOOLEAN)(Combined != 0);
}

STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20DecodeFdtAddress (
  IN  CONST VOID  *Property,
  IN  INT32        PropertyLength,
  OUT UINT64      *Address
  )
{
  UINT32  Value32;
  UINT64  Value64;

  if ((Property == NULL) || (Address == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (PropertyLength == sizeof (UINT32)) {
    CopyMem (&Value32, Property, sizeof (Value32));
    *Address = Fdt32ToCpu (Value32);
    return EFI_SUCCESS;
  }

  if (PropertyLength == sizeof (UINT64)) {
    CopyMem (&Value64, Property, sizeof (Value64));
    *Address = Fdt64ToCpu (Value64);
    return EFI_SUCCESS;
  }

  return EFI_COMPROMISED_DATA;
}

EFI_STATUS
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationPlanAndStageFv (
  IN  UINT64                                  DtbAddress,
  IN  CONST MOTO_G20_FOUNDATION_FV_IDENTITY  *ExpectedIdentity,
  OUT MOTO_G20_FOUNDATION_FV_PLAN             *Plan
  )
{
  CONST VOID                   *Fdt;
  CONST VOID                   *Property;
  EFI_FIRMWARE_VOLUME_HEADER   Header;
  INT32                        Chosen;
  INT32                        Length;
  UINT32                       DtbSize;
  UINT64                       InitrdStart;
  UINT64                       InitrdEnd;
  UINT64                       InitrdSize;
  UINT8                        SourceDigest[MOTO_G20_FOUNDATION_SHA256_SIZE];
  UINT8                        StagedDigest[MOTO_G20_FOUNDATION_SHA256_SIZE];

  if ((ExpectedIdentity == NULL) || (Plan == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  SetMem (Plan, sizeof (*Plan), 0);
  if ((ExpectedIdentity->TransportSize == 0) ||
      (ExpectedIdentity->TransportSize > MOTO_G20_FOUNDATION_FV_TRANSPORT_MAX) ||
      (ExpectedIdentity->FirmwareVolumeSize < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) ||
      (ExpectedIdentity->FirmwareVolumeSize > ExpectedIdentity->TransportSize) ||
      !MotoG20DigestIsProvisioned (ExpectedIdentity->TransportSha256)) {
    return EFI_SECURITY_VIOLATION;
  }

  if (DtbAddress != MOTO_G20_FOUNDATION_DTB_ADDRESS) {
    return EFI_SECURITY_VIOLATION;
  }

  Fdt = (CONST VOID *)(UINTN)DtbAddress;
  if (FdtCheckHeader (Fdt) != 0) {
    return EFI_COMPROMISED_DATA;
  }

  DtbSize = FdtTotalSize (Fdt);
  if ((DtbSize < sizeof (FDT_HEADER)) ||
      (DtbSize > MOTO_G20_FOUNDATION_DTB_MAX_SIZE) ||
      !MotoG20RangeWithin (DtbAddress, DtbSize, MOTO_G20_FOUNDATION_DRAM_BASE, MOTO_G20_FOUNDATION_DRAM_END)) {
    return EFI_COMPROMISED_DATA;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return EFI_NOT_FOUND;
  }

  Property = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &Length);
  if (EFI_ERROR (MotoG20DecodeFdtAddress (Property, Length, &InitrdStart))) {
    return EFI_COMPROMISED_DATA;
  }

  Property = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &Length);
  if (EFI_ERROR (MotoG20DecodeFdtAddress (Property, Length, &InitrdEnd))) {
    return EFI_COMPROMISED_DATA;
  }

  if ((InitrdStart == 0) || (InitrdEnd <= InitrdStart) || ((InitrdStart & 7U) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  InitrdSize = InitrdEnd - InitrdStart;
  if ((InitrdStart != MOTO_G20_FOUNDATION_INITRD_SOURCE_BASE) ||
      (InitrdSize != ExpectedIdentity->TransportSize) ||
      !MotoG20RangeWithin (InitrdStart, InitrdSize, MOTO_G20_FOUNDATION_DRAM_BASE, MOTO_G20_FOUNDATION_DRAM_END) ||
      !MotoG20RangeWithin (
         InitrdStart,
         InitrdSize,
         MOTO_G20_FOUNDATION_INITRD_SOURCE_BASE,
         MOTO_G20_FOUNDATION_INITRD_SOURCE_END
         ) ||
      MotoG20RangesOverlap (InitrdStart, InitrdSize, DtbAddress, DtbSize) ||
      MotoG20RangesOverlap (
        InitrdStart,
        InitrdSize,
        MOTO_G20_FOUNDATION_FV_STAGING_BASE,
        MOTO_G20_FOUNDATION_FV_STAGING_END - MOTO_G20_FOUNDATION_FV_STAGING_BASE
        )) {
    return EFI_SECURITY_VIOLATION;
  }

  CopyMem (&Header, (CONST VOID *)(UINTN)InitrdStart, sizeof (Header));
  if ((Header.Signature != EFI_FVH_SIGNATURE) ||
      (Header.Revision != EFI_FVH_REVISION) ||
      (Header.FvLength != ExpectedIdentity->FirmwareVolumeSize) ||
      (Header.HeaderLength < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) ||
      (Header.HeaderLength > Header.FvLength) ||
      ((Header.HeaderLength & 1U) != 0) ||
      ((Header.ExtHeaderOffset != 0) &&
       ((Header.ExtHeaderOffset < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) ||
        (Header.ExtHeaderOffset >= Header.FvLength))) ||
      (CalculateSum16 ((CONST UINT16 *)(UINTN)InitrdStart, Header.HeaderLength) != 0)) {
    return EFI_VOLUME_CORRUPTED;
  }

  MotoG20Sha256All ((CONST VOID *)(UINTN)InitrdStart, (UINTN)InitrdSize, SourceDigest);
  if (CompareMem (SourceDigest, ExpectedIdentity->TransportSha256, sizeof (SourceDigest)) != 0) {
    return EFI_SECURITY_VIOLATION;
  }

  CopyMem (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_FV_STAGING_BASE,
    (CONST VOID *)(UINTN)InitrdStart,
    (UINTN)InitrdSize
    );
  MotoG20Sha256All (
    (CONST VOID *)(UINTN)MOTO_G20_FOUNDATION_FV_STAGING_BASE,
    (UINTN)InitrdSize,
    StagedDigest
    );
  if ((CompareMem (SourceDigest, StagedDigest, sizeof (SourceDigest)) != 0) ||
      (CompareMem (StagedDigest, ExpectedIdentity->TransportSha256, sizeof (StagedDigest)) != 0)) {
    SetMem ((VOID *)(UINTN)MOTO_G20_FOUNDATION_FV_STAGING_BASE, (UINTN)InitrdSize, 0);
    return EFI_CRC_ERROR;
  }

  Plan->DtbAddress          = DtbAddress;
  Plan->DtbSize             = DtbSize;
  Plan->SourceBase          = InitrdStart;
  Plan->SourceSize          = InitrdSize;
  Plan->StagingBase         = MOTO_G20_FOUNDATION_FV_STAGING_BASE;
  Plan->StagingSize         = InitrdSize;
  Plan->FirmwareVolumeSize  = Header.FvLength;
  CopyMem (Plan->VerifiedSha256, StagedDigest, sizeof (Plan->VerifiedSha256));
  return EFI_SUCCESS;
}
