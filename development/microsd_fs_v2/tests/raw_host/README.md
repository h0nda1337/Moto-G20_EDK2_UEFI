# Read-only sector proof host tests

This harness compiles the actual `bds/MotoG20RawSdProbe.c` against small UEFI
BlockIo/device-path mocks. Run from WSL/Linux with GCC and sanitizers:

```sh
bash work/microsd_fs_v2/tests/raw_host/run_tests.sh /path/to/evidence
```

Checks cover the three exact 512-byte Android references at LBAs 0, 2048 and
99072; wrong controllers/slots/partition paths; malformed paths; missing handles
or protocols; unexpected capacity, absent media, geometry/alignment guards;
read errors and warnings; media replacement/removal/change during each read;
and skipped/corrupted bytes at all 1536 reference byte positions.

Each case asserts the exact number of attempted reads, individual PASS messages,
and complete-proof messages. Missing bytes in zero-filled reference tails must
fail just like any other missing byte. The harness also asserts pool cleanup,
LBA order/bounds, transfer length and buffer alignment. ASan, UBSan and leak
detection apply to the host executable. These tests do not establish physical SD,
filesystem or Windows PE success.
