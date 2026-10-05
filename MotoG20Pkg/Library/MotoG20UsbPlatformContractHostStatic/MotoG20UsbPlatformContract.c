/** @file
  Pure state and bounds validation for a future Moto G20 USB2 host driver.

  No function in this file accesses hardware, changes Type-C role, enables a
  regulator, installs an EFI protocol, or touches the device.  The stock DT
  says peripheral; host mode therefore requires fresh SC2730 ATTACHED_SRC
  provenance plus an explicit UEFI host-mode override.  VBUS remains a
  separately proven prerequisite.
**/

#include <Library/MotoG20UsbPlatformContract.h>

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbValidateStaticFacts (
  IN CONST MOTO_G20_USB_PLATFORM_FACTS  *Facts
  )
{
  if (Facts == NULL) {
    return MotoG20UsbContractInvalidParameter;
  }

  if ((Facts->MusbBase != MOTO_G20_MUSB_MMIO_BASE) ||
      (Facts->MusbSize != MOTO_G20_MUSB_MMIO_SIZE) ||
      (Facts->MusbInterrupt != MOTO_G20_MUSB_INTERRUPT) ||
      (Facts->CoreClockProvider != MOTO_G20_MUSB_CORE_CLOCK_PROVIDER) ||
      (Facts->CoreClockId != MOTO_G20_MUSB_CORE_CLOCK_ID) ||
      (Facts->PhyVddMicrovolts != MOTO_G20_USB2_PHY_VDD_MICROVOLTS) ||
      !Facts->ControllerCompatibleProven ||
      !Facts->PhyCompatibleProven ||
      !Facts->TypecCompatibleProven ||
      !Facts->MultipointProven ||
      !Facts->TypecDrpConfigured ||
      !Facts->StockDrModePeripheral)
  {
    return MotoG20UsbContractStaticFactMismatch;
  }

  return MotoG20UsbContractSuccess;
}

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbAdvanceState (
  IN  MOTO_G20_USB_PLATFORM_STATE        CurrentState,
  IN  MOTO_G20_USB_PLATFORM_EVENT        Event,
  IN  CONST MOTO_G20_USB_PLATFORM_FACTS  *Facts,
  OUT MOTO_G20_USB_PLATFORM_STATE        *NextState
  )
{
  MOTO_G20_USB_CONTRACT_STATUS  Status;

  if ((Facts == NULL) || (NextState == NULL) ||
      (CurrentState >= MotoG20UsbStateMaximum) ||
      (Event >= MotoG20UsbEventMaximum))
  {
    return MotoG20UsbContractInvalidParameter;
  }

  Status = MotoG20UsbValidateStaticFacts (Facts);
  if (Status != MotoG20UsbContractSuccess) {
    return Status;
  }

  if (CurrentState == MotoG20UsbStateProtocolPublished) {
    return MotoG20UsbContractAlreadyPublished;
  }

  // The state and event enumerations intentionally advance in lockstep.
  if ((UINTN)Event != (UINTN)CurrentState) {
    return MotoG20UsbContractOrderViolation;
  }

  switch (Event) {
    case MotoG20UsbEventProveHostRole:
      if (!Facts->HostModeOverrideSelected ||
          !Facts->TypecAttachedSource ||
          !Facts->HostRoleProvenanceFresh)
      {
        return MotoG20UsbContractRoleUnproven;
      }
      break;

    case MotoG20UsbEventEnableVbus:
      if (!Facts->VbusProviderProven || !Facts->MusbADeviceObserved) {
        return MotoG20UsbContractVbusUnproven;
      }
      break;

    case MotoG20UsbEventObservePortReady:
      if (!Facts->VbusValid) {
        return MotoG20UsbContractPortUnready;
      }
      break;

    case MotoG20UsbEventPublishProtocol:
      if (Facts->Usb2HcMethodCount != MOTO_G20_USB2_HC_METHOD_COUNT) {
        return MotoG20UsbContractProtocolIncomplete;
      }
      break;

    default:
      break;
  }

  *NextState = (MOTO_G20_USB_PLATFORM_STATE)((UINTN)CurrentState + 1U);
  return MotoG20UsbContractSuccess;
}

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbValidateMmioAccess (
  IN UINT64  Address,
  IN UINTN   Width
  )
{
  UINT64  Limit;

  if ((Width != 1U) && (Width != 2U) && (Width != 4U)) {
    return MotoG20UsbContractMmioOutOfRange;
  }

  Limit = MOTO_G20_MUSB_MMIO_BASE + MOTO_G20_MUSB_MMIO_SIZE;
  if ((Address < MOTO_G20_MUSB_MMIO_BASE) ||
      (Address > (Limit - Width)) ||
      ((Address & (Width - 1U)) != 0))
  {
    return MotoG20UsbContractMmioOutOfRange;
  }

  return MotoG20UsbContractSuccess;
}
