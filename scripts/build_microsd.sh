#!/usr/bin/env bash
# Native Linux or Debian WSL; all output stays in .build.
set -eo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$root"
python3 scripts/setup_edk2.py
jobs="${JOBS:-$(nproc)}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || exit 2
mkdir -p .build/CONF .build/reports
if [[ ! -x edk2/BaseTools/Source/C/bin/GenFv ]]; then
  make -C edk2/BaseTools -j "$jobs" > .build/reports/basetools.log 2>&1 || {
    tail -n 40 .build/reports/basetools.log
    exit 1
  }
fi
cp config/tools_def.txt config/build_rule.txt .build/CONF/
cat > .build/CONF/target.txt <<EOF
ACTIVE_PLATFORM = MotoG20Pkg/Foundation/MotoG20MicroSd.dsc
TARGET = DEBUG
TARGET_ARCH = AARCH64
TOOL_CHAIN_TAG = GCCNOLTO
TOOL_CHAIN_CONF = $root/.build/CONF/tools_def.txt
BUILD_RULE_CONF = $root/.build/CONF/build_rule.txt
EOF
export WORKSPACE="$root"
export PACKAGES_PATH="$root:$root/edk2"
export EDK_TOOLS_PATH="$root/edk2/BaseTools"
export CONF_PATH="$root/.build/CONF"
export GCCNOLTO_AARCH64_PREFIX=aarch64-linux-gnu-
export PYTHONDONTWRITEBYTECODE=1
export SOURCE_DATE_EPOCH=1788934756
source edk2/edksetup.sh BaseTools > .build/reports/setup.log 2>&1
python3 scripts/source_manifest.py > .build/reports/sources.json
{ aarch64-linux-gnu-gcc --version; python3 --version; } > .build/reports/toolchain.txt
set +e
build -a AARCH64 -t GCCNOLTO -b DEBUG -n "$jobs" \
  -p MotoG20Pkg/Foundation/MotoG20MicroSd.dsc --conf "$CONF_PATH" \
  > .build/reports/build.log 2>&1
status=$?
printf '%s\n' "$status" > .build/reports/exit_code.txt
tail -n 25 .build/reports/build.log
exit "$status"
