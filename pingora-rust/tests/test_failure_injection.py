import unittest
from scripts.failure_injection import summarize_events

class TestFailureSummary(unittest.TestCase):
    def test_phase_classification(self):
        records=[
            {"offset_s":1.0,"ok":True,"origin":"a"},
            {"offset_s":3.0,"ok":False,"origin":None},
            {"offset_s":3.5,"ok":True,"origin":"b"},
            {"offset_s":6.1,"ok":True,"origin":"b"},
            {"offset_s":7.25,"ok":True,"origin":"a"},
        ]
        s=summarize_events(records,2.0,6.0)
        self.assertEqual(s["baseline"]["requests"],1)
        self.assertEqual(s["outage"]["errors"],1)
        self.assertEqual(s["outage"]["observed_origin_b"],1)
        self.assertEqual(s["recovery"]["observed_origin_a"],1)
        self.assertAlmostEqual(s["origin_a_first_seen_after_restore_s"],1.25)

    def test_no_recovery_is_null(self):
        s=summarize_events([{"offset_s":8,"ok":True,"origin":"b"}],2,6)
        self.assertIsNone(s["origin_a_first_seen_after_restore_s"])
