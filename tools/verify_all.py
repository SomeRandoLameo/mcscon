#!/usr/bin/env python3
"""Full verification: build, unit tests (ctest), then per server version the scenario, packet-coverage and edge-case runs
against real local servers (127.0.0.1 only).  usage: verify_all.py [version-dir ...]   (default: all six)"""
import os, re, subprocess, sys, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALL = {"1.10.2": ("1.10", 25610), "1.11": ("1.11", 25613), "1.11.2": ("1.11.2", 25611), "1.12": ("1.12", 25614), "1.12.1": ("1.12.1", 25615), "1.12.2": ("1.12.2", 25612)}
vers = sys.argv[1:] or list(ALL)
def run(cmd, **kw): return subprocess.run(cmd, capture_output=True, text=True, cwd=root, **kw)
results = []
def rec(name, ok, detail=""): results.append((name, ok, detail)); print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)

r = run(["cmake", "--build", "build", "-j4"]); rec("build", r.returncode == 0)
r = run(["ctest", "--test-dir", "build"]); m = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", r.stdout); rec("unit tests (ctest)", r.returncode == 0, m.group(0) if m else "")
for v in vers:
    bv, port = ALL[v]
    r = run(["python3", "tools/integ_session.py", v]); m = re.search(r"SUMMARY pass=(\d+) fail=(\d+)", r.stdout); rec(f"{v} scenario", r.returncode == 0, m.group(0) if m else r.stdout[-200:])
    r = run(["python3", "tools/integ_coverage.py", v]); m = re.search(r"COVERAGE (\d+)/(\d+).*?failures=(\d+)", r.stdout); mm = re.search(r"MISSING:(.*)", r.stdout)
    rec(f"{v} packet coverage", r.returncode == 0 and m is not None, (m.group(0)[:60] + " | missing:" + (mm.group(1) if mm else "?")) if m else r.stdout[-200:])
    for mode in ("smallbuf", "nolimit"):
        r = run(["python3", "tools/run_with_console.py", v, str(port), "integ_edge", bv, mode]); line = [l for l in r.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]
        rec(f"{v} edge:{mode}", r.returncode == 0, line[0][5:140] if line else r.stdout[-150:])
for v in ("1.12.2", "1.10.2"):
    bv, port = ALL[v]; props = f"{root}/testserver/{v}/server.properties"; orig = open(props).read()
    open(props, "w").write(orig.replace("online-mode=false", "online-mode=true"))
    try: r = run(["python3", "tools/run_with_console.py", v, str(port), "integ_edge", bv, "online"])
    finally: open(props, "w").write(orig)
    line = [l for l in r.stdout.splitlines() if l.startswith(("PASS", "FAIL"))]; rec(f"{v} edge:online-mode server", r.returncode == 0, line[0][5:140] if line else r.stdout[-150:])
bad = [r for r in results if not r[1]]
print(f"\n{len(results) - len(bad)}/{len(results)} verification steps passed"); sys.exit(1 if bad else 0)
