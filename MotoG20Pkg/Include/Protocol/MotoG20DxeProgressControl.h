/** @file
  Private handoff from the Moto G20 protocol monitor to platform BDS.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL_H_
#define MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL_H_

#include <Uefi.h>

#define MOTO_G20_DXE_PROGRESS_CONTROL_REVISION  1U

typedef struct _MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL;

/**
  Permanently stop ordinary monitor drawing and return the observed bitmap.

  Call on the BSP at TPL_APPLICATION, during Boot Services only.  All monitor
  events run at TPL_CALLBACK.  The implementation latches suppression at
  TPL_NOTIFY before closing events and before restoring the caller's TPL.
  Repeated calls are safe; the bitmap is cumulative protocol observation only.
  Even on a CloseEvent error, suppression remains latched.  Fatal telemetry
  remains permitted.  This interface is not a pre-DXE or exception API.

  @retval EFI_SUCCESS            The monitor is quiesced; all events closed.
  @retval EFI_INVALID_PARAMETER  This or ProtocolBitmap is invalid.
  @retval Other                  First CloseEvent error; suppression is active.
**/
typedef
EFI_STATUS
(EFIAPI *MOTO_G20_DXE_PROGRESS_QUIESCE)(
  IN MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL  *This,
  OUT UINT64                               *ProtocolBitmap
  );

struct _MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL {
  UINT64                         Revision;
  MOTO_G20_DXE_PROGRESS_QUIESCE   Quiesce;
};

extern EFI_GUID  gMotoG20DxeProgressControlProtocolGuid;

#endif
