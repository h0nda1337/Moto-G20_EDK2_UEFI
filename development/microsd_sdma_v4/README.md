# Moto G20 microSD V4: read-only SDMA diagnostic

This snapshot is the V4 implementation tested on the phone, not a completed
microSD or Windows driver. The physical LBA0 read failed. See
[the V4 result](../../docs/evidence/microsd-v4-physical-result.json) and
[current status](../../docs/STATUS.md).

V3 programmed the 32-bit count at 0x00 but received transfer-complete without
the PIO-ready condition required by the driver. V4 introduces the SDMA path
using the native Motorola register layout. The pinned kernel source is
`261390cdfff66efef9082bd70c1af0abd21d77f2`; file hashes are in
[motorola-source-manifest.json](../../docs/evidence/motorola-source-manifest.json).

## Implementation

- One 512-byte CMD17 read at a time, SDMA address registers 0x58/0x5c, 32-bit
  addressing. Identification, clocks, rails and pin setup are retained.
- A reserved 4 KiB page below 4 GiB within identity-mapped bootstrap RAM
  `[0x83300000, 0x87500000)`. The DMA payload starts at page + 512.
- Payload poisoning, 3584 guard bytes, full-page cache maintenance and ARM
  barriers. Data reaches the caller only after successful completion and guards.
- Version, capabilities, count and control readbacks; command completion,
  transfer completion and data-inhibit release are all required.
- A failed issued transfer invalidates and checks guards without copying data.
  The session latches failure and the reserved page is not freed or reused.
  Retaining the page does not prove an errored DMA engine is quiescent.
- The BDS attempts FAT only after complete matches for LBA 0, 2048 and 99072,
  then checks the exact 31-byte `G20SD.TXT` and EOF. References are card-specific.

No SD writes, erase, voltage switching or Windows execution are implemented in
this diagnostic. Bootstrap RAM is still 66 MiB. Future RAM and CPU preparation
remain separate in `../winpe_platform_boot/`.

## Physical result

Build `01_20261008`, image SHA-256:
`5B09AEAAE1168D20478123A5C000596701C6A6BDA825B93BE7FF6A7D157B2ADA`.

The photo shows LBA0 `Device Error`, `tries=1`, `complete=0`, `guards=1`,
`fault=1`. The code programs `0x113A0011`, but the register reads `0x113A0013`.
The exact comparison rejects the added bit before polling DMA completion.
No sector, FAT or Windows success is established. The firmware fix is pending.

## Host checks

Run `bash scripts/test_host.sh` from the repository root for the current V4
checks, source consistency, storage connection and platform model. See
[BUILD.md](../../docs/BUILD.md) for counts and dependencies. ASan/UBSan and
freestanding AARCH64 checks do not establish physical-device success.

Historical lab-only integration and physical dispatch scripts are not part of
this folder. The public build scripts do not operate the phone. Rebuilt images
have their own hashes and remain physically untested.
