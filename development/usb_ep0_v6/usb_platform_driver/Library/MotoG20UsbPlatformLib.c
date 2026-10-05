// SPDX-License-Identifier: GPL-2.0-only
// Board sequence derived from Motorola kernel-sprd 261390cdfff66efef9082bd70c1af0abd21d77f2.
// This is a new, bounded DXE adaptation. See SOURCE_MANIFEST.json and README.md.
#include "MotoG20UsbPlatformLib.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>

#define ADI       0x32100000U
#define PMIC      0x32120000U
#define AON       0x327D0000U
#define ANA       0x323B0000U
#define I2C       0x70600000U
#define AP_APB    0x71000000U
#define AP_CLK    0x20200000U
#define HWLOCK    0x327F0000U
#define MUSB      0x5FFF0000U
#define TYPEC_MODE   0x0384U
#define TYPEC_INTEN  0x038CU
#define TYPEC_STATUS 0x039CU
#define TYPEC_TRIM   0x03BCU
#define EIC_DATA     0x0280U
#define EIC_MASK     0x0284U
#define BC12         0x1BA0U
#define VDD_PD       0x19E8U
#define VDD_VOL      0x19ECU
#define CTL_BASE     (BIT8 | (15U << 16) | (4U << 12) | BIT2 | BIT1)
#define PERIOD_100MS 1000000ULL

typedef struct { UINTN Base; UINTN Size; } REGION;
STATIC CONST REGION mRegions[] = {
  { ADI, 0x1000 }, { PMIC, 0x2000 }, { AON, 0x3000 },
  { ANA, 0x1000 }, { I2C, 0x1000 }, { AP_APB, 0x3000 },
  { AP_CLK, 0x1000 }, { HWLOCK, 0x1000 }, { MUSB, 0x2000 }
};
typedef struct { UINTN Address; UINT32 Mask; UINT32 Original; } MMIO_SAVE;
STATIC MMIO_SAVE mSaved[20];
STATIC UINTN mSavedCount;
STATIC BOOLEAN mPrepared, mInfra, mRoleReady, mI2cReady;
STATIC BOOLEAN mActive, mBoostAttempted, mBcTouched, mVddSaved, mFault;
STATIC BOOLEAN mRoleSaved;
STATIC UINT16 mVddOriginal, mVddVolOriginal;
STATIC UINT16 mEicOriginal, mTrimOriginal, mModeOriginal, mIntenOriginal;
STATIC UINT16 mTypecState;
STATIC BOOLEAN mVbusPresent;
STATIC EFI_EVENT mTimer;
STATIC UINT64 mLastFeed;
STATIC BOOLEAN mFeedDue;
STATIC EFI_STATUS mLastStatus = EFI_NOT_READY;
STATIC MOTO_G20_USB_PLATFORM_STAGE mStage;
STATIC MOTO_G20_USB_ROLE_DIAGNOSTIC mRoleDiagnostic;
STATIC BOOLEAN mRoleDiagnosticInitialized, mRoleSnapshotTaken;
STATIC UINT16 mSnapshotRoleState;
STATIC EFI_STATUS mSnapshotRoleReadStatus;

STATIC VOID
InitializeRoleDiagnostic (VOID)
{
  if (mRoleDiagnosticInitialized) { return; }
  ZeroMem (&mRoleDiagnostic, sizeof (mRoleDiagnostic));
  mRoleDiagnostic.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION;
  mRoleDiagnostic.Size = sizeof (mRoleDiagnostic);
  mRoleDiagnostic.RoleReadStatus = EFI_NOT_READY;
  mRoleDiagnostic.SnapshotStatus = EFI_NOT_READY;
  mRoleDiagnostic.CalibrationStatus = EFI_NOT_READY;
  mRoleDiagnostic.LastStopStatus = EFI_NOT_READY;
  mRoleDiagnosticInitialized = TRUE;
}

STATIC EFI_STATUS
RememberStatus (MOTO_G20_USB_PLATFORM_STAGE Stage, EFI_STATUS Status)
{
  mStage = Stage;
  mLastStatus = Status;
  return Status;
}

