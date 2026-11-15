"""Black-box failover exercise with two origins and the Pingora proxy.

Starts on ports 9001/9002, kills origin A, checks that requests reach origin B,
restarts origin A, and verifies both origins are available again.
"""
import json
import os
import signal
import subprocess
import sys
import time
from urllib.request import urlopen

BASE = "http://127.0.0.1:6188"
METRICS = "http://127.0.0.1:9188/metrics"

def get(url):
    with urlopen(url, timeout=3) as response:
        if response.status != 200:
            raise AssertionError((url, response.status))
        return response.read().decode()

def origin(port, name):
    return subprocess.Popen([sys.executable, "scripts/origin.py", "--port", str(port), "--name", name],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

def wait_origin(port, deadline=10):
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        try:
            json.loads(get(f"http://127.0.0.1:{port}/health"))
            return
        except Exception:
            time.sleep(.15)
    raise RuntimeError(f"Origin {port} did not start")

def origins_seen(num):
    seen = set()
    for index in range(num):
        seen.add(json.loads(get(f"{BASE}/probe?seq={index}"))["origin"])
    return seen

def main():
    # Called after the CI job starts the Pingora proxy. Startup is measured separately.
    a = origin(9001, "origin-a")
    b = origin(9002, "origin-b")
    try:
        wait_origin(9001)
        wait_origin(9002)
        time.sleep(3)
        assert origins_seen(30) == {"origin-a", "origin-b"}, "Both origins must receive traffic"
        a.terminate()
        a.wait(timeout=5)
        time.sleep(4)  # background health checks converge
        assert origins_seen(30) == {"origin-b"}, "Failed origin must be removed from rotation"
        a = origin(9001, "origin-a")
        wait_origin(9001)
        time.sleep(4)
        assert origins_seen(40) == {"origin-a", "origin-b"}, "Restored origin must receive traffic"
        metrics = get(METRICS)
        assert "edgeguard_requests_total " in metrics
        assert "edgeguard_request_duration_seconds_count " in metrics
        print("PASS: balanced traffic, failover, restoration, metrics")
    finally:
        for proc in (a,b):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()

if __name__ == "__main__":
    main()
