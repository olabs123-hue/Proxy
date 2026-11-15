"""Black-box smoke test with two healthy origin processes."""
import argparse
import collections
import json
import urllib.request

def check(base, n):
    counts = collections.Counter()
    for i in range(n):
        with urllib.request.urlopen(f"{base.rstrip('/')}/test?i={i}", timeout=3) as response:
            assert response.status == 200
            body = json.load(response)
            assert body["path"] == f"/test?i={i}", body
            counts[body["origin"]] += 1
    print("Origin distribution:", dict(counts))
    assert len(counts) >= 2, "Expected two healthy origins"
    return counts

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="http://127.0.0.1:6188")
    p.add_argument("--requests", type=int, default=30)
    args = p.parse_args()
    assert args.requests >= 2
    check(args.url, args.requests)
    print("PASS")
