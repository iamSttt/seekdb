#!/usr/bin/env python3
"""Portable regressions for evidence tools; does not launch or benchmark the DB.

Run from any directory: python3 -B /path/to/minivec/tests/test_iteration_tools.py
All fixture writes use temporary directories; no report or bytecode is written
into the checkout. Controlled process results test failure/report orchestration,
not database behavior or performance.
"""

import contextlib
import copy
from fractions import Fraction
import hashlib
import importlib.util
import io
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parents[1] / "tools"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


benchmark = load_tool("benchmark_iteration")
material = load_tool("validate_iteration_material")


class OracleTests(unittest.TestCase):
    def test_generated_queries_against_fraction_oracle(self):
        # Fractions reconstruct exact binary32 distances independently of the
        # implementation's math.fsum accumulation and heap selection.
        for spec in ((17, 16, 3, 0), (17, 16, 0, 0, 5)):
            workload = benchmark.make_workload(spec, 8911, 5, 7, 6)
            groups = [key % 5 for key in range(17)] if len(spec) > 4 else None
            if groups:
                groups[1] = 0
            for phase in ("before_oracle", "after_oracle"):
                for index, expected in workload[phase].items():
                    literal = re.search(r"L2_DISTANCE\(v,'(\[[^]]*\])'", workload["queries"][index]).group(1)
                    query = list(map(Fraction.from_float, json.loads(literal)))
                    ranked = []
                    for key, vector in enumerate(workload["vectors"]):
                        if groups is not None and groups[key] != index % 5:
                            continue
                        if phase == "after_oracle" and key < spec[2]:
                            vector = workload["updated_vector"]
                        distance = sum((Fraction.from_float(float(x)) - y) ** 2
                                       for x, y in zip(vector, query))
                        ranked.append((distance, key))
                    self.assertEqual([key for _, key in sorted(ranked)[:6]],
                                     [key for _, key in expected])

    def test_filtered_ties_and_sparse_group_cardinality(self):
        workload = benchmark.make_workload((11, 16, 0, 0, 5), 7319, 4, 4, 10)
        self.assertEqual(workload["query_counts"], [4, 1, 2, 2])
        self.assertEqual([key for _, key in workload["before_oracle"][0]][:2], [0, 1])
        self.assertEqual([key for _, key in workload["before_oracle"][1]], [6])
        self.assertEqual(sum(count for _, count in workload["inserts"]), 11)

    def test_update_changes_expected_rank_and_tie_order(self):
        workload = benchmark.make_workload((12, 16, 3, 0), 7319, 4, 5, 10)
        self.assertEqual(workload["after_oracle"][1][:3], [(0.0, 0), (0.0, 1), (0.0, 2)])
        self.assertNotEqual(workload["before_oracle"][1], workload["after_oracle"][1])

    def test_seed_controls_fingerprint(self):
        args = ((12, 16, 0, 0), 7319, 4, 5, 10)
        first = benchmark.make_workload(*args)
        repeated = benchmark.make_workload(*args)
        changed = benchmark.make_workload(args[0], 8911, *args[2:])
        self.assertEqual(first["sha256"], repeated["sha256"])
        self.assertNotEqual(first["sha256"], changed["sha256"])

    def test_corrupt_query_results_are_rejected(self):
        expected = [(0.0, 2), (0.0, 5), (25.0, 9)]
        valid = {"columns": ["id", "d"], "rows": [[2, 0.0], [5, 0.0], [9, 5.0]]}
        benchmark.verify_query(valid, expected, "valid fixture")
        corruptions = [
            {"columns": ["d", "id"], "rows": valid["rows"]},
            {"columns": valid["columns"], "rows": [[5, 0.0], [2, 0.0], [9, 5.0]]},
            {"columns": valid["columns"], "rows": [[2, 0.0], [2, 0.0], [9, 5.0]]},
            {"columns": valid["columns"], "rows": valid["rows"][:2]},
            {"columns": valid["columns"], "rows": [[True, 0.0], [5, 0.0], [9, 5.0]]},
        ]
        for distance in (float("nan"), float("inf"), -5.0, 25.0, "5", True):
            bad = copy.deepcopy(valid)
            bad["rows"][2][1] = distance
            corruptions.append(bad)
        for bad in corruptions:
            with self.subTest(response=bad), self.assertRaises(ValueError):
                benchmark.verify_query(bad, expected, "corrupt fixture")


