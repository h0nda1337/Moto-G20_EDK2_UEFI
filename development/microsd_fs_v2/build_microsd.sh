#!/usr/bin/env bash
set -eo pipefail
[[ $# -eq 1 && "$1" =~ ^[A-Za-z0-9_-]+$ ]] || exit 2
root='/mnt/g/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_20261004_V2'
shared='/mnt/c/MOTO_G20_WOA_LAB/05_EDK2/edk2'
run="$root/REPORTS/BUILD_$1"
mkdir "$run"
mkdir "$root/BUILD_TRANCHE.lock"
trap 'rmdir "$root/BUILD_TRANCHE.lock"' EXIT
export WORKSPACE="$root/WORKSPACE"
export PACKAGES_PATH="$WORKSPACE:$shared"
export EDK_TOOLS_PATH="$shared/BaseTools"
export GCCNOLTO_AARCH64_PREFIX=aarch64-linux-gnu-
export CONF_PATH="$root/CONF"
export PYTHONDONTWRITEBYTECODE=1
export SOURCE_DATE_EPOCH=1788934756
cd "$WORKSPACE"
source "$shared/edksetup.sh" BaseTools > "$run/setup.log" 2>&1
set -u
{ aarch64-linux-gnu-gcc --version; python3 --version; } > "$run/toolchain.txt"
python3 - "$WORKSPACE/MotoG20Pkg" > "$run/sources.json" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
print(json.dumps([{'path': p.relative_to(root).as_posix(), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest().upper()}
                 for p in sorted(root.rglob('*')) if p.is_file()], indent=2))
PY
set +e
logical_cpus="$(nproc)"
printf '%s\n' "$logical_cpus" > "$run/logical_cpus.txt"
timeout --signal=TERM --kill-after=5s 1800s build \
  -a AARCH64 -t GCCNOLTO -p MotoG20Pkg/Foundation/MotoG20MicroSd.dsc \
  -b DEBUG -n "$logical_cpus" --conf "$CONF_PATH" > "$run/build.log" 2>&1
status=$?
printf '%s\n' "$status" > "$run/exit_code.txt"
tail -n 30 "$run/build.log"
exit "$status"
