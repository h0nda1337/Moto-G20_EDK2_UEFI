/** @file
  Host-static, fail-closed platform contract for the Moto G20 USB2 host path.

  This interface deliberately contains no register access and publishes no
  protocol.  A future production driver must supply independently audited
  clock, regulator, syscon, Type-C, MUSB and protocol-install backends.
**/

#ifndef MOTO_G20_USB_PLATFORM_CONTRACT_H_
#define MOTO_G20_USB_PLATFORM_CONTRACT_H_

#include <Base.h>

#define MOTO_G20_MUSB_MMIO_BASE             0x5FFF0000ULL
#define MOTO_G20_MUSB_MMIO_SIZE             0x00002000U
#define MOTO_G20_MUSB_INTERRUPT             0x45U
#define MOTO_G20_MUSB_CORE_CLOCK_PROVIDER   0x04U
#define MOTO_G20_MUSB_CORE_CLOCK_ID         0x17U
#define MOTO_G20_USB2_PHY_VDD_MICROVOLTS    3300000U
#define MOTO_G20_USB2_HC_METHOD_COUNT       13U

typedef enum {
  MotoG20UsbStateReset = 0,
  MotoG20UsbStateRoleProvenHost,
  MotoG20UsbStateClockEnabled,
  MotoG20UsbStateVdd33Enabled,
  MotoG20UsbStatePhyHostSet,
  MotoG20UsbStatePhyInitialized,
  MotoG20UsbStateCoreSession,
  MotoG20UsbStateVbusEnabled,
  MotoG20UsbStatePortReady,
  MotoG20UsbStateProtocolPublished,
  MotoG20UsbStateMaximum
} MOTO_G20_USB_PLATFORM_STATE;

typedef enum {
  MotoG20UsbEventProveHostRole = 0,
  MotoG20UsbEventEnableCoreClock,
  MotoG20UsbEventEnableVdd33,
  MotoG20UsbEventSetPhyHost,
  MotoG20UsbEventInitializePhy,
  MotoG20UsbEventAssertCoreSession,
  MotoG20UsbEventEnableVbus,
  MotoG20UsbEventObservePortReady,
  MotoG20UsbEventPublishProtocol,
  MotoG20UsbEventMaximum
} MOTO_G20_USB_PLATFORM_EVENT;

typedef enum {
  MotoG20UsbContractSuccess = 0,
  MotoG20UsbContractInvalidParameter,
  MotoG20UsbContractStaticFactMismatch,
  MotoG20UsbContractOrderViolation,
  MotoG20UsbContractRoleUnproven,
  MotoG20UsbContractVbusUnproven,
  MotoG20UsbContractPortUnready,
  MotoG20UsbContractProtocolIncomplete,
  MotoG20UsbContractAlreadyPublished,
  MotoG20UsbContractMmioOutOfRange
} MOTO_G20_USB_CONTRACT_STATUS;

typedef struct {
  UINT64   MusbBase;
  UINT32   MusbSize;
  UINT32   MusbInterrupt;
  UINT32   CoreClockProvider;
  UINT32   CoreClockId;
  UINT32   PhyVddMicrovolts;
  UINT32   Usb2HcMethodCount;
  BOOLEAN  ControllerCompatibleProven;
  BOOLEAN  PhyCompatibleProven;
  BOOLEAN  TypecCompatibleProven;
  BOOLEAN  MultipointProven;
  BOOLEAN  TypecDrpConfigured;
  BOOLEAN  StockDrModePeripheral;
  BOOLEAN  HostModeOverrideSelected;
  BOOLEAN  TypecAttachedSource;
  BOOLEAN  HostRoleProvenanceFresh;
  BOOLEAN  VbusProviderProven;
  BOOLEAN  MusbADeviceObserved;
  BOOLEAN  VbusValid;
} MOTO_G20_USB_PLATFORM_FACTS;

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbValidateStaticFacts (
  IN CONST MOTO_G20_USB_PLATFORM_FACTS  *Facts
  );

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbAdvanceState (
  IN  MOTO_G20_USB_PLATFORM_STATE        CurrentState,
  IN  MOTO_G20_USB_PLATFORM_EVENT        Event,
  IN  CONST MOTO_G20_USB_PLATFORM_FACTS  *Facts,
  OUT MOTO_G20_USB_PLATFORM_STATE        *NextState
  );

MOTO_G20_USB_CONTRACT_STATUS
EFIAPI
MotoG20UsbValidateMmioAccess (
  IN UINT64  Address,
  IN UINTN   Width
  );

#endif
