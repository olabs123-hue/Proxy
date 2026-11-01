#!/usr/bin/env bash
# Benchmarks for edge-proxy. Requires: wrk, python3, curl. Build first:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# Usage: bench/run_bench.sh [duration_seconds] [connections]
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
DUR=${1:-8}
CONNS=${2:-64}
OUT=bench/results
mkdir -p "$OUT"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

start_backends() {
  for i in 1 2 3; do
    $BUILD/fake-backend --port $((19000 + i)) --id "b$i" >/dev/null 2>&1 &
    BPIDS[$i]=$!; PIDS+=("$!")
  done
  sleep 0.3
}
start_proxy() {  # args: extra proxy flags
  $BUILD/edge-proxy --port 18080 --admin-port 19100 \
    --backend 127.0.0.1:19001 --backend 127.0.0.1:19002 --backend 127.0.0.1:19003 "$@" >/dev/null 2>&1 &
  PROXY_PID=$!; PIDS+=("$PROXY_PID")
  sleep 0.4
}
stop_proxy() { kill "$PROXY_PID" 2>/dev/null; wait "$PROXY_PID" 2>/dev/null; sleep 0.3; }

summ() {  # print the interesting wrk lines
  grep -E "Requests/sec|50%|99%|Non-2xx|Socket errors|requests in" "$1" | sed 's/^ */  /'
}

{
echo "=== environment ==="
echo "cpus: $(nproc)   kernel: $(uname -r)   $(g++ --version | head -1)"
echo "load generator (wrk), proxy and fake origins all share this machine"
echo "duration: ${DUR}s per scenario, ${CONNS} connections, 1 wrk thread"
} | tee "$OUT/summary.txt"

start_backends

echo; echo "=== 1. baseline: wrk -> one origin directly (no proxy) ===" | tee -a "$OUT/summary.txt"
wrk -t1 -c"$CONNS" -d"${DUR}s" --latency http://127.0.0.1:19001/static > "$OUT/1_direct.txt" 2>&1
summ "$OUT/1_direct.txt" | tee -a "$OUT/summary.txt"

echo; echo "=== 2. proxy, cache ON (every request after the first is a cache hit) ===" | tee -a "$OUT/summary.txt"
start_proxy
BEFORE=$(curl -s http://127.0.0.1:19001/_stats | sed 's/hits=//')
wrk -t1 -c"$CONNS" -d"${DUR}s" --latency http://127.0.0.1:18080/static > "$OUT/2_cache_on.txt" 2>&1
summ "$OUT/2_cache_on.txt" | tee -a "$OUT/summary.txt"
HITS=$(curl -s http://127.0.0.1:19100/metrics | awk '/^proxy_cache_hits_total/{print $2}')
MISS=$(curl -s http://127.0.0.1:19100/metrics | awk '/^proxy_cache_misses_total/{print $2}')
AFTER=$(curl -s http://127.0.0.1:19001/_stats | sed 's/hits=//')
echo "  cache hits=$HITS misses=$MISS; requests that reached the origin during this run: $((AFTER - BEFORE))" | tee -a "$OUT/summary.txt"
stop_proxy

echo; echo "=== 3. proxy, cache OFF (every request is proxied to an origin, round-robin over 3) ===" | tee -a "$OUT/summary.txt"
start_proxy --no-cache
wrk -t1 -c"$CONNS" -d"${DUR}s" --latency http://127.0.0.1:18080/static > "$OUT/3_cache_off.txt" 2>&1
summ "$OUT/3_cache_off.txt" | tee -a "$OUT/summary.txt"
stop_proxy

echo; echo "=== 4. failover: cache OFF, origin b2 is killed 3s into the run ===" | tee -a "$OUT/summary.txt"
start_proxy --no-cache
( sleep 3; kill -9 "${BPIDS[2]}" 2>/dev/null ) &
wrk -t1 -c"$CONNS" -d"$((DUR + 2))s" --latency http://127.0.0.1:18080/static > "$OUT/4_failover.txt" 2>&1
summ "$OUT/4_failover.txt" | tee -a "$OUT/summary.txt"
M=$(curl -s http://127.0.0.1:19100/metrics)
echo "  upstream errors:  $(echo "$M" | awk '/^proxy_upstream_errors_total/{print $2}')" | tee -a "$OUT/summary.txt"
echo "  upstream retries: $(echo "$M" | awk '/^proxy_upstream_retries_total/{print $2}')" | tee -a "$OUT/summary.txt"
echo "  5xx to clients:   $(echo "$M" | awk '/class="5xx"/{print $2}')" | tee -a "$OUT/summary.txt"
echo "$M" | grep 'proxy_backend_up' | sed 's/^/  /' | tee -a "$OUT/summary.txt"
stop_proxy

echo; echo "=== 5. coalescing: 1000 simultaneous requests for one uncached URL (origin delay 300ms) ===" | tee -a "$OUT/summary.txt"
$BUILD/fake-backend --port 19010 --id slow --delay-ms 300 >/dev/null 2>&1 & PIDS+=("$!")
sleep 0.3
$BUILD/edge-proxy --port 18080 --admin-port 19100 --backend 127.0.0.1:19010 >/dev/null 2>&1 &
PROXY_PID=$!; PIDS+=("$PROXY_PID"); sleep 0.4
python3 bench/coalesce_demo.py --proxy 127.0.0.1:18080 --origin 127.0.0.1:19010 --clients 1000 | tee -a "$OUT/summary.txt"
