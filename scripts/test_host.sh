#!/usr/bin/env bash
# Test the sources used by the default V4 firmware build, without a phone.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$root"
dev="$root/development/microsd_sdma_v4"
out="$root/.build/tests/v4"
mkdir -p "$out"
python3 scripts/check_current_sources.py | tee "$out/source-check.json"
export UBSAN_OPTIONS=halt_on_error=1
bash "$dev/tests/run_host_tests.sh" "$out/core"
bash "$dev/tests/dma_buffer/run_tests.sh" "$out/dma-buffer" | tee "$out/dma-buffer.txt"
bash "$dev/tests/raw_host/run_tests.sh" "$out/raw"
bash "$dev/tests/storage_host/run_tests.sh" "$out/storage"
bash "$dev/tests/probe_host/run_tests.sh" "$out/probe"
# The checker verifies that this V1 platform engine matches the current V4.
bash "$root/development/microsd_v1/platform/run_tests.sh" "$out/platform"
# Separate platform plans remain unintegrated; retain their existing checks.
bash "$root/development/winpe_platform_boot/tests/run.sh" | tee "$out/winpe-platform-plans.txt"
printf '%s\n' 'PASS: current V4 sources and host checks; physical sector reading remains unverified.'
