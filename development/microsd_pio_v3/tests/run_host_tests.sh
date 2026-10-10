#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?supply an output directory}"
mkdir -p "$out"
cc --version > "$out/toolchain.txt"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$here/core" "$here/core/sdhci_ro.c" "$here/tests/test_sdhci_ro.c" \
  -o "$out/test_sdhci_ro"
ASAN_OPTIONS=detect_leaks=1 "$out/test_sdhci_ro" | tee "$out/host_tests.txt"
aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -ffreestanding -fno-builtin -I "$here/core" -c "$here/core/sdhci_ro.c" \
  -o "$out/sdhci_ro_aarch64.o"
find "$here/core" "$here/tests" -type f -print0 | sort -z | xargs -0 sha256sum > "$out/host_tests_sha256.txt"
sha256sum "$out/test_sdhci_ro" "$out/sdhci_ro_aarch64.o" >> "$out/host_tests_sha256.txt"
