#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dev="$(cd "$here/../.." && pwd)"
out="${1:?supply an evidence output directory}"
mkdir -p "$out"
cc --version > "$out/toolchain.txt"
cc -std=c11 -fshort-wchar -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$here/include" -I "$dev/driver" -I "$dev/platform" -I "$dev/core" "$here/test_probe.c" \
  -o "$out/test_probe"
ASAN_OPTIONS=detect_leaks=1 "$out/test_probe" | tee "$out/host_tests.txt"
sha256sum "$dev/bds/MotoG20SdProbe.c" "$dev/bds/MotoG20SdProbe.h" \
  "$dev/driver/MotoG20SdDiagnostic.h" "$here/test_probe.c" "$here/run_tests.sh" \
  "$here/include/uefi_mock.h" "$out/test_probe" > "$out/source_binary_sha256.txt"
