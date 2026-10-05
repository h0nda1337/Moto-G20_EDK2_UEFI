#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=${1:?supply output directory}
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$here/../core" -I"$here" "$here/sd_platform_engine.c" "$here/test_platform.c" \
  -o "$out/test_platform"
"$out/test_platform" | tee "$out/platform_tests.txt"
aarch64-linux-gnu-gcc -std=c11 -ffreestanding -Wall -Wextra -Werror -Wconversion -Wshadow \
  -I"$here/../core" -I"$here" -c "$here/sd_platform_engine.c" -o "$out/sd_platform_engine_aarch64.o"
sha256sum "$here"/*.[ch] "$here/run_tests.sh" > "$out/platform_tests_sources.sha256"