STATIC EFI_STATUS
Wait32 (UINTN Address, UINT32 Mask, UINT32 Expected, UINTN Limit)
{
  UINTN Index;
  for (Index = 0; Index < Limit; ++Index) {
    if ((MmioRead32 (Address) & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Rmw32Checked (UINTN Address, UINT32 Mask, UINT32 Value)
{
  UINT32 Old;
  Old = MmioRead32 (Address);
  MmioWrite32 (Address, (Old & ~Mask) | (Value & Mask));
  return ((MmioRead32 (Address) & Mask) == (Value & Mask)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
SaveRmw32 (UINTN Address, UINT32 Mask, UINT32 Value)
{
  if (mSavedCount == ARRAY_SIZE (mSaved)) {
    return EFI_OUT_OF_RESOURCES;
  }
  mSaved[mSavedCount].Address = Address;
  mSaved[mSavedCount].Mask = Mask;
  mSaved[mSavedCount].Original = MmioRead32 (Address) & Mask;
  ++mSavedCount;
  return Rmw32Checked (Address, Mask, Value);
}

/* Gate aliases are +0x1000 SET and +0x2000 CLEAR, not base/+0x1000.
   Only owned bits are changed. Base registers are used for verification. */
STATIC EFI_STATUS
EnableGate (UINTN Address, UINT32 Mask)
{
  if (mSavedCount == ARRAY_SIZE (mSaved)) {
    return EFI_OUT_OF_RESOURCES;
  }
  mSaved[mSavedCount].Address = Address;
  mSaved[mSavedCount].Mask = Mask;
  mSaved[mSavedCount].Original = MmioRead32 (Address) & Mask;
  ++mSavedCount;
  MmioWrite32 (Address + 0x1000, Mask);
  return ((MmioRead32 (Address) & Mask) == Mask) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
AdiRead (UINT16 Reg, UINT16 *Value)
{
  UINT32 Readback;
  EFI_STATUS Status;
  if ((Value == NULL) || ((Reg & 3U) != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  MmioWrite32 (ADI + 0x28, PMIC + Reg);
  Status = Wait32 (ADI + 0x2C, BIT31, 0, 2000);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Readback = MmioRead32 (ADI + 0x2C);
  if (((Readback >> 16) & 0x7FFFU) != (((PMIC + Reg) & 0x1FFFFU) >> 2)) {
    return EFI_DEVICE_ERROR;
  }
  *Value = (UINT16)Readback;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
AdiWrite (UINT16 Reg, UINT16 Value)
{
  EFI_STATUS Status;
  if ((Reg & 3U) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  Status = Wait32 (ADI + 0x30, BIT10, BIT10, 1000);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Wait32 (ADI + 0x30, BIT11, 0, 1000);
  if (EFI_ERROR (Status)) { return Status; }
  MmioWrite32 (PMIC + Reg, Value);
  return Wait32 (ADI + 0x30, BIT10, BIT10, 1000);
}

STATIC EFI_STATUS
AdiUpdate (UINT16 Reg, UINT16 Mask, UINT16 Value)
{
  UINT16 Old, Actual;
  EFI_STATUS Status;
  Status = AdiRead (Reg, &Old);
  if (EFI_ERROR (Status)) { return Status; }
  Status = AdiWrite (Reg, (UINT16)((Old & ~Mask) | (Value & Mask)));
  if (EFI_ERROR (Status)) { return Status; }
  Status = AdiRead (Reg, &Actual);
  if (EFI_ERROR (Status)) { return Status; }
  return ((Actual & Mask) == (Value & Mask)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
SnapshotTypec (VOID)
{
  // Register offsets come from pinned sc27xx_typec.c. These are reads only:
  // in particular, do not write or acknowledge INT_CLR (base +0x10).
  STATIC CONST UINT16 Registers[] = { 0x0380, 0x0384, 0x038C, 0x0394, 0x0398, 0x039C, 0x03BC };
  UINT16 *Fields[] = { &mRoleDiagnostic.En, &mRoleDiagnostic.Mode,
    &mRoleDiagnostic.IntEn, &mRoleDiagnostic.IntRaw, &mRoleDiagnostic.IntMask,
    &mRoleDiagnostic.TypecStatus, &mRoleDiagnostic.Trim };
  UINTN Index;
  UINT16 Value;
  EFI_STATUS Status, First;
  First = EFI_SUCCESS;
  ++mRoleDiagnostic.SnapshotAttempts;
  mRoleDiagnostic.ValidMask = 0;
  mRoleDiagnostic.ErrorMask = 0;
  for (Index = 0; Index < ARRAY_SIZE (Registers); ++Index) {
    *Fields[Index] = 0;
    Status = AdiRead (Registers[Index], &Value);
    if (EFI_ERROR (Status)) {
      mRoleDiagnostic.ErrorMask |= (UINT32)(1U << Index);
      if (!EFI_ERROR (First)) { First = Status; }
    } else {
      *Fields[Index] = Value;
      mRoleDiagnostic.ValidMask |= (UINT32)(1U << Index);
    }
  }
  if (EFI_ERROR (First)) { ++mRoleDiagnostic.SnapshotFailures; }
  mRoleDiagnostic.SnapshotStatus = First;
  return First;
}

STATIC VOID
CacheRoleSample (EFI_STATUS Status, BOOLEAN StateValid)
{
  BOOLEAN Changed;
  if (StateValid) {
    mRoleDiagnostic.SeenStates |= (UINT32)(1U << mTypecState);
    if (mTypecState == 5) { ++mRoleDiagnostic.SourceSeen; }
  }
  Changed = !mRoleSnapshotTaken || (Status != mSnapshotRoleReadStatus) ||
            (StateValid && (mTypecState != mSnapshotRoleState));
  mRoleDiagnostic.RoleReadStatus = Status;
  if (Changed) {
    // At most one sweep per state/error transition. A failed diagnostic sweep
    // is cached too, so getters and steady 1ms polling cannot retry it forever.
    (VOID)SnapshotTypec ();
    mRoleSnapshotTaken = TRUE;
    mSnapshotRoleReadStatus = Status;
    if (StateValid) { mSnapshotRoleState = mTypecState; }
  }
}

STATIC EFI_STATUS
ReadRole (VOID)
{
  UINT16 Value;
  EFI_STATUS Status;
  ++mRoleDiagnostic.Samples;
  Status = AdiRead (TYPEC_STATUS, &Value);
  if (EFI_ERROR (Status)) { CacheRoleSample (Status, FALSE); return Status; }
  mTypecState = Value & 0x1FU;
  Status = AdiRead (EIC_DATA, &Value);
  if (EFI_ERROR (Status)) { CacheRoleSample (Status, TRUE); return Status; }
  mVbusPresent = (Value & BIT0) != 0;
  CacheRoleSample (EFI_SUCCESS, TRUE);
  return EFI_SUCCESS;
}

/* Dedicated byte transfers, bounded to 2 bytes. A SMBus read consists of
   register-index write with STOP suppressed, then a repeated-START read. */
STATIC EFI_STATUS
I2cMessage (BOOLEAN Read, UINT8 *Bytes, UINTN Count, BOOLEAN Stop)
{
  EFI_STATUS Status;
  UINT32 Control, Value;
  UINTN Index;
  if ((Count == 0) || (Count > 2) || (Bytes == NULL)) { return EFI_INVALID_PARAMETER; }
  Status = Wait32 (I2C + 0x14, BIT0, 0, 2000);
  if (EFI_ERROR (Status)) { return Status; }
  MmioWrite32 (I2C + 0x2C, 1);
  MmioWrite32 (I2C + 0x14, MmioRead32 (I2C + 0x14) & ~(BIT1 | BIT2));
  MmioWrite32 (I2C + 0x04, 0x6BU << 1);
  MmioWrite32 (I2C + 0x08, (UINT32)Count);
  Control = CTL_BASE | (Read ? BIT3 : 0) | (Stop ? 0 : BIT20);
  MmioWrite32 (I2C, Control);
  if (!Read) {
    for (Index = 0; Index < Count; ++Index) { MmioWrite8 (I2C + 0x10, Bytes[Index]); }
  }
  MmioWrite32 (I2C, Control | BIT0);
  /* Completion must be observed; an initially clear BUSY is not success. */
  Status = EFI_TIMEOUT;
  for (Index = 0; Index < 2000; ++Index) {
    Value = MmioRead32 (I2C + 0x14);
    if ((Value & BIT1) != 0) { Status = EFI_NO_RESPONSE; break; }
    if (((Value & BIT2) != 0) && ((Value & BIT0) == 0)) { Status = EFI_SUCCESS; break; }
    MicroSecondDelay (1);
  }
  if (!EFI_ERROR (Status) && Read) {
    if ((MmioRead32 (I2C + 0x14) & BIT3) != 0) {
      Status = EFI_DEVICE_ERROR;
    } else {
      for (Index = 0; Index < Count; ++Index) { Bytes[Index] = MmioRead8 (I2C + 0x0C); }
    }
  }
  MmioWrite32 (I2C, Control);
  MmioWrite32 (I2C + 0x14, MmioRead32 (I2C + 0x14) & ~(BIT1 | BIT2));
  if (EFI_ERROR (Status)) {
    MmioWrite32 (I2C, CTL_BASE); // Release no-STOP selection on failure.
    MmioWrite32 (I2C + 0x2C, 1);
  }
  return Status;
}

STATIC EFI_STATUS
BqWrite (UINT8 Reg, UINT8 Value)
{
  UINT8 Bytes[2];
  Bytes[0] = Reg; Bytes[1] = Value;
  return I2cMessage (FALSE, Bytes, 2, TRUE);
}

STATIC EFI_STATUS
BqRead (UINT8 Reg, UINT8 *Value)
{
  EFI_STATUS Status;
  Status = I2cMessage (FALSE, &Reg, 1, FALSE);
  return EFI_ERROR (Status) ? Status : I2cMessage (TRUE, Value, 1, TRUE);
}

STATIC EFI_STATUS
EfusePoll (UINT16 Mask)
{
  UINTN Index;
  UINT16 Value;
  EFI_STATUS Status;
  for (Index = 0; Index < 300; ++Index) {
    Status = AdiRead (0x0314, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    if ((Value & Mask) != 0) { return EFI_SUCCESS; }
    MicroSecondDelay (10000);
  }
  return EFI_TIMEOUT;
}

/* Read calibration block 13 (DT cc_calib byte offset0x1a), never program.
   Serialize with PMIC eFuse hardware lock12. Restore only the enable bit. */
STATIC EFI_STATUS
CalibrateTypec (VOID)
{
  UINTN Index;
  UINT16 Enable, Mode, Data;
  EFI_STATUS Status, Cleanup;
  mRoleDiagnostic.EfuseValid = FALSE;
  mRoleDiagnostic.TrimApplied = FALSE;
  mRoleDiagnostic.CalibrationStatus = EFI_NOT_READY;
  Status = EFI_TIMEOUT;
  for (Index = 0; Index < 5000; ++Index) {
    if (MmioRead32 (HWLOCK + 0x830) == 0) { Status = EFI_SUCCESS; break; }
    MicroSecondDelay (1);
  }
  if (EFI_ERROR (Status)) { mRoleDiagnostic.CalibrationStatus = Status; return Status; }
  Status = AdiRead (0x1808, &Enable);
  if (EFI_ERROR (Status)) { goto Unlock; }
  Status = AdiUpdate (0x1808, BIT6, BIT6);
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = EfusePoll (BIT2);
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = AdiRead (0x0310, &Mode);
  if (EFI_ERROR (Status)) { goto Restore; }
  if ((Mode & BIT0) != 0) { Status = EFI_NOT_READY; goto Restore; }
  Status = AdiWrite (0x030C, 13);
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = AdiWrite (0x0310, (UINT16)((Mode & ~(BIT0 | BIT2)) | BIT1));
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = EfusePoll (BIT4);
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = AdiRead (0x0304, &Data);
  if (EFI_ERROR (Status)) { goto Restore; }
  mRoleDiagnostic.EfuseData = Data;
  mRoleDiagnostic.EfuseValid = TRUE;
  mRoleDiagnostic.RequestedTrim = (UINT16)(((Data >> 5) & 0x1F) | ((Data & 0x1F) << 5));
  Status = AdiWrite (0x0310, (UINT16)((Mode & ~(BIT0 | BIT1)) | BIT2));
  if (EFI_ERROR (Status)) { goto Restore; }
  Status = AdiUpdate (TYPEC_TRIM, 0x3FF, (UINT16)(((Data >> 5) & 0x1F) | ((Data & 0x1F) << 5)));
  mRoleDiagnostic.TrimApplied = !EFI_ERROR (Status);
Restore:
  Cleanup = AdiUpdate (0x1808, BIT6, Enable & BIT6);
  if (EFI_ERROR (Cleanup)) { Status = Cleanup; mFault = TRUE; }
Unlock:
  MmioWrite32 (HWLOCK + 0x830, 0x55AA10C5);
  mRoleDiagnostic.CalibrationStatus = Status;
  return Status;
}

STATIC EFI_STATUS
PrepareRole (VOID)
{
  EFI_STATUS Status;
  if (mRoleReady) { return EFI_SUCCESS; }
  if (!mInfra) {
    Status = EnableGate (AON + 8, BIT9); // ADI.
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageAdi, Status); }
    Status = EnableGate (AON + 4, BIT10); // PMIC eFuse hwspinlock.
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageEfuse, Status); }
    mInfra = TRUE;
  }
  if (!mRoleSaved) {
    Status = AdiRead (EIC_MASK, &mEicOriginal);
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageAdi, Status); }
    Status = AdiRead (TYPEC_TRIM, &mTrimOriginal);
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageTypec, Status); }
    Status = AdiRead (TYPEC_MODE, &mModeOriginal);
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageTypec, Status); }
    Status = AdiRead (TYPEC_INTEN, &mIntenOriginal);
    if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageTypec, Status); }
    mRoleSaved = TRUE;
  }
  Status = AdiUpdate (EIC_MASK, BIT0, BIT0);
  if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageAdi, Status); }
  Status = CalibrateTypec ();
  if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageEfuse, Status); }
  Status = AdiUpdate (TYPEC_MODE, 3, 2); // DRP. Never force source.
  if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageTypec, Status); }
  Status = AdiUpdate (TYPEC_INTEN, 3, 3); // Source driver enables detection here.
  if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageTypec, Status); }
  mRoleReady = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PrepareI2c (VOID)
{
  EFI_STATUS Status;
  UINT8 Probe;
  Status = SaveRmw32 (AP_CLK + 0x40, 0x703, 0); // ext-26MHz, divider1.
  if (EFI_ERROR (Status)) { return Status; }
  Status = EnableGate (AP_APB, BIT12);
  if (EFI_ERROR (Status)) { return Status; }
  MmioWrite32 (I2C, BIT8);
  MmioWrite32 (I2C + 0x20, 0x000C0012);
  MmioWrite32 (I2C + 0x24, 0);
  MmioWrite32 (I2C + 0x28, 15);
  MmioWrite32 (I2C + 0x2C, 1);
  MmioWrite32 (I2C + 0x14, MmioRead32 (I2C + 0x14) & ~(BIT1 | BIT2));
  MmioWrite32 (I2C, CTL_BASE);
  mI2cReady = TRUE;
  /* A real ACKed repeated-START read is required before enabling boost. */
  return BqRead (0x0B, &Probe);
}

