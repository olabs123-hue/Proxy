import os
import unittest
from scripts.resource_monitor import snapshot, summarize_samples

class TestResourceMonitor(unittest.TestCase):
    def test_snapshot_self(self):
        ticks, rss = snapshot(os.getpid())
        self.assertGreaterEqual(ticks, 0)
        self.assertGreater(rss, 0)

    def test_summary(self):
        sample = [
            {"cpu_one_core_percent":None,"rss_bytes":100},
            {"cpu_one_core_percent":20.0,"rss_bytes":200},
            {"cpu_one_core_percent":40.0,"rss_bytes":300},
        ]
        out=summarize_samples(sample)
        self.assertEqual(out["samples"],3)
        self.assertEqual(out["rss_peak_bytes"],300)
        self.assertEqual(out["cpu_mean_one_core_percent"],30)

    def test_empty(self):
        out=summarize_samples([])
        self.assertIsNone(out["rss_peak_bytes"])
        self.assertIsNone(out["cpu_peak_one_core_percent"])
