#!/usr/bin/env python3
"""Checks of the benchmark oracle; no database or speed claims."""
import copy
from fractions import Fraction
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import benchmark_projection as projection


class ProjectionOracle(unittest.TestCase):
    def test_rejects_precision_type_zero_sign_and_string_corruption(self):
        expected = [[projection.LOW, projection.HIGH, "中\0\n😀", [-0.0, projection.F32_TINY, projection.F32_MAX]]]
        projection.verify_value(json.loads(projection.encoded(expected)), expected, "rows")
        for mutate in (lambda rows: rows[0].__setitem__(1, float(projection.HIGH)),
                       lambda rows: rows[0].__setitem__(0, True),
                       lambda rows: rows[0].__setitem__(2, "中\n😀"),
                       lambda rows: rows[0][3].__setitem__(0, 0.0),
                       lambda rows: rows[0][3].__setitem__(1, 0.0),
                       lambda rows: rows.pop()):
            bad = copy.deepcopy(expected)
            mutate(bad)
            with self.assertRaises(AssertionError):
                projection.verify_value(bad, expected, "rows")

    def test_distance_order_agrees_with_independent_rational_oracle(self):
        projection.CASES["tiny_test_only"] = (32, 16, 96)
        try:
            work = projection.workload("tiny_test_only", 7319)
            all_rows = next(phase["rows"] for phase in work["phases"] if phase["label"] == "wide_all")
            nearest = next(phase["rows"] for phase in work["phases"] if phase["label"] == "wide_distance10")
            exact = sorted(all_rows, key=lambda row: (sum(Fraction(value) ** 2 for value in row[3][4:]), row[0]))[:10]
            self.assertEqual([row[0] for row in nearest], [row[0] for row in exact])
            self.assertEqual(all_rows[0][0], projection.LOW)
            self.assertEqual(all_rows[-1][0], projection.HIGH)
        finally:
            del projection.CASES["tiny_test_only"]


if __name__ == "__main__":
    unittest.main()
