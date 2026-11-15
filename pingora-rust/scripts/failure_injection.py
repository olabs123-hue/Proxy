#!/usr/bin/env python3
"""Controlled origin-failure exercise for C++ and Rust/Pingora proxies.

Runs sequentially on the same CI host. This is a low-concurrency reliability
experiment, NOT a production SLA or precise failover-time benchmark.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
ORIGINS = (19501, 19502)
CONFIG = {
    "cpp": (ROOT / "build" / "edge-proxy", 18580, 19580),
    "rust": (ROOT / "pingora-rust" / "target" / "release" / "edgeguard-pingora", 16588, 19588),
}

def stop_process(p):
    if p is None or p.poll() is not None:
        return
    p.terminate()
    try:
        p.wait(timeout=4)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait(timeout=4)

def wait_endpoint(url, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urlopen(url, timeout=0.6) as r:
                if r.status == 200:
                    return
        except Exception:
            time.sleep(.15)
    raise RuntimeError(f"Endpoint not ready: {url}")

def summarize_events(records, outage_s, restore_s):
    phases = {"baseline": [], "outage": [], "recovery": []}
    for r in records:
        phase = "baseline" if r["offset_s"] < outage_s else (
            "outage" if r["offset_s"] < restore_s else "recovery"
        )
        phases[phase].append(r)
    result = {}
    for name, items in phases.items():
        result[name] = {
            "requests": len(items),
            "successes": sum(x["ok"] for x in items),
            "errors": sum(not x["ok"] for x in items),
            "observed_origin_a": sum(x.get("origin") == "a" for x in items),
            "observed_origin_b": sum(x.get("origin") == "b" for x in items),
        }
    after = [r["offset_s"] for r in records
             if r["offset_s"] >= restore_s and r.get("origin") == "a" and r["ok"]]
    result["origin_a_first_seen_after_restore_s"] = (
        round(min(after) - restore_s, 4) if after else None
    )
    # This is observed routing recovery, not a precise backend health-check duration.
    return result

def test_one(name, duration, concurrency, output):
    binary, port, metrics_port = CONFIG[name]
    if not binary.exists():
        raise FileNotFoundError(f"Build {binary} before testing")
    origins = []
    proxy = None
    abort = threading.Event()
    records = []
    lock = threading.Lock()
    logs = output / f"{name}_startup.log"
    try:
        for backend_port, label in zip(ORIGINS, ("a", "b")):
            p = subprocess.Popen(
                [sys.executable, str(ROOT/"pingora-rust"/"scripts"/"origin.py"),
                 "--port", str(backend_port), "--name", label],
                cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            origins.append(p)
            wait_endpoint(f"http://127.0.0.1:{backend_port}/health")

        if name == "cpp":
            args = [str(binary), "--bind","127.0.0.1","--port",str(port),
                    "--admin-port",str(metrics_port),"--policy","rr","--no-cache",
                    "--health-interval","1",
                    "--backend",f"127.0.0.1:{ORIGINS[0]}",
                    "--backend",f"127.0.0.1:{ORIGINS[1]}"]
            env = os.environ.copy()
        else:
            args = [str(binary)]
            env = os.environ.copy()
            env.update({
                "EDGEGUARD_LISTEN":f"127.0.0.1:{port}",
                "EDGEGUARD_METRICS_LISTEN":f"127.0.0.1:{metrics_port}",
                "EDGEGUARD_BACKENDS":f"127.0.0.1:{ORIGINS[0]},127.0.0.1:{ORIGINS[1]}",
            })
        with logs.open("w") as fp:
            proxy = subprocess.Popen(args, cwd=ROOT, env=env,
                                     stdout=fp, stderr=subprocess.STDOUT)
        try:
            wait_endpoint(f"http://127.0.0.1:{metrics_port}/metrics")
            wait_endpoint(f"http://127.0.0.1:{port}/test-ready")
        except Exception as exc:
            raise RuntimeError(f"{name} failed to start: {exc}; log={logs.read_text()[-3000:]}") from exc
        time.sleep(3)  # allow initial health checks to observe both origins

        start = time.monotonic()
        outage_s = 2.0
        restore_s = 6.0

        def worker(worker_id):
            n = 0
            while not abort.is_set():
                n += 1
                try:
                    with urlopen(f"http://127.0.0.1:{port}/failure-test?worker={worker_id}&n={n}",
                                 timeout=1.8) as response:
                        data = json.loads(response.read().decode())
                        ok = response.status == 200 and data.get("origin") in ("a", "b")
                        origin = data.get("origin")
                        error = None if ok else "unexpected response"
                except Exception as exc:
                    ok, origin, error = False, None, type(exc).__name__
                entry = {"offset_s":round(time.monotonic()-start,4),
                         "ok":ok,"origin":origin,"error":error}
                with lock:
                    records.append(entry)
                # Limit the observer's own CPU usage on shared runners.
                time.sleep(.015)

        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
            tasks = [pool.submit(worker, i) for i in range(concurrency)]
            while time.monotonic() - start < outage_s:
                time.sleep(.05)
            crash_at = time.monotonic()-start
            stop_process(origins[0])
            while time.monotonic() - start < restore_s:
                time.sleep(.05)
            origins[0] = subprocess.Popen(
                [sys.executable, str(ROOT/"pingora-rust"/"scripts"/"origin.py"),
                 "--port",str(ORIGINS[0]),"--name","a"],
                cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
            wait_endpoint(f"http://127.0.0.1:{ORIGINS[0]}/health")
            restored_at = time.monotonic()-start
            while time.monotonic()-start < duration:
                time.sleep(.05)
            abort.set()
            for task in tasks:
                task.result(timeout=6)
        records.sort(key=lambda x:x["offset_s"])
        with urlopen(f"http://127.0.0.1:{metrics_port}/metrics",timeout=3) as response:
            metrics=response.read().decode()
        (output / f"{name}_events.json").write_text(json.dumps(records,indent=2)+"\n")
        (output / f"{name}_metrics.txt").write_text(metrics)
        summary=summarize_events(records,crash_at,restored_at)
        return {"proxy":name,"failure_injected_s":round(crash_at,4),
                "origin_restart_s":round(restored_at,4),
                "duration_s":duration,"observation":summary,
                "warning":"Low-concurrency shared-runner synthetic origins; first observed restoration is not exact health-check recovery."}
    finally:
        abort.set()
        stop_process(proxy)
        for p in origins:
            stop_process(p)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration",type=float,default=11)
    parser.add_argument("--concurrency",type=int,default=4)
    parser.add_argument("--output",default="benchmarks/results/failure-injection")
    args=parser.parse_args()
    if not (10 <= args.duration <= 45 and 1 <= args.concurrency <= 16):
        parser.error("duration must be 10-45 and concurrency 1-16")
    output=Path(args.output)
    output.mkdir(parents=True,exist_ok=True)
    results={"timestamp_utc":datetime.now(timezone.utc).isoformat(),
             "environment":"CI/local shared-host synthetic benchmark",
             "concurrency":args.concurrency,"results":[]}
    try:
        for name in ("cpp","rust"):
            print("Testing origin failure:",name,flush=True)
            row=test_one(name,args.duration,args.concurrency,output)
            results["results"].append(row)
            print(json.dumps(row,indent=2),flush=True)
    finally:
        (output/"failure_report.json").write_text(json.dumps(results,indent=2)+"\n")
    print("Saved",output/"failure_report.json")

if __name__=="__main__":
    main()
