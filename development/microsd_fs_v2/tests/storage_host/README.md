# Storage-chain host tests

The harness compiles the production `bds/MotoG20StorageConnect.c` directly. Its
small UEFI header models only the used fields and services; it does not claim
firmware ABI compatibility. Run from WSL/Linux with a C compiler:

```sh
bash work/microsd_fs_v2/tests/storage_host/run_tests.sh outputs/microsd_fs_v2_20261007/storage_host
```

The runner enables AddressSanitizer and UndefinedBehaviorSanitizer, leak checks,
strict warnings, and short `wchar_t` for UEFI string literals. It records compiler
version, output, and source/binary SHA-256 digests. This is host verification only;
it does not establish a physical sector read, FAT mount, or Windows boot.

Coverage includes 64 behavioral cases and 10,000 deterministic malformed-path
pairs. Fake boot services observe distinct image/binding handles, connection
order and priorities, nonrecursive calls, new partitions created by root
connection, existing filesystem protocols, absent/duplicate bindings, malformed
and unrelated device paths, absent or ambiguous roots, handle/child count caps,
null protocol interfaces, service errors, media replacement, and pool cleanup.
The nine-partition/no-root case is a regression for an eight-slot output-array
overflow. Root connection success alone cannot become a filesystem success.

## Semantics checked against the pinned EDK2 source

`MdeModulePkg/Core/Dxe/Hand/DriverSupport.c` at
`b03a21a63e3bd001f52c527e5a57feddb53a690b` was reviewed locally:

- `CoreConnectSingleController` passes each supplied priority to
  `AddSortedDriverBindingProtocol` with `IsImageHandle = TRUE`; it resolves
  `EFI_DRIVER_BINDING_PROTOCOL.ImageHandle` to its bindings. The helper must pass
  loaded image handles, even when a binding is installed on another handle.
- The core subsequently adds other driver bindings, so the priorities are not an
  exclusive allowlist. No `LoadImage`, `StartImage`, direct binding `Start`, or
  dispatch bypass is introduced by the helper.
- With `Recursive = FALSE`, the core expands only the selected controller level.
  The helper refreshes BlockIo inventory after the root call, then selects only
  exact native-controller/SD-slot-0/hard-drive/end paths for the next phase.
- `EFI_NOT_FOUND` from `ConnectController` can mean no additional driver started.
  It is therefore followed by an inventory check. Other connection errors are
  preserved rather than overwritten by an inventory result.

`EFI_SUCCESS` from the helper means only that a present, matching SD partition
exposes SimpleFileSystem. The separate BDS file probe must establish readable
contents. UnicodeCollation2 availability is printed as a diagnostic observation;
it is not treated as proof that FAT has bound.
