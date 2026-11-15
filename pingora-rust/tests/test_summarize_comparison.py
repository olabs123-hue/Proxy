import unittest
from scripts.summarize_comparison import summarize, render_markdown

class TestComparisonSummary(unittest.TestCase):
    def fixture(self):
        runs = []
        for proxy, rates in (("cpp", (5000., 5300., 5100.)), ("rust", (4900., 5000., 5200.))):
            for i, rate in enumerate(rates):
                runs.append({
                    "proxy": proxy,
                    "iteration": i + 1,
                    "requests_per_second": rate,
                    "p50_ms": 1 + i,
                    "p75_ms": 2 + i,
                    "p90_ms": 3 + i,
                    "p99_ms": 4 + i,
                    "socket_errors": 0,
                    "non_2xx_3xx": 0,
                })
        return {"workload": {"seconds": 6, "connections": 32, "threads": 2}, "runs": runs}

    def test_median_and_ranges(self):
        s = summarize(self.fixture())
        self.assertEqual(s["cpp"]["sample_count"], 3)
        self.assertEqual(s["cpp"]["metrics"]["requests_per_second"]["median"], 5100)
        self.assertEqual(s["rust"]["metrics"]["requests_per_second"]["maximum"], 5200)
        self.assertEqual(s["cpp"]["socket_errors"], 0)

    def test_missing_rust_rejected(self):
        doc = self.fixture()
        doc["runs"] = [r for r in doc["runs"] if r["proxy"] == "cpp"]
        with self.assertRaises(ValueError):
            summarize(doc)

    def test_unequal_trials_rejected(self):
        doc = self.fixture()
        doc["runs"].pop()
        with self.assertRaises(ValueError):
            summarize(doc)

    def test_markdown_discloses_limitations(self):
        doc = self.fixture()
        md = render_markdown(doc, summarize(doc))
        self.assertIn("shared runner", md)
        self.assertIn("p99", md)
        self.assertNotIn("is faster", md)
