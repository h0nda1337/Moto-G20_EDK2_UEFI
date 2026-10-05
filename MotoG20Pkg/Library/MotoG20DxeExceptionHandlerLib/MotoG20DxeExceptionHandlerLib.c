/** @file
  Fail-closed AArch64 DXE exception sink for the Moto G20.
**/

#include <Uefi.h>
#include <Protocol/DebugSupport.h>
#include <Library/DefaultExceptionHandlerLib.h>
#include <Library/MotoG20DxeTelemetryLib.h>

VOID
DefaultExceptionHandler (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  MotoG20DxeTelemetryException (ExceptionType, SystemContext);
}
