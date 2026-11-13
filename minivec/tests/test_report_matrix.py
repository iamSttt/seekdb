#!/usr/bin/env python3
"""Independent workload/oracle checks; does not execute the Linux database."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import benchmark_report_matrix as matrix


class MatrixTests(unittest.TestCase):
    def test_reversed_conjunction_preserves_oracle(self):
        left = matrix.make_workload((128, 16, 10, 0, 'compound'), 7319, 8, 20)
        right = matrix.make_workload((128, 16, 10, 0, 'reverse'), 7319, 8, 20)
        self.assertEqual(left['before_oracle'], right['before_oracle'])
        self.assertNotEqual(left['queries'], right['queries'])
        self.assertNotEqual(left['sha256'], right['sha256'])
        self.assertEqual(left['before_oracle'][0], [(0.0, 0)])
        self.assertEqual(left['query_counts'], [1]*8)

    def test_empty_and_full_predicates(self):
        empty = matrix.make_workload((32, 16, 10, 0, 'zero'), 8911, 4, 20)
        full = matrix.make_workload((32, 16, 100, 0, 'high'), 8911, 4, 20)
        self.assertEqual(empty['query_counts'], [0]*4)
        self.assertTrue(all(not result for result in empty['before_oracle'].values()))
        self.assertEqual(full['query_counts'], [32]*4)
        # Planted duplicate vectors still tie by ascending primary key.
        self.assertEqual(full['before_oracle'][0][:2], [(0.0, 0), (0.0, 1)])

    def test_nominal_selectivity_and_range_endpoints(self):
        labels = [(key % 2, (key // 2) % 1000) for key in range(10000)]
        _, match = matrix.predicate('compound', 0)
        self.assertEqual(sum(match(a, b) for a, b in labels), 5)
        for kind, count in [('one_percent', 100), ('ten_percent', 1000), ('high', 10000), ('zero', 0)]:
            _, match = matrix.predicate(kind, 0)
            self.assertEqual(sum(match(a, b) for a, b in labels), count)
        _, match = matrix.predicate('range', 1)
        self.assertFalse(match(0, 1))
        self.assertFalse(match(1, 0))
        self.assertTrue(match(1, 1))
        self.assertTrue(match(1, 10))
        self.assertFalse(match(1, 11))

    def test_spill_encoding_and_all_queries_oracled(self):
        work = matrix.make_workload((32, 16, 10, 8192, 'range'), 13337, 5, 10)
        self.assertIn('pad VARCHAR(8192),a BIGINT,b BIGINT', work['create'])
        self.assertEqual(sum(count for _, count in work['inserts']), 32)
        self.assertEqual(set(work['before_oracle']), set(range(5)))
        self.assertEqual(work['before_oracle'], work['after_oracle'])


if __name__ == '__main__':
    unittest.main()
