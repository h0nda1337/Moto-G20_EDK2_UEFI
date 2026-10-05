STATIC EFI_STATUS EFIAPI ReadDiagnostic (MOTO_G20_USB_DIAGNOSTIC_PROTOCOL *This, MOTO_G20_USB_DIAGNOSTIC_STATUS *Status) {
  MOTO_G20_USB_PLATFORM_STAGE PlatformStage;
  EFI_STATUS RoleStatus;
  EFI_TPL OldTpl;
  if ((This != &mDiagnosticProtocol) || (Status == NULL)) { return EFI_INVALID_PARAMETER; }
  // Keep HC and both platform caches coherent against role/timer notifications.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (Status, &mDiagnostic, sizeof (*Status));
  Status->Revision = MOTO_G20_USB_DIAGNOSTIC_REVISION;
  Status->Size = sizeof (*Status);
  Status->HostReady = mUsb.Ready;
  Status->HcPublished = mUsb.Published;
  Status->PortConnected = mUsb.Connected;
  Status->PortEnabled = mUsb.Enabled;
  Status->SessionLimit = MOTO_G20_USB_SESSION_ATTEMPT_LIMIT;
  Status->RecoveryWaiting = mUsb.WaitingDetach;
  Status->FatalLatched = mUsb.Fatal;
  Status->SessionPending = mUsb.SessionPending;
  Status->SessionDeadlineUs = MOTO_G20_USB_PEER_DEADLINE_US;
  MotoG20UsbPlatformGetDiagnostic (&PlatformStage, &Status->PlatformStatus, &Status->TypecState, &Status->VbusPresent);
  Status->PlatformStage = (UINT32)PlatformStage;
  RoleStatus = MotoG20UsbPlatformGetRoleDiagnostic (&Status->Role);
  if (!EFI_ERROR (RoleStatus) &&
      ((Status->Role.Revision != MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION) ||
       (Status->Role.Size != sizeof (Status->Role)))) {
    RoleStatus = EFI_INCOMPATIBLE_VERSION;
  }
  if (EFI_ERROR (RoleStatus)) {
    // Do not leave partially returned fields marked valid after a getter failure.
    ZeroMem (&Status->Role, sizeof (Status->Role));
    Status->Role.Revision = MOTO_G20_USB_ROLE_DIAGNOSTIC_REVISION;
    Status->Role.Size = sizeof (Status->Role);
    Status->Role.RoleReadStatus = RoleStatus;
    Status->Role.SnapshotStatus = RoleStatus;
    Status->Role.CalibrationStatus = RoleStatus;
  }
  gBS->RestoreTPL (OldTpl);
  return RoleStatus;
}
