#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
core="$here/../microsd_v1/core"
out=${1:?supply output directory}
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$core" -I"$here" \
  "$core/sdhci_ro.c" "$here/legacy_clock_plan.c" "$here/test_legacy_clock_plan.c" -o "$out/test_legacy_clock_plan"
"$out/test_legacy_clock_plan" | tee "$out/legacy_clock_plan_tests.txt"
sha256sum "$here"/*.[ch] "$here/run_tests.sh" > "$out/legacy_clock_plan_sources.sha256"
