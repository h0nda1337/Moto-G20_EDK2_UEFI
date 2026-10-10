# BDS probe host checks

The harness includes the actual `bds/MotoG20SdProbe.c`; its decision logic is not
copied into a second implementation. Fake boot services provide diagnostic,
device-path and filesystem protocols and record calls and read sizes. File opens
assert `EFI_FILE_MODE_READ`, zero attributes and the exact root marker name.

`run_tests.sh <output-directory>` builds with ASan/UBSan and strict C warnings. The
run records named scenarios, compiler version and hashes. Fixtures cover a 31-byte
marker plus EOF, partial reads, truncation, changed bytes, appended data, the
128-byte cap, errors including the EOF read and close, foreign controllers/slots,
malformed and multi-instance paths, protocol failures and once-only invocation.

The headers deliberately model only the API surface required by the probe. They
are not a UEFI ABI test. `DevicePathLib` validation and console formatting are
simulated. This does not prove firmware dispatch, FAT compatibility, hardware I/O
or the electrical SD setup; EDK2 compilation and physical card testing remain
separate requirements. No phone, G: build tree or firmware image is accessed.