STATIC EFI_STATUS
PreparePhy (VOID)
{
  EFI_STATUS Status;
  Status = AdiRead (VDD_PD, &mVddOriginal);
  if (EFI_ERROR (Status)) { return Status; }
  Status = AdiRead (VDD_VOL, &mVddVolOriginal);
  if (EFI_ERROR (Status)) { return Status; }
  mVddSaved = TRUE;
  Status = AdiWrite (0x1BD0, 0x6E7F); // SC2730 regulator write key.
  if (EFI_ERROR (Status)) { return Status; }
  Status = AdiUpdate (VDD_VOL, 0xFF, 210); // 1.2V + 210*10mV =3.3V.
  if (EFI_ERROR (Status)) { return Status; }
  Status = AdiUpdate (VDD_PD, BIT0, 0);
  if (EFI_ERROR (Status)) { return Status; }
  MicroSecondDelay (200);
  Status = EnableGate (AON + 4, BIT8 | BIT12); // UTMI + analog.
  if (EFI_ERROR (Status)) { return Status; }
  Status = EnableGate (AON + 0x138, BIT12 | BIT10);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x70, 1, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x5C, 0x18, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (AON + 0x208, BIT30 | BIT3, BIT30); // 16bit UTMI, host ID.
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x58, BIT28 | 0xFFFF, BIT28 | 0x200);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x74, 6, 6);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x60, 0x18, 0x18);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (ANA + 0x64, 0x07F80000, 0x06A00000);
  if (EFI_ERROR (Status)) { return Status; }
  Status = SaveRmw32 (AON + 0x10, BIT8 | BIT9, BIT8 | BIT9);
  if (EFI_ERROR (Status)) { return Status; }
  MicroSecondDelay (25000);
  return Rmw32Checked (AON + 0x10, BIT8 | BIT9, 0);
}

