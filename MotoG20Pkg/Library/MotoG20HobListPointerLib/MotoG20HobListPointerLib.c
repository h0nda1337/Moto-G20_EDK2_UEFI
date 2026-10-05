#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Library/PrePiHobListPointerLib.h>

STATIC VOID *mMotoG20HobList;

VOID *
EFIAPI
PrePeiGetHobList (
  VOID
  )
{
  return mMotoG20HobList;
}

EFI_STATUS
EFIAPI
PrePeiSetHobList (
  IN VOID *HobList
  )
{
  mMotoG20HobList = HobList;
  return EFI_SUCCESS;
}
