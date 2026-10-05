# Confirmed static console-path incompatibility

The GOP's published handle cannot match the path which BmConsole derives from that same GOP path. This is a source-proven incompatibility; it is not a runtime observation proving it was the only obstacle in the last boot.

All source paths below are relative to `C:\MOTO_G20_WOA_LAB\05_EDK2`.

## Exact chain

1. `MotoG20Pkg/Drivers/MotoG20FramebufferGopDxe/MotoG20FramebufferGopDxe.c:34-67` defines the device path as vendor node (20 bytes) + ACPI_ADR node (8 bytes) + END node (4 bytes). Its vendor GUID is `3fa80f1d-6627-41de-a6ef-71caa95721d4`. The driver publishes this complete path and GOP together on its single `mFramebufferHandle` (297-305). It does not publish a vendor-only parent. A search through MotoG20Pkg finds no other definition or installation of this vendor GUID.
2. `MotoG20PlatformBootManagerLib.c:188-198` takes `DevicePathFromHandle` and adds that full path to ConOut and ErrOut. `EfiBootManagerUpdateConsoleVariable` appends the supplied path; it does not create parent handles (`edk2/MdeModulePkg/Library/UefiBootManagerLib/BmConsole.c:423-489`).
3. `EfiBootManagerConnectConsoleVariable` finds the ACPI_ADR node in the instance at BmConsole589-601, overwrites its header with END at608, then asks `EfiBootManagerConnectDevicePath` to connect the shortened parent path at609. The effective request is now vendor (20 bytes) + END. The original 8-byte ADR tail may remain in the allocated buffer, but traversal stops at the new END.
4. `EfiBootManagerConnectDevicePath` immediately calls LocateDevicePath with this shortened path (`BmConnect.c:135`). No full-path retry exists on initial lookup failure.
5. `CoreLocateDevicePath` computes the request's non-END size as20 (`edk2/MdeModulePkg/Core/Dxe/Hand/Locate.c:476-490`). It computes the complete GOP handle path's non-END size as28, then requires `Size <= SourceSize` before comparing (`514-516`). The GOP therefore cannot match: **28 <= 20 is false**.
6. With no distinct matching parent handle, Locate returns NOT_FOUND (540-541); BmConnect exits without calling ConnectController on the GOP. BmConsole then removes that path instance from the console variable (619-624) and ultimately returns NOT_FOUND if no other instance connected (635-638).

The public virtual splitter interfaces remain installed even when this route never binds GraphicsConsole to the physical GOP. This supplies a concrete mechanism for a non-null, successful but empty ConOut.

The proof model `prove_flat_gop_path.py` encodes the exact vendor/ADR/END lengths and CoreLocateDevicePath prefix condition, with positive controls for the original full path and a hypothetical real parent. `FLAT_GOP_PATH_PROOF.json` reports seven assertions passed. It is a host model, not EDK2 execution or a phone test.

## Minimal controlled repair for this milestone

Within BeforeConsole, after successfully adding the physical GOP's **full** device path to ConOut and ErrOut, a direct:

```
gBS->ConnectController (KnownPhysicalGopHandle, NULL, NULL, TRUE)
```

bypasses the faulty path lookup. Scope it to the known MotoG20 GOP: non-virtual handle, correct vendor path, framebuffer base/size/mode compatible with the fixed platform. Record the call, returned status and subsequent physical/backend census; do not treat ConnectController success alone as proof every required driver bound.

Why this route is justified: `GraphicsConsoleControllerDriverSupported` accepts a controller with GOP and a device path, while excluding the virtual splitter; DriverStart installs same-handle SimpleTextOut. ConPlatform can then match the already-populated full ConOut/ErrOut paths and install ConsoleOutDeviceGuid/StandardErrorDeviceGuid. ConSplitter binds those identifiers to its virtual consoles. `CoreConnectSingleController` loops driver support/start passes until no further driver can start (`DriverSupport.c:628-676`), allowing this layered same-controller sequence during one direct call. HII or allocation problems can still stop it, so census/return records remain necessary.

This repair addresses connection to the existing physical handle. It does **not** make the later unmodified BmConsole parent lookup valid: that call still truncates and can erase ConOut/ErrOut even after the direct route succeeded. The deletion does not itself invoke DisconnectController or uninstall the already-open backend. Consequently:

- capture the standard helper's real failure instead of relabeling it success;
- do not reset solely on that helper's error if the separately verified direct path succeeded;
- retain distinct direct-connection, variable-helper and physical/backend results;
- decide explicitly whether restoring the known full console-variable entries after the failed helper is needed for the milestone; the original path mismatch still affects any later reuse of the generic helper.

## More complete alternatives

1. Represent the platform's flat GOP controller with vendor+END, removing the unimplemented parent/child ACPI_ADR topology. Then the generic helper follows its non-ADR path and LocateDevicePath can select the actual GOP handle. This requires changing/rebuilding GOP and verifying all consumers of the device path; the framebuffer/memory fix need not otherwise change.
2. Provide a narrowly bounded full-path fallback when the parent lookup fails in BmConsole, preserving/restoring the original node header before connecting the complete path. This can be confined to the platform's isolated BDS build, but changes the generic boot-manager library semantics and requires explicit tests for genuine bus-backed GOPs and full-path fallback failure.
3. Implement a real parent/controller-child driver topology if that topology is desired. Merely inventing a vendor-only handle with a device path is insufficient: recursively connecting that empty parent will not discover the existing GOP as a child without a proper driver/open relationship.

For the current minimal diagnostic candidate, direct known-handle connection plus honest census is the smallest isolated correction. For durable generic reconnection semantics, the flat vendor-only path or a carefully tested complete-path fallback is preferable.

## Remaining uncertainty

No live device-path census was collected here. An unknown runtime root/prefix handle can affect which alternative handle LocateDevicePath selects; it cannot make the 28-byte GOP handle match the 20-byte request. The last photo still does not establish that BDS reached this connection routine. The new candidate must retain entry/connection checkpoints to distinguish this verified incompatibility from additional earlier failures.