STATIC EFI_STATUS
RestoreSavedTo (UINTN Keep)
{
  EFI_STATUS Status;
  MMIO_SAVE *Save;
  while (mSavedCount > Keep) {
    Save = &mSaved[mSavedCount - 1];
    Status = Rmw32Checked (Save->Address, Save->Mask, Save->Original);
    if (EFI_ERROR (Status)) {
      // A failed restoration remains owned, including its original value.
      // Keep older clock/gate prerequisites live so the retry can access it.
      // Stop/Shutdown must retry it before reporting complete cleanup.
      return Status;
    }
    --mSavedCount;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
DisablePower (VOID)
{
  EFI_STATUS Status, First;
  BOOLEAN BoostOff;
  First = EFI_SUCCESS;
  ++mRoleDiagnostic.StopAttempts;
  BoostOff = !mBoostAttempted;
  mActive = FALSE;
  if (mTimer != NULL) {
    Status = gBS->SetTimer (mTimer, TimerCancel, 0);
    if (EFI_ERROR (Status)) { First = Status; }
  }
  if (mBoostAttempted && mI2cReady) {
    Status = BqWrite (1, 0x5A);
    if (EFI_ERROR (Status)) {
      if (!EFI_ERROR (First)) { First = Status; }
    }
    else { BoostOff = TRUE; }
  } else if (mBoostAttempted && !EFI_ERROR (First)) {
    First = EFI_DEVICE_ERROR; // Pending source ownership without its OFF transport.
  }
  // Always restore BC1.2, including after a failed I2C disable.
  if (mBcTouched) {
    Status = AdiUpdate (BC12, 1, 0);
    if (EFI_ERROR (Status) && !EFI_ERROR (First)) { First = Status; }
  }
  // Keep I2C mux/gate available if the boost-OFF transaction failed. A later
  // Stop can try again; Init stays fault-latched and cannot re-enable boost.
  Status = RestoreSavedTo (BoostOff ? 2U : 4U);
  if (EFI_ERROR (Status) && !EFI_ERROR (First)) { First = Status; }
  // Keep the PHY supply while any register restoration still needs retry.
  if (mVddSaved && !EFI_ERROR (Status)) {
    Status = AdiUpdate (VDD_PD, BIT0, mVddOriginal & BIT0);
    if (EFI_ERROR (Status) && !EFI_ERROR (First)) { First = Status; }
    Status = AdiUpdate (VDD_VOL, 0xFF, mVddVolOriginal & 0xFF);
    if (EFI_ERROR (Status) && !EFI_ERROR (First)) { First = Status; }
  }
  if (BoostOff) { mI2cReady = FALSE; mBoostAttempted = FALSE; }
  if (!EFI_ERROR (First)) { mBcTouched = FALSE; mVddSaved = FALSE; }
  if (EFI_ERROR (First)) { mFault = TRUE; RememberStatus (MotoUsbStageRollback, First); }
  else if (!mFault && (mStage == MotoUsbStageRunning)) {
    // This is an inactive software state, not a new physical EIC measurement.
    RememberStatus (MotoUsbStageStopped, EFI_NOT_READY);
  }
  mRoleDiagnostic.LastStopStatus = First;
  return First;
}

STATIC VOID EFIAPI
Monitor (EFI_EVENT Event, VOID *Context)
{
  BOOLEAN Active;
  (VOID)Event; (VOID)Context;
  MotoG20UsbPlatformPollRole (&Active);
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformPrepare (VOID)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS Status;
  UINTN Index;
  UINT64 Address, End, Length;
  InitializeRoleDiagnostic ();
  if (mPrepared) { return EFI_SUCCESS; }
  for (Index = 0; Index < ARRAY_SIZE (mRegions); ++Index) {
    Address = mRegions[Index].Base;
    End = Address + mRegions[Index].Size;
    while (Address < End) {
      Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
      if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageMapping, Status); }
      if ((Desc.Length == 0) || (Desc.BaseAddress > Address) ||
          (Desc.BaseAddress + Desc.Length <= Address)) {
        return RememberStatus (MotoUsbStageMapping, EFI_COMPROMISED_DATA);
      }
      Length = MIN (End, Desc.BaseAddress + Desc.Length) - Address;
      if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
        Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Address, Length, EFI_MEMORY_UC);
        if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageMapping, Status); }
      } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
        return RememberStatus (MotoUsbStageMapping, EFI_ACCESS_DENIED);
      }
      Status = gDS->SetMemorySpaceAttributes (Address, Length, EFI_MEMORY_UC);
      if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageMapping, Status); }
      Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
      if (EFI_ERROR (Status) || (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
          ((Desc.Attributes & (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB)) != EFI_MEMORY_UC)) {
        return RememberStatus (MotoUsbStageMapping, EFI_DEVICE_ERROR);
      }
      Address += Length;
    }
  }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Monitor, NULL, &mTimer);
  if (EFI_ERROR (Status)) { return RememberStatus (MotoUsbStageMapping, Status); }
  mPrepared = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformInit (UINTN *ControllerBase)
{
  EFI_STATUS Status, Cleanup;
  EFI_TPL OldTpl;
  UINTN Index;
  if (ControllerBase == NULL) { return EFI_INVALID_PARAMETER; }
  *ControllerBase = 0;
  if (!mPrepared) { return RememberStatus (MotoUsbStageMapping, EFI_NOT_READY); }
  if (mFault) { return mLastStatus; }
  if (mActive) { *ControllerBase = MUSB; return EFI_SUCCESS; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = PrepareRole ();
  if (EFI_ERROR (Status)) {
    // Partial transport setup must not append duplicate journal entries.
    // Only the fully initialized role detector is retried after cable changes.
    if (mInfra && !mRoleSnapshotTaken) {
      (VOID)SnapshotTypec ();
      mRoleSnapshotTaken = TRUE;
      mSnapshotRoleReadStatus = Status;
    }
    mFault = TRUE;
    goto End;
  }
  Status = ReadRole ();
  if (EFI_ERROR (Status)) { mFault = TRUE; RememberStatus (MotoUsbStageTypec, Status); goto End; }
  // State5 alone is host source. Sink, audio/debug/accessory and DRP wait fail closed.
  if ((mTypecState != 5) || mVbusPresent) {
    Status = RememberStatus (mVbusPresent ? MotoUsbStageExternalVbus : MotoUsbStageTypec, EFI_NOT_READY);
    goto End;
  }
  MicroSecondDelay (20000);
  Status = ReadRole ();
  if (EFI_ERROR (Status) || (mTypecState != 5) || mVbusPresent) {
    if (EFI_ERROR (Status)) { mFault = TRUE; }
    else { Status = EFI_NOT_READY; }
    RememberStatus (MotoUsbStageExternalVbus, Status); goto End;
  }
  Status = PrepareI2c ();
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStageI2c, Status); goto Fail; }
  Status = PreparePhy ();
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStagePhy, Status); goto Fail; }
  // Recheck after slow work and immediately before the first source-enable write.
  Status = ReadRole ();
  if (EFI_ERROR (Status) || (mTypecState != 5) || mVbusPresent) {
    if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
    RememberStatus (MotoUsbStageExternalVbus, Status); goto Fail;
  }
  mBcTouched = TRUE;
  Status = AdiUpdate (BC12, 1, 1);
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStageBoost, Status); goto Fail; }
  mBoostAttempted = TRUE; // Set before uncertain write so rollback always tries OFF.
  ++mRoleDiagnostic.BoostAttempts;
  Status = BqWrite (1, 0x3A);
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStageBoost, Status); goto Fail; }
  for (Index = 0; Index < 50; ++Index) {
    MicroSecondDelay (10000);
    Status = ReadRole ();
    if (EFI_ERROR (Status) || (mTypecState != 5)) {
      if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
      RememberStatus (MotoUsbStageTypec, Status); goto Fail;
    }
    if (Index == 4) {
      Status = BqWrite (1, 0x7A); // First watchdog feed at50ms, as board driver.
      if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStageBoost, Status); goto Fail; }
    }
  }
  if (!mVbusPresent) { Status = RememberStatus (MotoUsbStageVbusSense, EFI_NO_RESPONSE); goto Fail; }
  // These PHY valid bits are software overrides, not a VBUS measurement.
  // Only set them after the independent EIC input became high.
  Status = SaveRmw32 (AON + 0x204, BIT24, BIT24);
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStagePhy, Status); goto Fail; }
  Status = SaveRmw32 (ANA + 0x58, BIT16, BIT16);
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStagePhy, Status); goto Fail; }
  MicroSecondDelay (150000); // Board glue waits150ms before host SESSION.
  Status = ReadRole ();
  if (EFI_ERROR (Status) || (mTypecState != 5) || !mVbusPresent) {
    if (!EFI_ERROR (Status)) { Status = (mTypecState != 5) ? EFI_NOT_READY : EFI_NO_RESPONSE; }
    RememberStatus (MotoUsbStageVbusSense, Status); goto Fail;
  }
  mLastFeed = GetPerformanceCounter ();
  mFeedDue = FALSE;
  mActive = TRUE;
  Status = gBS->SetTimer (mTimer, TimerPeriodic, PERIOD_100MS);
  if (EFI_ERROR (Status)) { RememberStatus (MotoUsbStageBoost, Status); goto Fail; }
  *ControllerBase = MUSB;
  Status = RememberStatus (MotoUsbStageRunning, EFI_SUCCESS);
  goto End;
