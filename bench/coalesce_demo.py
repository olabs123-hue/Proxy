#!/usr/bin/env python3
"""Fires N simultaneous requests for one URL through the proxy, then asks the
origin how many requests it actually received."""
import argparse
import asyncio
import collections
import time


async def one(host, port, path, results):
    try:
        reader, writer = await asyncio.open_connection(host, port)
        writer.write(f"GET {path} HTTP/1.1\r\nHost: demo\r\nConnection: close\r\n\r\n".encode())
        await writer.drain()
        raw = await reader.read()
        head = raw.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
        status = int(head.split(" ", 2)[1])
        cache = "?"
        for line in head.split("\r\n")[1:]:
            if line.lower().startswith("x-cache:"):
                cache = line.split(":", 1)[1].strip()
        results.append((status, cache))
        writer.close()
    except Exception as e:  # noqa: BLE001
        results.append((0, type(e).__name__))


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy", default="127.0.0.1:18080")
    ap.add_argument("--origin", default="127.0.0.1:19010")
    ap.add_argument("--clients", type=int, default=1000)
    ap.add_argument("--path", default="/hot-item")
    a = ap.parse_args()
    ph, pp = a.proxy.rsplit(":", 1)
    oh, op = a.origin.rsplit(":", 1)
    results = []
    t0 = time.time()
    await asyncio.gather(*[one(ph, int(pp), a.path, results) for _ in range(a.clients)])
    elapsed = time.time() - t0
    by = collections.Counter(results)
    print(f"  {a.clients} clients finished in {elapsed:.2f}s")
    for (status, cache), n in sorted(by.items(), key=lambda kv: -kv[1]):
        print(f"  status={status} x-cache={cache}: {n}")
    r, w = await asyncio.open_connection(oh, int(op))
    w.write(b"GET /_stats HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    await w.drain()
    body = (await r.read()).split(b"\r\n\r\n", 1)[1].decode().strip()
    print(f"  origin received: {body}  (for {a.clients} client requests)")


asyncio.run(main())
