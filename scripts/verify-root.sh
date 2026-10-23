#!/usr/bin/env bash
set -euo pipefail

serial=${1:-${ADB_SERIAL:-}}
adb_cmd=(adb)
if [[ -n $serial ]]; then
  adb_cmd+=(-s "$serial")
fi

"${adb_cmd[@]}" shell "/data/local/tmp/ghostlock-root -c 'id; whoami; getenforce'"
