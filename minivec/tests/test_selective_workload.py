#!/usr/bin/env python3
"""Independent Fraction check of the experimental dyadic benchmark oracle."""
from array import array
from fractions import Fraction
import json
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import benchmark_selective_scale as benchmark


class WorkloadChecks(unittest.TestCase):
    def test_canonical_dyadic_oracle_and_pk_ties(self):
        benchmark.CASES["tiny_oracle_only"] = (64, 16, "tail_outlier")
        try:
            work = benchmark.make_workload("tiny_oracle_only", 991337, 4, 16)
            for index, sql in enumerate(work["queries"]):
                query = json.loads(re.search(r"L2_DISTANCE\(v,'(\[[^]]+\])'\)", sql).group(1))
                exact = sorted((float(sum((Fraction(left) - Fraction(right)) ** 2
                                          for left, right in zip(vector, query))), key)
                               for key, vector in enumerate(work["vectors"]))[:10]
                self.assertEqual(exact, work["before_oracle"][index])
            self.assertEqual([key for _, key in work["before_oracle"][0]][:2], [0, 1])
        finally:
            del benchmark.CASES["tiny_oracle_only"]

    def test_weighted_gate_prediction_boundary(self):
        fine = array("f", [31.75] * 128)
        coarse = array("f", [127.0] * 128)
        for count, fine_count, allowed in ((1280, 1024, True), (1281, 1024, False), (257, 256, True)):
            result = benchmark.predicted_gate([fine] * fine_count + [coarse] * (count - fine_count))
            self.assertEqual(result["selective_eligible_expected"], allowed)
            self.assertEqual(result["substantially_finer_rows"], fine_count)
        result = benchmark.predicted_gate([array("f", [31.75] * 16)] * 1024 + [array("f", [127.] * 16)] * 256)
        self.assertFalse(result["selective_eligible_expected"])


if __name__ == "__main__":
    unittest.main()