class ReportTests(unittest.TestCase):
    def test_median_uses_each_pair_and_keeps_regressions(self):
        pairs = [{"scenario": "s", "baseline": {"phase_seconds": {"q": old}},
                  "candidate": {"phase_seconds": {"q": new}}}
                 for old, new in ((2, 1), (100, 100), (4, 1), (1, 2), (2, 1))]
        summary = benchmark.paired_summary(pairs)["s"]["q"]
        self.assertEqual(summary, {"pairs": 5, "median_baseline_over_candidate": 2.0,
                                   "minimum": 0.5, "maximum": 4.0})

    def test_report_write_rejects_nonfinite_without_losing_prior_report(self):
        with tempfile.TemporaryDirectory(prefix="mvdb-tools-report-") as directory:
            path = Path(directory) / "result.json"
            original = {"status": "running", "official_score": None, "pairs": []}
            benchmark.write_report(path, original)
            for number in (float("nan"), float("inf"), -float("inf")):
                with self.subTest(number=number), self.assertRaises(ValueError):
                    benchmark.write_report(path, {"status": "passed", "seconds": number})
                self.assertEqual(json.loads(path.read_text()), original)
                self.assertFalse(path.with_name("result.json.tmp").exists())

    def run_fake_pairs(self, directory, outcomes, count):
        # Patch only host/process boundaries: production main still builds the
        # workload, alternates runs, compares fingerprints and persists failures.
        baseline = Path(directory) / "baseline.bin"
        candidate = Path(directory) / "candidate.bin"
        output = Path(directory) / "result.json"
        baseline.write_bytes(b"baseline test placeholder")
        candidate.write_bytes(b"candidate test placeholder")
        argv = ["benchmark_iteration", "--baseline", str(baseline), "--candidate", str(candidate),
                "--output", str(output), "--suite", "smoke", "--scenario", "resident",
                "--queries", "2", "--pairs", str(count)]
        with patch.object(sys, "argv", argv), patch.object(sys, "platform", "linux"), \
                patch.object(benchmark.os, "access", return_value=True), \
                patch.object(benchmark.os, "sched_getaffinity", return_value={0, 1}, create=True), \
                patch.object(benchmark, "one_run", side_effect=outcomes), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            error = None
            try:
                benchmark.main()
            except (ValueError, TimeoutError) as exc:
                error = exc
        return json.loads(output.read_text()), error

    @staticmethod
    def run_result(fingerprint):
        return {"phase_seconds": {"query": 1.0}, "response_sha256": fingerprint}

    def test_mismatched_output_never_becomes_successful_performance_pair(self):
        with tempfile.TemporaryDirectory(prefix="mvdb-tools-mismatch-") as directory:
            report, error = self.run_fake_pairs(directory, [self.run_result("a"), self.run_result("b")], 1)
        self.assertIsInstance(error, ValueError)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["pairs"], [])
        self.assertIsNone(report["official_score"])
        self.assertIn("response mismatch", report["error"])

    def test_later_timeout_preserves_verified_pairs_and_failure_status(self):
        with tempfile.TemporaryDirectory(prefix="mvdb-tools-timeout-") as directory:
            report, error = self.run_fake_pairs(directory, [self.run_result("a"), self.run_result("a"),
                                                            self.run_result("a"), TimeoutError("test timeout")], 2)
        self.assertIsInstance(error, TimeoutError)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(report["pairs"]), 1)
        self.assertTrue(report["pairs"][0]["all_responses_identical"])
        self.assertEqual(report["summary"]["resident"]["query"]["pairs"], 1)
        self.assertIsNone(report["official_score"])


class MaterialTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mvdb-tools-material-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo"
        (self.root / "agent").mkdir(parents=True)
        self.payload = b"fixture evidence, not an executed database experiment\n"
        (self.root / "evidence.txt").write_bytes(self.payload)
        self.record = {
            "base_commit": "a90b2b750e40f020793cddefe01d5b66631afc63",
            "messages": [{"id": "u", "role": "user", "content": "Proposed a test."},
                         {"id": "a", "role": "assistant", "content": "Ran a test."}],
            "adopted_directions": [{"source_message": "u", "origin_role": "user",
                                    "implementation_role": "assistant"}],
            "experiments": [{"executed_by": "assistant", "artifacts": [
                {"path": "evidence.txt", "sha256": hashlib.sha256(self.payload).hexdigest()}]}],
        }

    def save(self, record=None, raw=None):
        data = raw if raw is not None else json.dumps(
            record if record is not None else self.record, ensure_ascii=False).encode("utf-8")
        (self.root / "agent/conversation.json").write_bytes(data)
        (self.root / "conversation.json").write_bytes(data)
        return data

    def rejected(self, record=None, raw=None):
        self.save(record, raw)
        with self.assertRaises((ValueError, KeyError, TypeError, OSError)):
            material.validate(self.root)

    def test_valid_material_fingerprints_bytes_and_copies(self):
        data = self.save()
        result = material.validate(self.root)
        self.assertEqual(result["bytes"], len(data))
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual((result["messages"], result["experiments"]), (2, 1))

    def test_different_material_copies_rejected(self):
        self.save()
        (self.root / "conversation.json").write_bytes(b"{}")
        with self.assertRaises(ValueError):
            material.validate(self.root)

    def test_duplicate_json_key_rejected(self):
        data = self.save()
        self.rejected(raw=b'{"base_commit":"unexpected",' + data[1:])

    def test_duplicate_message_id_rejected(self):
        self.record["messages"].append(copy.deepcopy(self.record["messages"][0]))
        self.rejected()

    def test_unknown_message_role_rejected(self):
        self.record["messages"][0]["role"] = "system"
        self.rejected()

    def test_nontext_or_empty_message_rejected(self):
        for content in (None, [], ""):
            with self.subTest(content=content):
                self.record["messages"][0]["content"] = content
                self.rejected()

    def test_assistant_source_cannot_be_attributed_to_user(self):
        self.record["adopted_directions"][0]["source_message"] = "a"
        self.rejected()

    def test_missing_source_reference_rejected(self):
        self.record["adopted_directions"][0]["source_message"] = "missing"
        self.rejected()

    def test_implementation_cannot_claim_user_role(self):
        self.record["adopted_directions"][0]["implementation_role"] = "user"
        self.rejected()

    def test_experiment_executor_cannot_claim_user_role(self):
        self.record["experiments"][0]["executed_by"] = "user"
        self.rejected()

    def test_changed_artifact_rejected(self):
        self.save()
        (self.root / "evidence.txt").write_bytes(b"changed evidence")
        with self.assertRaises(ValueError):
            material.validate(self.root)

    def test_parent_escape_rejected_even_with_matching_hash(self):
        (self.root.parent / "outside.txt").write_bytes(self.payload)
        self.record["experiments"][0]["artifacts"][0]["path"] = "../outside.txt"
        self.rejected()

    def test_limit_counts_utf8_bytes_not_characters(self):
        self.record["messages"][0]["content"] = "中" * 50000
        self.rejected()

    def test_nonfinite_json_tokens_and_exponent_overflow_rejected(self):
        base = self.save()
        for token in (b"NaN", b"Infinity", b"-Infinity", b"1e400", b"-1e400"):
            with self.subTest(token=token):
                self.rejected(raw=b'{"unused_numeric_field":' + token + b"," + base[1:])

    def test_optimized_python_cannot_disable_material_guards(self):
        self.record["adopted_directions"][0]["source_message"] = "a"
        self.save()
        code = ("import importlib.util,sys;from pathlib import Path;"
                "s=importlib.util.spec_from_file_location('validator',sys.argv[2]);"
                "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                "m.validate(Path(sys.argv[1]))")
        run = subprocess.run([sys.executable, "-B", "-O", "-c", code, str(self.root),
                              str(TOOLS / "validate_iteration_material.py")],
                             capture_output=True, text=True, timeout=15)
        self.assertNotEqual(run.returncode, 0, "python -O silently accepted a false user attribution")
        self.assertIn("ValueError", run.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
