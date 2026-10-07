#!/usr/bin/env python3
"""Joins two bots repeatedly (each for 17 s, i.e. past the 15 s keep-alive window) against one running server; checks that the
server never logs 'Timed out'.  usage: join_stress.py <server-dir> <port> <rounds>"""
import os, subprocess, sys, threading, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver, port, rounds = sys.argv[1], sys.argv[2], int(sys.argv[3])
sd = f"{root}/testserver/{ver}"; shutil.rmtree(f"{sd}/world", ignore_errors=True)
srv = subprocess.Popen(["java", "-Xmx512M", "-jar", "server.jar", "nogui"], cwd=sd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
ready = threading.Event(); log = []
def pump():
    for l in srv.stdout:
        log.append(l)
        if "Done (" in l: ready.set()
threading.Thread(target=pump, daemon=True).start(); ready.wait(90)
bad = 0
for i in range(rounds):
    r = subprocess.run([f"{root}/build/integ_two", port, "17"], capture_output=True, text=True)
    ok = "PASS" in r.stdout; bad += (not ok); print(f"round {i+1}: {'ok' if ok else 'FAILED'}  {r.stdout.strip().splitlines()[-3:]}")
srv.stdin.write("stop\n"); srv.stdin.flush(); srv.wait(20)
timeouts = [l.strip() for l in log if "Timed out" in l]
print(f"rounds={rounds} failed={bad} server-side 'Timed out' lines={len(timeouts)}"); sys.exit(1 if bad or timeouts else 0)
