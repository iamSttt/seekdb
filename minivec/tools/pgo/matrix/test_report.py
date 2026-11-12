#!/usr/bin/env python3
"""Evidence-gate tests using synthetic reports; these are not DB benchmarks."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

import report


class EvidenceGates(unittest.TestCase):
    def paired_report(self):
        pairs = [{"scenario": "standard10k", "seed": seed, "pair_index": pair,
                  "all_responses_identical": True,
                  "baseline": {"response_sha256": "matching"}, "candidate": {"response_sha256": "matching"}}
                 for seed in report.SEEDS for pair in range(5)]
        return {"status": "passed", "seeds": list(report.SEEDS), "pairs_per_seed": 5,
                "queries_per_phase": 1000, "binaries": {"baseline": {"sha256": "base"}, "candidate": {"sha256": "candidate"}},
                "pairs": pairs}

    def paired_passes(self, value):
        return report.benchmark_proof(value, "base", "candidate", ["standard10k"], 5, 1000)["passed"]

    def test_complete_pairs_pass(self):
        self.assertTrue(self.paired_passes(self.paired_report()))

    def test_duplicate_pair_cannot_replace_missing_seed(self):
        value = self.paired_report()
        value["pairs"][-1] = copy.deepcopy(value["pairs"][0])
        self.assertFalse(self.paired_passes(value))

    def test_wrong_binary_partial_failure_and_response_mismatch_fail(self):
        for mutation in (lambda value: value["binaries"]["candidate"].update(sha256="different"),
                         lambda value: value.update(status="failed"),
                         lambda value: value["pairs"].pop(),
                         lambda value: value["pairs"][0]["candidate"].update(response_sha256="wrong")):
            value = self.paired_report()
            mutation(value)
            self.assertFalse(self.paired_passes(value))

    def test_raw_oracle_rejects_trailing_response_and_forged_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected, actual = root / "expected", root / "actual"
            expected.write_text('{"type":"ready"}\n{"id":1,"expect_error":true,"ok":false}\n')
            matching = '{"type":"ready"}\n{"id":1,"ok":false,"error":{"code":"SQL","message":"bad query"}}\n'
            actual.write_text(matching)
            self.assertTrue(report.responses_match(expected, actual))
            actual.write_text(matching + '{"id":2,"ok":true}\n')
            self.assertFalse(report.responses_match(expected, actual))
            actual.write_text(matching.replace('"code":"SQL"', '"code":""'))
            self.assertFalse(report.responses_match(expected, actual))

    def test_missing_evidence_fails_with_zero_exit_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertFalse(report.collect(root, "commit", 0))
            proof = json.loads((root / "proof.json").read_text())
            self.assertEqual(proof["status"], "incomplete_or_failed")
            self.assertFalse(proof["all_six_builds_and_24_correctness_gates_passed"])

    def test_missing_training_is_not_valid_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            proof = report.training_proof(Path(directory), True, 10)
            self.assertFalse(proof["passed"])
            self.assertFalse(proof["profile_hashes_and_translation_unit_count_match"])
            self.assertFalse(proof["spill_passed"])

    def test_valid_training_then_corrupt_profile_and_extra_response(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "corpus"
            profiles = root / "profile-data"
            corpus.mkdir(); profiles.mkdir()
            response = '{"type":"ready"}\n{"id":1,"ok":true}\n'
            report.save(corpus / "manifest.json", {"seed": 2026111201, "phase_requests": [1, 1, 1]})
            records = []
            for phase in range(3):
                (corpus / f"phase-{phase}.jsonl").write_text('{"id":1,"op":"shutdown"}\n')
                (corpus / f"phase-{phase}.expected.jsonl").write_text(response)
                for threads in (1, 2):
                    name = f"responses-{threads}-{phase}.jsonl"
                    (root / name).write_text(response)
                    records.append({"status": "passed", "actual_file": name, "responses": 2})
            (root / "verification.jsonl").write_text("\n".join(json.dumps(record) for record in records) + "\n")
            (root / "corpus-sha256.txt").write_text("\n".join(
                f"{report.digest(path)}  /training/corpus/{path.name}" for path in sorted(corpus.iterdir())) + "\n")
            profile = profiles / "one.gcda"
            profile.write_bytes(b"synthetic profile used only for hash-gate test")
            (root / "profile-sha256.txt").write_text(f"{report.digest(profile)}  /pgo/one.gcda\n")
            report.save(root / "original-corpus-match.json", {"status": "passed", "files_compared": 7})
            report.save(root / "training-cost.json", {"spill_enabled": 0, "base_nanoseconds": 1,
                "spill_extension_nanoseconds": 0, "total_nanoseconds": 1})
            self.assertTrue(report.training_proof(root, False, 1)["passed"])
            self.assertFalse(report.training_proof(root, False, 2)["passed"])
            profile.write_bytes(b"changed")
            self.assertFalse(report.training_proof(root, False, 1)["profile_hashes_and_translation_unit_count_match"])
            (root / "responses-1-0.jsonl").write_text(response + '{"id":2,"ok":true}\n')
            self.assertFalse(report.training_proof(root, False, 1)["base_oracle"])


if __name__ == "__main__":
    unittest.main()
