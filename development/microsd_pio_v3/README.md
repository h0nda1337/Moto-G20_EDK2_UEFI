# Moto G20 microSD V3: CMD17 transfer setup

Historical implementation retained for comparison. The current build uses V4.

V2 identified the card but failed the first LBA0 read. V3 reads the SDHCI
specification byte, selects the 32-bit count register at 0x00 for version >=3,
programs a count of 1 and verifies block size/count before CMD17. It uses
single-block PIO mode 0x10 and requires buffer-ready, all 512 bytes, transfer
completion and data-inhibit release before returning data.

The native Motorola source is pinned at
`261390cdfff66efef9082bd70c1af0abd21d77f2`; the source manifest is under
`../../docs/evidence/motorola-source-manifest.json`.

Build `01_20261008`, tested image SHA-256:
`15FE4039A757FCD0102A9BD8C5D94E90A9EBE75199A145560A10AD807D52434C`.

The photo confirmed count=1 at 0x00 and block=0x200, but raw LBA0 still failed
with IRQ 2 and zero delivered blocks. IRQ 2 signals transfer completion; it
is not a hardware CRC/timeout error bit. PIO-ready was absent from the observed
interrupt status. The later No Media reflected the driver's fault latch.
[Physical result](../../docs/evidence/microsd-v3-physical-result.json).

No sector or FAT success was established, and Windows PE was not started.
The RAM expansion, ACPI/SMBIOS and Windows CPU work remain separate. Historical
host checks and mock tests do not establish physical success. Physical dispatch
scripts and device-specific logs are not included in this publication.