Fail:
  // Cable/role changes may be retried after successful OFF. Transport,
  // register and source-voltage faults require a new driver lifetime: do not
  // repeatedly pulse boost while a hardware failure remains present.
  if (Status != EFI_NOT_READY) { mFault = TRUE; }
  Cleanup = DisablePower ();
  if (EFI_ERROR (Cleanup)) { Status = Cleanup; }
End:
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformPollRole (BOOLEAN *HostActive)
{
  EFI_STATUS Status, Cleanup;
  EFI_TPL OldTpl;
  UINT64 Now;
  if (HostActive == NULL) { return EFI_INVALID_PARAMETER; }
  *HostActive = FALSE;
  if (!mActive) { return mFault ? mLastStatus : EFI_NOT_READY; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = ReadRole ();
  if (!EFI_ERROR (Status)) {
    if (mTypecState != 5) { Status = EFI_NOT_READY; }
    else if (!mVbusPresent) { Status = EFI_NO_RESPONSE; }
  }
  if (!EFI_ERROR (Status)) {
    Now = GetPerformanceCounter ();
    // Board TimerLib counter is ascending; counter reversal fails closed.
    if (Now < mLastFeed) { Status = EFI_DEVICE_ERROR; }
    else if (GetTimeInNanoSecond (Now - mLastFeed) >= 15000000000ULL) { mFeedDue = TRUE; }
    if (!EFI_ERROR (Status) && mFeedDue) {
      Status = BqWrite (1, 0x7A);
      if (!EFI_ERROR (Status)) { mLastFeed = Now; mFeedDue = FALSE; }
    }
  }
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_READY) { mFault = TRUE; }
    RememberStatus (MotoUsbStageVbusSense, Status);
    Cleanup = DisablePower ();
    if (EFI_ERROR (Cleanup)) { Status = Cleanup; }
  } else {
    *HostActive = TRUE;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformQuiesceForRetry (VOID)
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!mPrepared) { Status = EFI_NOT_READY; }
  else {
    // Restore only source/PHY ownership. Keep the calibrated detector and ADI
    // prerequisites available for fresh cable observations without rearming.
    Status = DisablePower ();
    if (!EFI_ERROR (Status) && mFault) { Status = mLastStatus; }
    if (!EFI_ERROR (Status) && (!mInfra || !mRoleReady)) { Status = EFI_NOT_READY; }
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformDetectRole (UINT16 *TypecState, BOOLEAN *VbusPresent)
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if ((TypecState == NULL) || (VbusPresent == NULL)) { return EFI_INVALID_PARAMETER; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mFault) { Status = mLastStatus; }
  else if (!mPrepared || !mInfra || !mRoleReady || mActive || mBoostAttempted) {
    Status = EFI_NOT_READY;
  } else {
    Status = ReadRole ();
    if (EFI_ERROR (Status)) {
      mFault = TRUE;
      RememberStatus (MotoUsbStageTypec, Status);
    } else {
      // Publish a pair only after both reads succeed. Never invent detached
      // state or absent VBUS after a transport error.
      *TypecState = mTypecState;
      *VbusPresent = mVbusPresent;
    }
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformStop (VOID)
{
  EFI_STATUS Status, Cleanup;
  EFI_TPL OldTpl;
  if (!mPrepared) { return EFI_SUCCESS; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = DisablePower ();
  if (!EFI_ERROR (Status) && mRoleSaved) {
    Cleanup = AdiUpdate (TYPEC_INTEN, 3, mIntenOriginal & 3);
    if (EFI_ERROR (Cleanup)) { Status = Cleanup; }
    Cleanup = AdiUpdate (TYPEC_MODE, 3, mModeOriginal & 3);
    if (EFI_ERROR (Cleanup) && !EFI_ERROR (Status)) { Status = Cleanup; }
    Cleanup = AdiUpdate (TYPEC_TRIM, 0x3FF, mTrimOriginal & 0x3FF);
    if (EFI_ERROR (Cleanup) && !EFI_ERROR (Status)) { Status = Cleanup; }
    Cleanup = AdiUpdate (EIC_MASK, BIT0, mEicOriginal & BIT0);
    if (EFI_ERROR (Cleanup) && !EFI_ERROR (Status)) { Status = Cleanup; }
    if (!EFI_ERROR (Status)) { mRoleSaved = FALSE; mRoleReady = FALSE; }
  }
  if (!EFI_ERROR (Status)) {
    Status = RestoreSavedTo (0);
    if (!EFI_ERROR (Status)) { mInfra = FALSE; }
  }
  if (EFI_ERROR (Status)) { mFault = TRUE; RememberStatus (MotoUsbStageRollback, Status); }
  mRoleDiagnostic.LastStopStatus = Status;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

VOID EFIAPI
MotoG20UsbPlatformGetDiagnostic (MOTO_G20_USB_PLATFORM_STAGE *Stage, EFI_STATUS *Status,
                               UINT16 *TypecState, BOOLEAN *VbusPresent)
{
  if (Stage != NULL) { *Stage = mStage; }
  if (Status != NULL) { *Status = mLastStatus; }
  if (TypecState != NULL) { *TypecState = mTypecState; }
  if (VbusPresent != NULL) { *VbusPresent = mVbusPresent; }
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformGetRoleDiagnostic (MOTO_G20_USB_ROLE_DIAGNOSTIC *Role)
{
  EFI_TPL OldTpl;
  if (Role == NULL) { return EFI_INVALID_PARAMETER; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  InitializeRoleDiagnostic ();
  CopyMem (Role, &mRoleDiagnostic, sizeof (*Role));
  Role->DriverActive = mActive;
  Role->BoostRequested = mBoostAttempted;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
MotoG20UsbPlatformShutdown (VOID)
{
  EFI_STATUS Status;
  Status = MotoG20UsbPlatformStop ();
  if (EFI_ERROR (Status)) { return Status; }
  if (mTimer != NULL) {
    Status = gBS->CloseEvent (mTimer);
    if (EFI_ERROR (Status)) { return Status; }
    mTimer = NULL;
  }
  mPrepared = FALSE;
  return EFI_SUCCESS;
}
