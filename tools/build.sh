#!/bin/bash
# build.sh [target]: kills the game, builds release, prints errors and the last lines
cd "$(dirname "$0")/.."
# the build's own exe only (a copy the player runs elsewhere is left alone)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='CODFH.exe'\" | Where-Object { \$_.ExecutablePath -like '*\\out\\build\\release\\CODFH.exe' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1
cmd //c "call .\build_local.cmd release $1 > build_local.log 2>&1"
rc=$?
grep -aE "error:|error [A-Z]+[0-9]+|FAILED" build_local.log | head -20
tail -2 build_local.log
echo "build exit $rc"
exit $rc
