# Console backend audit, 2026-09-14

Read-only source review. No build, firmware edit, device command or new physical result. The last photograph shows a protocol notification: TEXT OUTPUT, status 0, bitmap 0x3F. It does not prove Shell entry or failure. Source paths below are relative to `C:\MOTO_G20_WOA_LAB\05_EDK2`.

## Findings that can be demonstrated from source

1. **The displayed text interface can be an empty virtual console.** `edk2/MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitter.c:505-519` installs `mConOut.TextOut` and assigns it to `gST->ConOut` during initialization. The `mConOut` template starts with zero `CurrentNumberOfConsoles` (123-168). `ConSplitterTextOutOutputString` initializes its return status to SUCCESS before its physical-device loop (4426); with zero consoles it only updates the virtual cursor (4436-4495) and still returns SUCCESS. Consequently, non-null `gST->ConOut`, a non-null text interface, `QueryMode` success and `OutputString` success individually do not establish a physical console.

2. **The monitor does not identify the interface which caused a notification.** `MotoG20Pkg/Drivers/MotoG20DxeProgressDxe/MotoG20DxeProgressDxe.c:47-58` calls `LocateProtocol(Guid, NULL, ...)`, ignoring its registration cursor. Every later notification can report the already-existing first interface. The bitmap is cumulative, and the callback redraws even when the corresponding bit was already set. Thus a GraphicsConsole installation can cause a later TEXT OUTPUT marker carrying the old virtual splitter pointer.

3. **Display release is module-local.** `MotoG20Pkg/Library/MotoG20DxeTelemetryLib/MotoG20DxeTelemetryLib.c:193` declares `mDisplayReleased` STATIC; `ReleaseDisplay` only sets this flag (1215-1221). Both BdsDxe (via PlatformBootManagerLib) and ProgressDxe link the library separately. Releasing the BDS instance leaves the Progress instance able to draw over the console. This is a concrete control defect, but the photograph does not prove it was the cause of the last visible screen.

4. **The callback performs substantial synchronous work at TPL_CALLBACK.** Progress creates six protocol events at TPL_CALLBACK (83-89). Its callback calls the allocation-free telemetry path, which directly stores pixels to the framebuffer (`FillRectangle`, telemetry 450-475), renders records and commits ramoops before returning (`Publish`, 966-1106). Initial panel clear alone writes 720 x 1600 = 1,152,000 pixels. Subsequent records erase and redraw the value cells. Same or lower-priority callback work waits until this callback yields control. This supports a latency/overdraw concern, not an established deadlock: the normal telemetry path contains no UEFI blocking wait, no GOP call, and no allocation. The 20-second wait is only in failure/reset paths. The bitmap remains a notification marker, not a measured execution timeline.

5. **The current boot policy discards diagnostic return values.** `MotoG20PlatformBootManagerLib.c:195-197` discards ErrOut-variable update status. `PlatformBootManagerAfterConsole` treats non-null ConOut as ready (214-218), discards ClearScreen status (232), and uses Print without recording the underlying OutputString status (233). `edk2/MdeModulePkg/Universal/BdsDxe/BdsEntry.c:927-928` discards both console-connection statuses in this platform's lazy-input branch (`PcdConInConnectOnDemand=TRUE`, platform DSC111).

6. **A splitter driver-start result can itself hide an add-device error.** `ConSplitterConOutDriverBindingStart` assigns the result of `ConSplitterTextOutAddDevice` at1335 but does not test it. If a GOP exists, it overwrites Status with GOP QueryMode at1339 and returns that status at1349. An allocation failure in AddDevice can therefore leave the backend absent while the start routine eventually reports success. This is a verified error-propagation flaw, not evidence it occurred on the phone.

## The exact physical-to-splitter relation

Do not query BY_CHILD_CONTROLLER on SimpleTextOut or GOP expecting the splitter link. `ConSplitterConOutDriverBindingStart` passes **gEfiConsoleOutDeviceGuid** to `ConSplitterStart` at1300-1307. `ConSplitterStart` opens that DeviceGuid BY_DRIVER at1045-1052, then BY_CHILD_CONTROLLER at1060-1067. The latter open has:

- handle containing the protocol: the physical console controller;
- protocol: `gEfiConsoleOutDeviceGuid`;
- AgentHandle: the ConOut splitter's DriverBindingHandle;
- ControllerHandle: `mConOut.VirtualHandle`, which should equal `gST->ConsoleOutHandle`;
- attributes: `EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER`;
- OpenCount: positive.

SimpleTextOut is subsequently opened only GET_PROTOCOL (1077-1084). ConPlatform adds ConsoleOutDeviceGuid to the eligible text-output handle (`edk2/MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatform.c:420-427`). GraphicsConsole opens GOP BY_DRIVER on its physical controller (GraphicsConsole.c401-409), then installs SimpleTextOut on the **same** controller (557-563).

A useful public-API census in `AfterConsole` therefore enumerates GOP handles, excludes `gST->ConsoleOutHandle`, records each GOP's actual framebuffer base/size and mode, checks same-handle SimpleTextOut, checks ConsoleOutDeviceGuid, and queries that GUID's `OpenProtocolInformation`. A positive child relation to `gST->ConsoleOutHandle` shows the expected binding relationship. It does **not** prove the splitter backend list was populated: these opens precede `ConSplitterTextOutAddDevice`, which can fail without cleaning them up.

Do not hard-code the earlier pointer or cast `gST->ConOut` to a private splitter struct by guessed offset. Module addresses and layout can change in the next build. For this fixed platform, framebuffer base 0x9E000000 and size 0x465000 help distinguish the physical MotoG20 GOP from virtual GOP, but the report must include what was actually observed.

