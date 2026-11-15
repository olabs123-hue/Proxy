#!/usr/bin/env python3
"""Summarize repeated wrk runs without making cross-machine performance claims."""
import argparse
import json
import math
import statistics
from pathlib import Path

METRICS = ("requests_per_second", "p50_ms", "p75_ms", "p90_ms", "p99_ms")

def summarize(document):
    runs = document.get("runs", [])
    summary = {}
    for name in ("cpp", "rust"):
        group = [x for x in runs if x.get("proxy") == name]
        if not group:
            raise ValueError(f"No completed {name} runs")
        metrics = {}
        for metric in METRICS:
            vals = [float(x[metric]) for x in group]
            if not all(math.isfinite(v) and v >= 0 for v in vals):
                raise ValueError(f"Invalid {metric} values for {name}")
            metrics[metric] = {
                "median": round(statistics.median(vals), 4),
                "minimum": round(min(vals), 4),
                "maximum": round(max(vals), 4),
            }
        summary[name] = {
            "sample_count": len(group),
            "metrics": metrics,
            "socket_errors": sum(int(x.get("socket_errors", 0)) for x in group),
            "non_2xx_3xx": sum(int(x.get("non_2xx_3xx", 0)) for x in group),
        }
    if summary["cpp"]["sample_count"] != summary["rust"]["sample_count"]:
        raise ValueError("Unequal completed trial counts; report would be misleading")
    return summary

def render_markdown(document, summary):
    trials = summary["cpp"]["sample_count"]
    workload = document.get("workload", {})
    lines = [
        "# C++17 vs Rust/Pingora — repeated CI measurements",
        "",
        "**Experimental only.** These runs were recorded sequentially on the same shared runner,",
        "not on dedicated hardware. Do not infer which programming language or framework is faster.",
        "Both implementations use two Python origins; C++ caching is disabled, but proxy",
        "architecture and concurrency strategies are not identical.",
        "",
        f"- Trials per proxy: **{trials}**",
        f"- Measured seconds per trial: **{workload.get('seconds', 'unknown')}**",
        f"- Connections: **{workload.get('connections', 'unknown')}**",
        f"- wrk threads: **{workload.get('threads', 'unknown')}**",
        "",
        "| Metric | C++ median [min, max] | Rust median [min, max] |",
        "|---|---:|---:|",
    ]
    labels = {
        "requests_per_second": "Requests/sec",
        "p50_ms": "p50 (ms)",
        "p75_ms": "p75 (ms)",
        "p90_ms": "p90 (ms)",
        "p99_ms": "p99 (ms)",
    }
    for key in METRICS:
        def fmt(proxy):
            row = summary[proxy]["metrics"][key]
            return f"{row['median']:,.2f} [{row['minimum']:,.2f}, {row['maximum']:,.2f}]"
        lines.append(f"| {labels[key]} | {fmt('cpp')} | {fmt('rust')} |")
    lines.extend([
        f"| Socket errors (all trials) | {summary['cpp']['socket_errors']} | {summary['rust']['socket_errors']} |",
        f"| Non-2xx/3xx responses (all trials) | {summary['cpp']['non_2xx_3xx']} | {summary['rust']['non_2xx_3xx']} |",
        "",
        "**Limitations:** Short shared-CI trials, workload contention, and non-equivalent proxy internals",
        "make these results useful for experimentation, not ranking implementations.",
        "Per-run wrk outputs, startup logs and proxy metrics are retained alongside results.json.",
        "The wrk p95 percentile is unavailable and is not estimated.",
        "",
    ])
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results_json", type=Path)
    args = parser.parse_args()
    content = json.loads(args.results_json.read_text())
    summary = summarize(content)
    target = args.results_json.parent
    (target / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    report = render_markdown(content, summary)
    (target / "summary.md").write_text(report)
    print(report)

if __name__ == "__main__":
    main()
