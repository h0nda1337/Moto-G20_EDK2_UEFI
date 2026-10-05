/** @file
  DXE Core DebugLib: ASSERTs are observable; debug printing remains disabled.
**/

#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/MotoG20DxeTelemetryLib.h>

VOID
EFIAPI
DebugPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  ...
  )
{
}

VOID
EFIAPI
DebugVPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  IN VA_LIST      VaListMarker
  )
{
}

VOID
EFIAPI
DebugBPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  IN BASE_LIST    BaseListMarker
  )
{
}

VOID
EFIAPI
DebugAssert (
  IN CONST CHAR8  *FileName,
  IN UINTN        LineNumber,
  IN CONST CHAR8  *Description
  )
{
  MotoG20DxeTelemetryAssert (FileName, LineNumber, Description);
}

VOID *
EFIAPI
DebugClearMemory (
  OUT VOID  *Buffer,
  IN UINTN  Length
  )
{
  return Buffer;
}

BOOLEAN
EFIAPI
DebugAssertEnabled (
  VOID
  )
{
  return TRUE;
}

BOOLEAN
EFIAPI
DebugPrintEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugCodeEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugClearMemoryEnabled (
  VOID
  )
{
  return FALSE;
}

BOOLEAN
EFIAPI
DebugPrintLevelEnabled (
  IN CONST UINTN  ErrorLevel
  )
{
  return FALSE;
}
