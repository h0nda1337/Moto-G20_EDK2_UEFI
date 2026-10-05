#ifndef MOTO_G20_FOUNDATION_DISPLAY_H_
#define MOTO_G20_FOUNDATION_DISPLAY_H_

#include "MotoG20FoundationBootstrap.h"

VOID
EFIAPI
MotoG20FoundationDisplayBegin (
  IN UINT64  DtbAddress,
  IN UINT64  SctlrEl2
  );

VOID
EFIAPI
MotoG20FoundationDisplayProgress (
  IN MOTO_G20_FOUNDATION_STAGE  Stage,
  IN EFI_STATUS                 Status,
  IN UINT64                     Detail
  );

VOID
EFIAPI
MotoG20FoundationDisplayFailure (
  IN CONST MOTO_G20_FOUNDATION_FAILURE_RECORD  *Record
  );

#endif
