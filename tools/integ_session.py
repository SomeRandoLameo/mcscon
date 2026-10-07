#!/usr/bin/env python3
"""Runs tests/integ_session against a local vanilla server (127.0.0.1 only). usage: integ_session.py <1.10.2|1.11|1.11.2|1.12|1.12.1|1.12.2> [keep]
Starts the server, forwards the bot's 'CMD ...' lines to the server console, prints PASS/FAIL lines, stops the server."""
import os, subprocess, sys, threading, time, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver = sys.argv[1]; botver = {"1.10.2": "1.10", "1.11": "1.11", "1.11.2": "1.11.2", "1.12": "1.12", "1.12.1": "1.12.1", "1.12.2": "1.12.2"}[ver]
port = {"1.10.2": 25610, "1.11.2": 25611, "1.12.2": 25612, "1.11": 25613, "1.12": 25614, "1.12.1": 25615}[ver]
sd = f"{root}/testserver/{ver}"
if "keep" not in sys.argv:            # fresh world per run = deterministic (flat world generates instantly)
    for d in ("world",): shutil.rmtree(f"{sd}/{d}", ignore_errors=True)
srv = subprocess.Popen(["java", "-Xmx512M", "-jar", "server.jar", "nogui"], cwd=sd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
ready = threading.Event(); log = []
def pump():
    for line in srv.stdout:
        log.append(line)
        if "Done (" in line: ready.set()
threading.Thread(target=pump, daemon=True).start()
if not ready.wait(90): print("server did not start"); srv.kill(); sys.exit(2)
srv.stdin.write("gamerule doMobSpawning false\ngamerule doDaylightCycle false\ntime set 6000\nweather clear\n"); srv.stdin.flush()
bot = subprocess.Popen([f"{root}/build/integ_session", str(port), botver], stdout=subprocess.PIPE, text=True)
rc = 1; t0 = time.time()
try:
    for line in bot.stdout:
        line = line.rstrip()
        if line.startswith("CMD "):
            n0 = len(log); srv.stdin.write(line[4:] + "\n"); srv.stdin.flush(); print("   > " + line[4:])
            time.sleep(0.25)
            for l in log[n0:]:
                if "Done" not in l: print("     | " + l.rstrip()[-110:])
        else: print(line)
        if time.time() - t0 > 120: print("TIMEOUT"); bot.kill(); break
    bot.wait(); rc = bot.returncode
finally:
    try: srv.stdin.write("stop\n"); srv.stdin.flush()
    except Exception: pass
    try: srv.wait(20)
    except Exception: srv.kill()
open(f"{root}/build/server-{ver}.log","w").writelines(log)
bad = [l for l in log if "ERROR" in l and "FileNotFound" not in l and "banned" not in l]
if bad: print("server errors:", *bad[:5], sep="\n  ")
print("RESULT", "OK" if rc == 0 else "FAILED", ver); sys.exit(rc)
