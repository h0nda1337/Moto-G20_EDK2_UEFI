#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
export WORKSPACE="$root/build_workspace"
export EDK_TOOLS_PATH=/mnt/c/MOTO_G20_WOA_LAB/05_EDK2/edk2/BaseTools
export PACKAGES_PATH="$WORKSPACE:/mnt/c/MOTO_G20_WOA_LAB/05_EDK2/edk2"
export GCCNOLTO_AARCH64_PREFIX=aarch64-linux-gnu-
export PYTHON_COMMAND=python3
export PYTHONDONTWRITEBYTECODE=1
export SOURCE_DATE_EPOCH=1790985600
mkdir -p "$WORKSPACE/WinPePlatformBootPkg" "$WORKSPACE/Conf"
cp -a "$root/memory" "$WORKSPACE/WinPePlatformBootPkg/"
cp -a "$root/acpi" "$WORKSPACE/WinPePlatformBootPkg/"
cp -a "$root/cpu" "$WORKSPACE/WinPePlatformBootPkg/"
for f in target tools_def build_rule; do
  cp "/mnt/g/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_20261002_V1/CONF/$f.txt" "$WORKSPACE/Conf/$f.txt"
done
cp "$root/WinPePlatformBoot.dsc" "$WORKSPACE/WinPePlatformBootPkg/"
cd "$WORKSPACE"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
set +e
build -p WinPePlatformBootPkg/WinPePlatformBoot.dsc -a AARCH64 -t GCCNOLTO -b DEBUG -n "$(nproc)" > "$root/evidence/edk2_build.log" 2>&1
result=$?
set -e
tail -n 12 "$root/evidence/edk2_build.log"
exit "$result"
