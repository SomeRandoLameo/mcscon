#!/usr/bin/env python3
"""usage: run_with_console.py <server-dir-name> <port> <bot-binary> [args...]  - generic: forwards 'CMD ...' lines of the bot to the server console"""
import os, subprocess, sys, threading, time, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver, port, binary, args = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
sd = f"{root}/testserver/{ver}"; shutil.rmtree(f"{sd}/world", ignore_errors=True)
srv = subprocess.Popen(["java", "-Xmx512M", "-jar", "server.jar", "nogui"], cwd=sd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
ready = threading.Event(); log = []
def pump():
    for line in srv.stdout:
        log.append(line)
        if "Done (" in line: ready.set()
threading.Thread(target=pump, daemon=True).start()
rc = 1
try:
    if not ready.wait(90): print("server did not start"); sys.exit(2)
    bot = subprocess.Popen([f"{root}/build/{binary}", port] + args, stdout=subprocess.PIPE, text=True)
    for line in bot.stdout:
        line = line.rstrip()
        if line.startswith("CMD "): srv.stdin.write(line[4:] + "\n"); srv.stdin.flush()
        else: print(line)
    bot.wait(); rc = bot.returncode
finally:
    try: srv.stdin.write("stop\n"); srv.stdin.flush()
    except Exception: pass
    try: srv.wait(20)
    except Exception: srv.kill()
print("--- server WARN/ERROR lines:"); print("".join(l for l in log if "WARN" in l or "ERROR" in l and "FileNotFound" not in l), end="")
sys.exit(rc)
