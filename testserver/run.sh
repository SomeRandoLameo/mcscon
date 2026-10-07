#!/bin/sh
# usage: ./run.sh 1.12.2   (foreground; stop with Ctrl-C or "stop"). Binds to 127.0.0.1 only.
cd "$(dirname "$0")/$1" && exec java -Xmx512M -jar server.jar nogui
