#!/usr/bin/env bash
# Exercise the SD core, platform sequencing, and read-only filesystem probe.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$root"
mkdir -p .build/tests
dev="$root/development/microsd_v1"
out="$root/.build/tests"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I "$dev/core" "$dev/core/sdhci_ro.c" "$dev/tests/test_sdhci_ro.c" \
  -o "$out/test_sdhci_ro"
ASAN_OPTIONS=detect_leaks=1 "$out/test_sdhci_ro" | tee "$out/sd-core.txt"
bash "$dev/platform/run_tests.sh" "$out/platform"
bash "$dev/tests/probe_host/run_tests.sh" "$out/probe"
