#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?supply an output directory}"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$here/core" "$here/core/sdhci_ro.c" "$here/tests/test_sdhci_ro.c" \
  -o "$out/test_sdhci_ro"
ASAN_OPTIONS=detect_leaks=1 "$out/test_sdhci_ro" | tee "$out/host_tests.txt"
aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -ffreestanding -fno-builtin -I "$here/core" -c "$here/core/sdhci_ro.c" \
  -o "$out/sdhci_ro_aarch64.o"
sha256sum "$here/core/"* "$here/tests/"* "$out/sdhci_ro_aarch64.o" > "$out/host_tests_sha256.txt"
