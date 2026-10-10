#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dev="$(cd "$here/../.." && pwd)"
out="${1:?supply an evidence output directory}"
mkdir -p "$out"
cc --version > "$out/toolchain.txt"
cc -std=c11 -fshort-wchar -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$here/include" "$here/test_raw.c" -o "$out/test_raw"
ASAN_OPTIONS=detect_leaks=1 "$out/test_raw" | tee "$out/host_tests.txt"
sha256sum "$dev/bds/MotoG20RawSdProbe.c" "$dev/bds/MotoG20RawSdProbe.h" \
  "$dev/bds/MotoG20SdSectorReferences.h" "$here/test_raw.c" \
  "$here/run_tests.sh" "$here/include/uefi_mock.h" "$out/test_raw" \
  > "$out/source_binary_sha256.txt"
