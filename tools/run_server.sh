#!/bin/sh
# usage: tools/run_server.sh <dir-name> <seconds> <command...>  - starts testserver/<dir> (127.0.0.1 only), runs the command, stops the server
d="$(cd "$(dirname "$0")/.." && pwd)/testserver/$1"; secs=$2; shift 2
rm -rf "$d/world"; (cd "$d" && exec java -Xmx512M -jar server.jar nogui < /dev/null > console.log 2>&1) &
spid=$!
i=0; while [ $i -lt 90 ] && ! grep -q 'Done (' "$d/console.log" 2>/dev/null; do sleep 1; i=$((i+1)); done
"$@"; rc=$?
kill $spid 2>/dev/null; wait $spid 2>/dev/null
exit $rc
