import unittest
from scripts.compare_proxies import parse_wrk

SAMPLE = """
Running 5s test @ http://127.0.0.1:6188/test
  2 threads and 32 connections
  Latency Distribution
     50%    1.52ms
     75%    2.00ms
     90%    3.00ms
     95%    4.61ms
     99%    9.25ms
  18000 requests in 5.00s, 3.33MB read
Requests/sec:   3600.00
Transfer/sec:  681.98KB
"""
class TestParseWrk(unittest.TestCase):
    def test_success(self):
        v=parse_wrk(SAMPLE)
        self.assertEqual(v["requests"],18000)
        self.assertEqual(v["requests_per_second"],3600.0)
        self.assertEqual(v["p50_ms"],1.52)
        self.assertEqual(v["p95_ms"],4.61)
        self.assertEqual(v["p99_ms"],9.25)
        self.assertEqual(v["socket_errors"],0)
    def test_error_counts_and_units(self):
        text=SAMPLE.replace("1.52ms","520.00us").replace("9.25ms","0.01s")+"\nSocket errors: connect 2, read 1, write 0, timeout 1\nNon-2xx or 3xx responses: 8\n"
        v=parse_wrk(text)
        self.assertEqual(v["p50_ms"],.52)
        self.assertEqual(v["p99_ms"],10.)
        self.assertEqual(v["socket_errors"],4)
        self.assertEqual(v["non_2xx_3xx"],8)
