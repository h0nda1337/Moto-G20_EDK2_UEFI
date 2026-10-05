#!/usr/bin/env bash
set -eo pipefail
[[ $# -eq 2 ]] || exit 2
case "$1" in
  HC) module='MotoG20Pkg/Drivers/MotoG20MusbDxe/MotoG20MusbDxe.inf' ;;
  BDS) module='MotoG20Pkg/Diagnostics/BdsDxe/BdsDxe.inf' ;;
  *) exit 2 ;;
esac
[[ "$2" =~ ^[A-Za-z0-9_-]+$ ]] || exit 2
root='/mnt/c/MOTO_G20_WOA_LAB/AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
shared='/mnt/c/MOTO_G20_WOA_LAB/05_EDK2/edk2'
run_dir="$root/REPORTS/${1}_${2}"
mkdir "$run_dir"
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
source "$shared/edksetup.sh" BaseTools > "$run_dir/setup.log" 2>&1
set -u
{ aarch64-linux-gnu-gcc --version; python3 --version; sha256sum /usr/bin/aarch64-linux-gnu-gcc; } > "$run_dir/toolchain.txt"
python3 - "$WORKSPACE/MotoG20Pkg" > "$run_dir/overlay_sha256.json" <<'PY'
import hashlib, json, pathlib, sys
root=pathlib.Path(sys.argv[1])
print(json.dumps([{'path':p.relative_to(root).as_posix(),'size':p.stat().st_size,
                  'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()}
                 for p in sorted(root.rglob('*')) if p.is_file()],indent=2))
PY
set +e
logical_cpus="$(nproc)"
printf '%s\n' "$logical_cpus" > "$run_dir/logical_cpus.txt"
date -u +'%Y-%m-%dT%H:%M:%SZ' > "$run_dir/started_utc.txt"
setsid timeout --signal=TERM --kill-after=5s 1800s build \
  -a AARCH64 -t GCCNOLTO -p "$WORKSPACE/MotoG20Pkg/Foundation/MotoG20UsbHost.dsc" \
  -m "$module" -b DEBUG -n "$logical_cpus" --conf "$root/CONF" > "$run_dir/build.log" 2>&1 &
workload_pid=$!
printf '%s\n' "$workload_pid" > "$run_dir/linux_workload_pid.txt"
wait "$workload_pid"
status=$?
printf '%s\n' "$status" > "$run_dir/exit_code.txt"
date -u +'%Y-%m-%dT%H:%M:%SZ' > "$run_dir/finished_utc.txt"
tail -n 24 "$run_dir/build.log"
exit "$status"