To assert actual aggregation, the strongest small source probe is within ConSplitter: record `CurrentNumberOfConsoles`, `CurrentNumberOfGraphicsOutput`, add-device status, and the first physical TextOut pointer after the add operation; alternatively expose these through a read-only diagnostic protocol. If keeping ConSplitter byte-identical, call the public-API result **physical candidate plus registered child relation**, not “backend confirmed.” A distinctive marker written through `gST->ConOut` and visible in the photograph is an independent physical observation of text rendering.

## Where BDS can stop before the platform hook

The protocol bitmap does not cover all 13 mandatory architectural protocols. `DxeMain/DxeProtocolNotify.c:21-35` includes Watchdog Timer at27 along with variable, variable-write, runtime, reset, RTC, security, metronome, capsule and monotonic services. `DxeMain.c:646-669` checks the complete list and fails/resets before `gBds->Entry` if any is absent. Installing BDS protocol plus the monitor's six bits is insufficient.

`BdsEntry.c:679` begins BDS, but `PlatformBootManagerBeforeConsole` is called only at905. Between those points are variable formalization and policies, language initialization, multiple allocations and SetVariable calls, ASSERTs, and a `CpuDeadLoop` if FileDevicePath allocation fails (816-820). Therefore BEFORE CONSOLE proves arrival at that hook, not the earliest BDS entry. Absence of that marker cannot identify which earlier operation stopped execution.

There is no SetWatchdogTimer call in BdsEntry before BeforeConsole. Five-minute watchdog setup exists in standard UefiBootManagerLib boot/load-option paths (BmBoot2105; BmLoadOption1490), but the local `LaunchBuiltInShell` calls gBS StartImage directly and bypasses those wrappers. Do not attribute this panel to a watchdog without independent evidence.

## Minimal next diagnostic boundary changes

Keep names honest about whether an event is **before a call**, **after it returned**, or **actual application entry**. Allocate fresh numeric stage IDs centrally; values below are names only.

1. Quiesce ProgressDxe before the platform starts handing the framebuffer to text output. A shared control protocol implemented by ProgressDxe can set its stop flag first and close its own six notify events, then report bitmap/close status to BootManager. It should be idempotent; already queued notifications must check the stop flag before publishing. Merely calling BDS's existing `ReleaseDisplay` is insufficient. First-seen bit deduplication also bounds normal pre-BDS drawing.
2. Add BDS ENTRY at the beginning of BdsEntry, distinct from BEFORE CONSOLE. This is in the same BdsDxe binary as the boot-manager library, so it does not require replacing an extra firmware module. It does require an explicit isolated source copy/override if the original edk2 tree must remain unchanged.
3. Preserve existing BdsEntry control flow. Around the **existing** calls at927-928 emit CONOUT CONNECT CALL / CONOUT CONNECT RETURN with the real returned status, then ERROUT equivalents. In the alternate branch instrument the existing ConnectAllDefaultConsoles call. Do not add duplicate connection calls in BeforeConsole; that changes behavior and can hide the original failure.
4. In AfterConsole capture the public-API census above. Preserve separate counts for physical GOP candidates, same-handle text interfaces and matching splitter child relations. Record failures per operation rather than coercing all failures to “no backend.” If the deeper internal probe is not added, retain its stated uncertainty.
5. Quiesce monitor, release BDS telemetry drawing, call ClearScreen, record its return; call `gST->ConOut->OutputString` directly with a distinctive short marker, record its return. Print does not expose the EFI_STATUS needed here. Once display is released, successful telemetry records should persist without repainting; a failed required output step can deliberately reclaim the diagnostic screen through the failure path. OutputString SUCCESS still needs census and/or visible evidence.
6. In LaunchBuiltInShell record SHELL PATH RESULT; SHELL LOAD CALL; SHELL LOAD RETURN with the **actual** LoadImage status and returned handle; SHELL START CALL; SHELL START RETURN with the **actual** StartImage status and ExitDataSize. Record before freeing ShellPath/ExitData if their values are needed, and never dereference a failed or null image handle.
7. The current `MotoG20DxeStageShellStart` is before StartImage. Rename its displayed label to SHELL START CALL or document that exact meaning. A call marker does not prove image entry. To prove Shell code entry add a Shell-side entry checkpoint or use verified DxeCore entry instrumentation tied to the loaded Shell identity; visible Shell prompt is stronger evidence for the user. A success return proves StartImage returned successfully, not that Shell is currently running.

## Suggested census skeleton (design, not compiled code)

```
for each GOP handle H:
    obtain GOP interface and safely validate Mode/Info pointers
    if H == gST->ConsoleOutHandle: classify as virtual-system-console
    otherwise record base/size/resolution as a candidate
    text_status = HandleProtocol(H, SimpleTextOutGuid, &physical_text)
    open_status = OpenProtocolInformation(H, ConsoleOutDeviceGuid, &entries, &count)
    if open_status succeeded:
        for each entry:
            child_relation |= entry.OpenCount > 0
                && (entry.Attributes & BY_CHILD_CONTROLLER) != 0
                && entry.ControllerHandle == gST->ConsoleOutHandle
    FreePool(entries) if returned
```

Results should never collapse `text_status`, `open_status`, child-relation presence and actual rendering into one “console ready” boolean. The request is to distinguish false positives, so these dimensions must remain inspectable.

## Limits

This audit explains why both hypotheses remain possible: console under an unreleased monitor, or virtual console without usable physical backend; earlier BDS stops also remain possible. It does not establish the phone's latest state, recovery, execution count or a new success. Those require the parent's fresh device evidence and preserved registry.
