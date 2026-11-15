"""Summarize repeated origin-failure trials with explicit observational limits."""
import argparse
import json
import statistics
from pathlib import Path

def compute(rows):
    output={}
    for name in ("cpp","rust"):
        group=[r for r in rows if r["proxy"]==name]
        if not group:
            raise ValueError("Missing measurements for "+name)
        delays=[r["observation"]["origin_a_first_seen_after_restore_s"] for r in group
                if r["observation"]["origin_a_first_seen_after_restore_s"] is not None]
        peaks=[r["process_resources"]["rss_peak_bytes"] for r in group
               if r.get("process_resources",{}).get("rss_peak_bytes") is not None]
        cpus=[r["process_resources"]["cpu_mean_one_core_percent"] for r in group
              if r.get("process_resources",{}).get("cpu_mean_one_core_percent") is not None]
        output[name]={
            "trials":len(group),
            "outage_requests":sum(r["observation"]["outage"]["requests"] for r in group),
            "outage_errors":sum(r["observation"]["outage"]["errors"] for r in group),
            "observed_return_median_s":round(statistics.median(delays),4) if delays else None,
            "observed_return_range_s":[min(delays),max(delays)] if delays else None,
            "mean_cpu_one_core_percent_median":round(statistics.median(cpus),2) if cpus else None,
            "peak_rss_mib_median":round(statistics.median(peaks)/1048576,2) if peaks else None,
        }
    if output["cpp"]["trials"]!=output["rust"]["trials"]:
        raise ValueError("Unequal trial counts")
    return output

def render(rows, result):
    lines=[
        "# Phase 6: repeated outage and process-resource observations",
        "",
        "Synthetic, shared-runner experiments. NOT production performance rankings or SLAs.",
        "CPU percentage is relative to a single logical core; RSS reflects process resident pages.",
        "Both implementations were tested sequentially with the same local Python origins.",
        "",
        "| Metric | C++17 | Rust/Pingora |",
        "|---|---:|---:|",
    ]
    labels=[
        ("trials","Completed trials"),
        ("outage_requests","Outage requests"),
        ("outage_errors","Outage errors"),
        ("observed_return_median_s","Median first observed origin return (s)"),
        ("observed_return_range_s","Origin return range (s)"),
        ("mean_cpu_one_core_percent_median","Median of trial CPU means (% of one core)"),
        ("peak_rss_mib_median","Median of trial peak RSS (MiB)"),
    ]
    for key,label in labels:
        lines.append(f"| {label} | {result['cpp'][key]} | {result['rust'][key]} |")
    lines.extend([
        "",
        "Observed origin return is measured from the successful origin restart until the",
        "first routed response; it is NOT exact internal health-check recovery time.",
        "Resource samples include test startup and runtime and exclude child origins.",
        "The benchmark uses few threads and short windows, and cannot support",
        "a general claim that one implementation is faster or more reliable.",
        "Raw request events, metrics and resources are provided in the artifact.",
        "",
    ])
    return "\n".join(lines)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("path",type=Path)
    a=p.parse_args()
    data=json.loads(a.path.read_text())
    result=compute(data["results"])
    (a.path.parent/"phase6_summary.json").write_text(json.dumps(result,indent=2)+"\n")
    report=render(data["results"],result)
    (a.path.parent/"phase6_summary.md").write_text(report)
    print(report)

if __name__=="__main__":
    main()
