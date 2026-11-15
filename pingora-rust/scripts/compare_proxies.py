#!/usr/bin/env python3
"""Compare C++ and Pingora proxy throughput using the SAME origin and wrk workload.

Measurements are only comparable within this single run. This script intentionally
does not declare a winner or compare them with historical benchmark numbers.
"""
import argparse
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "pingora-rust" / "benchmarks" / "results"
ORIGIN_PORTS = (19001, 19002)
CPP_PORT = 18080
RUST_PORT = 16188
RUST_METRICS_PORT = 19188

def command(*parts):
    return subprocess.run(parts, cwd=ROOT, check=True, text=True)

def wait_http(port, path="/health", timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            with urlopen(f"http://127.0.0.1:{port}{path}", timeout=.7) as response:
                if response.status == 200:
                    return
        except Exception:
            time.sleep(.15)
    raise RuntimeError(f"Server 127.0.0.1:{port} not ready")

def terminate(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)

def parse_wrk(output):
    def number(pattern):
        match = re.search(pattern, output, flags=re.MULTILINE)
        return float(match.group(1)) if match else None

    def latency(pattern):
        match = re.search(pattern, output, flags=re.MULTILINE)
        if not match:
            return None
        value, unit = float(match.group(1)), match.group(2)
        return round(value * {"us": .001, "ms": 1, "s": 1000}[unit], 3)

    sock = re.search(
        r"Socket errors:\s*connect\s*(\d+),\s*read\s*(\d+),\s*write\s*(\d+),\s*timeout\s*(\d+)",
        output
    )
    non2xx = number(r"Non-2xx or 3xx responses:\s*(\d+)")
    total = number(r"\s*(\d+) requests in ")
    return {
        "requests": int(total) if total is not None else None,
        "requests_per_second": number(r"Requests/sec:\s*([0-9.]+)"),
        "p50_ms": latency(r"^\s*50%\s+([0-9.]+)(us|ms|s)\s*$"),
        "p75_ms": latency(r"^\s*75%\s+([0-9.]+)(us|ms|s)\s*$"),
        "p90_ms": latency(r"^\s*90%\s+([0-9.]+)(us|ms|s)\s*$"),
        "p99_ms": latency(r"^\s*99%\s+([0-9.]+)(us|ms|s)\s*$"),
        "socket_errors": sum(map(int, sock.groups())) if sock else 0,
        "non_2xx_3xx": int(non2xx) if non2xx is not None else 0,
    }

def run_wrk(port, seconds, concurrency, threads, path):
    args = ["wrk", "-t", str(threads), "-c", str(concurrency),
            "-d", f"{seconds}s", "--latency", f"http://127.0.0.1:{port}{path}"]
    result = subprocess.run(args, text=True, capture_output=True, cwd=ROOT, timeout=seconds+35)
    if result.returncode:
        raise RuntimeError(f"wrk exited {result.returncode}: {result.stderr}\n{result.stdout}")
    metrics = parse_wrk(result.stdout)
    if any(metrics[k] is None for k in ("requests", "requests_per_second", "p50_ms", "p99_ms")):
        raise ValueError(f"Could not parse wrk output. Raw output:\n{result.stdout}\nSTDERR:\n{result.stderr}")
    return metrics, result.stdout

def run_one(name, executable, args, port, admin_port, seconds, connections, threads, path):
    print(f"Testing {name} at port {port} (cache disabled, 2 origins)...", flush=True)
    proc = subprocess.Popen([str(executable), *args], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_http(port, "/bench-warmup")
        time.sleep(2)
        run_wrk(port, 2, connections, threads, path)  # warmup, discarded
        time.sleep(.25)
        values, raw = run_wrk(port, seconds, connections, threads, path)
        with urlopen(f"http://127.0.0.1:{admin_port}/metrics", timeout=3) as f:
            admin = f.read().decode()
        return {"metrics": values, "wrk_output": raw, "proxy_metrics": admin}
    finally:
        terminate(proc)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=int, default=8)
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--path", default="/benchmark?mode=uncached")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()
    if not (3 <= args.duration <= 120 and 1 <= args.connections <= 512 and 1 <= args.threads <= 32 and 1 <= args.repeat <= 10):
        parser.error("Use duration 3-120, connections 1-512, threads 1-32, repeat 1-10")
    if not args.path.startswith("/") or "\r" in args.path or "\n" in args.path:
        parser.error("Path must begin with / and contain no newline")

    for app in ("wrk",):
        if not shutil.which(app):
            raise SystemExit(f"Missing {app}; install it before running benchmarks")
    cpp = ROOT / "build" / "edge-proxy"
    rust = ROOT / "pingora-rust" / "target" / "release" / "edgeguard-pingora"
    for binary in (cpp, rust):
        if not binary.is_file():
            raise SystemExit(f"Missing executable: {binary}; build both proxies first")
    OUT.mkdir(parents=True, exist_ok=True)
    directory = Path(args.output) if args.output else OUT / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory.mkdir(parents=True, exist_ok=True)

    origins = []
    try:
        for port, name in zip(ORIGIN_PORTS, ("a", "b")):
            origins.append(subprocess.Popen(
                [sys.executable, str(ROOT/"pingora-rust"/"scripts"/"origin.py"), "--port", str(port), "--name", name],
                cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            ))
            wait_http(port)
        config = {
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "host": {"platform":platform.platform(), "machine":platform.machine(),
                     "logical_cpus":os.cpu_count(), "python":platform.python_version()},
            "workload":{"seconds":args.duration, "connections":args.connections, "threads":args.threads,
                        "repeat":args.repeat,"path":args.path,"warmup_seconds":2,
                        "origins":["127.0.0.1:19001","127.0.0.1:19002"],
                        "origin_implementation":"Python ThreadingHTTPServer; identical for both proxies"},
            "fairness":{"proxy_mode":"HTTP/1.1 GET, round-robin, cache OFF",
                        "notes":"C++ single-thread epoll vs Pingora multithreaded; not architecture-equivalent.",
                        "measurement":"same machine/load generator; proxies tested sequentially, not simultaneously"},
            "runs":[],
        }
        for index in range(args.repeat):
            # Alternate test order to limit warm/cold-order bias.
            order = ["cpp","rust"] if index % 2 == 0 else ["rust","cpp"]
            for name in order:
                if name == "cpp":
                    result = run_one(name,cpp,[
                        "--bind","127.0.0.1","--port",str(CPP_PORT),
                        "--admin-port","19100","--policy","rr","--no-cache",
                        "--backend","127.0.0.1:19001","--backend","127.0.0.1:19002"
                    ],CPP_PORT,19100,args.duration,args.connections,args.threads,args.path)
                else:
                    env = os.environ.copy()
                    env["EDGEGUARD_LISTEN"]=f"127.0.0.1:{RUST_PORT}"
                    env["EDGEGUARD_METRICS_LISTEN"]=f"127.0.0.1:{RUST_METRICS_PORT}"
                    env["EDGEGUARD_BACKENDS"]="127.0.0.1:19001,127.0.0.1:19002"
                    # Rust environment configuration passed through process environment.
                    print(f"Testing Rust at port {RUST_PORT}...",flush=True)
                    # Use unique benchmark ports; the preceding CI integration test uses
                    # 6188/9188, and orphaned processes must not mask startup failures.
                    log_path = directory / f"{index+1}_rust_startup.log"
                    with log_path.open("w") as startup_log:
                        proc = subprocess.Popen([str(rust)],cwd=ROOT,env=env,
                                                stdout=startup_log,stderr=subprocess.STDOUT)
                    try:
                        try:
                            wait_http(RUST_METRICS_PORT,"/metrics",timeout=20)
                            # Metrics starts first, so check the traffic listener separately.
                            wait_http(RUST_PORT,"/bench-warmup",timeout=30)
                        except Exception as exc:
                            status = proc.poll()
                            logs = log_path.read_text(errors="replace")[-6000:]
                            raise RuntimeError(
                                f"Rust startup failed (exit code {status}): {exc}\n"
                                f"Captured startup log:\n{logs}"
                            ) from exc
                        time.sleep(2)
                        run_wrk(RUST_PORT,2,args.connections,args.threads,args.path)
                        time.sleep(.25)
                        m,raw=run_wrk(RUST_PORT,args.duration,args.connections,args.threads,args.path)
                        with urlopen(f"http://127.0.0.1:{RUST_METRICS_PORT}/metrics",timeout=3) as f:
                            pm=f.read().decode()
                        result={"metrics":m,"wrk_output":raw,"proxy_metrics":pm}
                    finally:
                        terminate(proc)
                entry={"iteration":index+1,"proxy":name,**result["metrics"]}
                config["runs"].append(entry)
                (directory/f"{index+1}_{name}_wrk.txt").write_text(result["wrk_output"])
                (directory/f"{index+1}_{name}_metrics.txt").write_text(result["proxy_metrics"])
                print(entry,flush=True)
        (directory/"results.json").write_text(json.dumps(config,indent=2)+"\n")
        print(f"Results saved: {directory/'results.json'}")
        print("Avoid quoting performance numbers without describing host, origin and workload.")
    finally:
        for proc in origins:
            terminate(proc)

if __name__ == "__main__":
    main()
