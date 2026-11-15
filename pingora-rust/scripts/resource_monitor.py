"""Approximate Linux /proc process CPU and RSS monitor; no third-party packages."""
import os
import statistics
from pathlib import Path
import threading
import time

def snapshot(pid):
    base = Path("/proc") / str(pid)
    # /proc/pid/stat's comm field may contain spaces; split after final ")".
    fields = (base / "stat").read_text().rsplit(") ", 1)[1].split()
    ticks = int(fields[11]) + int(fields[12])  # stat utime and stime
    pages = int((base / "statm").read_text().split()[1])
    return ticks, pages * os.sysconf("SC_PAGE_SIZE")

def summarize_samples(samples):
    cpu = [x["cpu_one_core_percent"] for x in samples
           if x["cpu_one_core_percent"] is not None]
    mem = [x["rss_bytes"] for x in samples]
    return {
        "samples": len(samples),
        "cpu_mean_one_core_percent": round(statistics.mean(cpu), 2) if cpu else None,
        "cpu_peak_one_core_percent": round(max(cpu), 2) if cpu else None,
        "rss_peak_bytes": max(mem) if mem else None,
        "rss_mean_bytes": round(statistics.mean(mem)) if mem else None,
        "note": "Linux process-only samples; 100% CPU = one saturated logical core. Not system-wide usage.",
    }

class ProcessSampler:
    def __init__(self, pid, interval_s=0.2):
        self.pid = pid
        self.interval_s = interval_s
        self.samples = []
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)
        return summarize_samples(self.samples)

    def _run(self):
        previous = None
        hz = os.sysconf("SC_CLK_TCK")
        while not self._stop.is_set():
            stamp = time.monotonic()
            try:
                ticks, rss = snapshot(self.pid)
            except (OSError, ValueError, IndexError):
                break
            cpu = None
            if previous:
                elapsed = stamp - previous[0]
                if elapsed > 0:
                    cpu = round(max(0, ticks-previous[1])*100/(hz*elapsed),2)
            self.samples.append({
                "monotonic_s": stamp,
                "cpu_one_core_percent": cpu,
                "rss_bytes": rss,
            })
            previous = (stamp, ticks)
            self._stop.wait(self.interval_s)
