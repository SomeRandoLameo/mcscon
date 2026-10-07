#!/usr/bin/env python3
"""Packet-coverage run: tools/integ_coverage.py <version-dir>. Sets a resource pack so ResourcePackSend is exercised."""
import os, subprocess, sys, threading, time, shutil, re
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver = sys.argv[1]
botver = {"1.10.2": "1.10", "1.11": "1.11", "1.11.2": "1.11.2", "1.12": "1.12", "1.12.1": "1.12.1", "1.12.2": "1.12.2"}[ver]
port = {"1.10.2": 25610, "1.11.2": 25611, "1.12.2": 25612, "1.11": 25613, "1.12": 25614, "1.12.1": 25615}[ver]
sd = f"{root}/testserver/{ver}"; props = f"{sd}/server.properties"
shutil.rmtree(f"{sd}/world", ignore_errors=True)
orig = open(props).read()
open(props, "w").write(re.sub(r"^resource-pack.*\n", "", orig, flags=re.M) + "resource-pack=http://127.0.0.1:9/none.zip\nresource-pack-sha1=0000000000000000000000000000000000000000\nmax-players=10\n")
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
    bot = subprocess.Popen([f"{root}/build/integ_coverage", str(port), botver], stdout=subprocess.PIPE, text=True)
    t0 = time.time()
    for line in bot.stdout:
        line = line.rstrip()
        if line.startswith("CMD "): srv.stdin.write(line[4:] + "\n"); srv.stdin.flush()
        else: print(line)
        if time.time() - t0 > 300: print("TIMEOUT"); bot.kill(); break
    bot.wait(); rc = bot.returncode
finally:
    open(props, "w").write(orig)
    try: srv.stdin.write("stop\n"); srv.stdin.flush()
    except Exception: pass
    try: srv.wait(20)
    except Exception: srv.kill()
    open(f"{root}/build/coverage-{ver}.log", "w").writelines(log)
errs = [l for l in log if ("Exception" in l or "ERROR" in l) and "FileNotFound" not in l]
if errs: print("server errors:", *[e.strip()[-100:] for e in errs[:4]], sep="\n  ")
print("RESULT", "OK" if rc == 0 else "FAILED", ver); sys.exit(rc)
