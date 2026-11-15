"""Health-check smoke test; start proxy + origins, stop one origin, wait 3s."""
import argparse
import collections
import json
import urllib.request

def sample(url, n):
    counts = collections.Counter()
    errors = 0
    for i in range(n):
        try:
            with urllib.request.urlopen(f"{url.rstrip('/')}/failover?i={i}", timeout=2) as r:
                counts[json.load(r)["origin"]] += 1
        except Exception:
            errors += 1
    return counts, errors

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="http://127.0.0.1:6188")
    p.add_argument("--requests", type=int, default=30)
    p.add_argument("--expected-origin", default=None)
    args = p.parse_args()
    dist, errors = sample(args.url, args.requests)
    print(f"Distribution: {dict(dist)}, errors: {errors}")
    if args.expected_origin:
        assert not errors and set(dist) == {args.expected_origin}
