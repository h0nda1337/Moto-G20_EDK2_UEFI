#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$root/tests/make_cases.py"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g "$root/tests/test_memory_plan.c" "$root/memory/memory_plan.c" -o "$root/evidence/test_memory_plan"
"$root/evidence/test_memory_plan" "$root/evidence/cases"
aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -c "$root/memory/memory_plan.c" -o "$root/evidence/memory_plan_aarch64.o"
cc -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g "$root/tests/test_cpu_plan.c" "$root/cpu/cpu_boot_plan.c" -o "$root/evidence/test_cpu_plan"
"$root/evidence/test_cpu_plan"
aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -ffreestanding -c "$root/cpu/cpu_boot_plan.c" -o "$root/evidence/cpu_boot_plan_aarch64.o"
