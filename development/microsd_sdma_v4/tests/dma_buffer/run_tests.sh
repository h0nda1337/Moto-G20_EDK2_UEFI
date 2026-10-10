#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$test_dir/../.." && pwd)
output_dir=${1:?Usage: run_tests.sh OUTPUT_DIR}
mkdir -p "$output_dir"
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$source_dir/core" -I"$source_dir/driver" \
  "$source_dir/driver/sd_dma_buffer.c" "$test_dir/test_dma_buffer.c" \
  -o "$output_dir/test_dma_buffer"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$output_dir/test_dma_buffer"
aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -pedantic \
  -ffreestanding -fno-builtin -O2 -I"$source_dir/core" -I"$source_dir/driver" \
  -c "$source_dir/driver/sd_dma_buffer.c" -o "$output_dir/sd_dma_buffer.aarch64.o"
printf '%s\n' 'DMA buffer AArch64 freestanding compile PASS'
