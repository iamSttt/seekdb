#!/usr/bin/env python3
"""Independent fixtures for material attachment hashes and existing citations."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "validate_material", Path(__file__).resolve().parents[1] / "tools" / "validate_material.py")
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)
PAYLOAD = b'{"result":"pass"}\n'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


def document(revision="1" * 40):
    return {"artifact_integrity": {"hash_basis": "git_blob", "artifact_revision": revision},
            "experiments": [{"artifacts": [{"path": "reports/result.json", "sha256": DIGEST}]}]}


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mvdb-material-archive-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        (self.repo / "reports").mkdir()
        (self.repo / "reports/result.json").write_bytes(PAYLOAD.replace(b"\n", b"\r\n"))

    def test_archive_content_is_qualified_and_lf_normalized(self):
        result = validator.inspect_artifacts(document(), self.repo)
        self.assertEqual(result["count"], 1)
        self.assertEqual(result["mode"], "archive_lf_content_only")
        self.assertFalse(result["git_revisions_verified"])

    def test_archive_tamper_is_rejected(self):
        (self.repo / "reports/result.json").write_bytes(b'{"result":"changed"}\r\n')
        with self.assertRaisesRegex(ValueError, "sha256 mismatch"):
            validator.inspect_artifacts(document(), self.repo)

    def test_parent_absolute_drive_and_ads_paths_are_rejected(self):
        for path in ("../outside.json", "reports/../result.json", "/outside.json",
                     "C:/outside.json", "reports\\result.json", "reports/result.json:stream",
                     "reports//result.json", "./reports/result.json"):
            with self.subTest(path=path):
                d = document()
                d["experiments"][0]["artifacts"][0]["path"] = path
                with self.assertRaisesRegex(ValueError, "unsafe artifact path"):
                    validator.inspect_artifacts(d, self.repo)

    def test_symlink_cannot_escape_archive(self):
        with tempfile.TemporaryDirectory(prefix="mvdb-material-outside-") as outside:
            target = Path(outside) / "result.json"
            target.write_bytes(PAYLOAD)
            link = self.repo / "reports/link.json"
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError) as exc:
                self.skipTest(f"symlink unavailable: {exc}")
            d = document()
            d["experiments"][0]["artifacts"][0]["path"] = "reports/link.json"
            with self.assertRaisesRegex(ValueError, "outside the archive"):
                validator.inspect_artifacts(d, self.repo)

    def test_hash_basis_and_immutable_revision_required(self):
        for field, invalid in (("hash_basis", "working_tree"), ("artifact_revision", "HEAD")):
            d = document()
            d["artifact_integrity"][field] = invalid
            with self.assertRaises(ValueError):
                validator.inspect_artifacts(d, self.repo)

    def test_per_artifact_declaration_and_historical_locators(self):
        d = document()
        d["experiments"][0]["artifacts"][0].update(d.pop("artifact_integrity"))
        d["historical_source"] = {"path": "/other-host/unavailable.json", "sha256": "old-source"}
        self.assertEqual(validator.inspect_artifacts(d, self.repo)["count"], 1)

    def test_missing_archive_file_fails(self):
        (self.repo / "reports/result.json").unlink()
        with self.assertRaises(FileNotFoundError):
            validator.inspect_artifacts(document(), self.repo)


@unittest.skipUnless(shutil.which("git"), "Git is required for Git-blob fixtures")
class GitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mvdb-material-git-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        (self.repo / "reports").mkdir()
        (self.repo / "reports/result.json").write_bytes(PAYLOAD)
        self.git("-c", "core.autocrlf=false", "add", "reports/result.json")
        self.git("-c", "user.name=Material Test", "-c", "user.email=material-test@invalid.local",
                 "commit", "-qm", "fixture")
        self.revision = self.git("rev-parse", "HEAD").decode("ascii").strip()
        self.doc = document(self.revision)

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-c", f"safe.directory={self.repo.as_posix()}", "-C", str(self.repo), *args],
            stderr=subprocess.PIPE)

    def test_crlf_worktree_uses_exact_fixed_blob(self):
        (self.repo / "reports/result.json").write_bytes(PAYLOAD.replace(b"\n", b"\r\n"))
        result = validator.inspect_artifacts(self.doc, self.repo)
        self.assertEqual(result["mode"], "git_blob")
        self.assertTrue(result["git_revisions_verified"])
        # A declaration for a historical commit must not silently track HEAD.
        (self.repo / "reports/result.json").write_bytes(b"later worktree contents\n")
        self.git("-c", "core.autocrlf=false", "add", "reports/result.json")
        self.git("-c", "user.name=Material Test", "-c", "user.email=material-test@invalid.local",
                 "commit", "-qm", "later")
        self.assertEqual(validator.inspect_artifacts(self.doc, self.repo)["count"], 1)

    def test_worktree_crlf_digest_is_not_a_git_blob_digest(self):
        d = copy.deepcopy(self.doc)
        d["experiments"][0]["artifacts"][0]["sha256"] = hashlib.sha256(
            PAYLOAD.replace(b"\n", b"\r\n")).hexdigest()
        with self.assertRaisesRegex(ValueError, "sha256 mismatch"):
            validator.inspect_artifacts(d, self.repo)

    def test_tampered_digest_is_rejected(self):
        d = copy.deepcopy(self.doc)
        d["experiments"][0]["artifacts"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "sha256 mismatch"):
            validator.inspect_artifacts(d, self.repo)

    def test_unavailable_revision_uses_head_blob_with_explicit_limit(self):
        d = document("f" * 40)
        (self.repo / "reports/result.json").write_bytes(b"uncommitted mismatch\n")
        result = validator.inspect_artifacts(d, self.repo)
        self.assertEqual(result["mode"], "git_blob_current_head_fallback")
        self.assertFalse(result["git_revisions_verified"])
        self.assertEqual(result["unverified_revisions"], ["f" * 40])
        self.assertEqual(result["resolved_revisions"], {"f" * 40: self.revision})

    def test_shallow_clone_checks_bytes_without_claiming_old_commit(self):
        # Keep artifact bytes unchanged in a newer commit, then clone only that
        # commit. The declared historical object really is absent in the clone.
        (self.repo / "marker").write_bytes(b"second commit\n")
        self.git("add", "marker")
        self.git("-c", "user.name=Material Test", "-c", "user.email=material-test@invalid.local",
                 "commit", "-qm", "newer artifact container")
        with tempfile.TemporaryDirectory(prefix="mvdb-material-shallow-") as target:
            shallow = Path(target) / "checkout"
            self.git("clone", "-q", "--depth=1", "--no-local", self.repo.as_uri(), str(shallow))
            self.assertTrue((shallow / ".git/shallow").is_file())
            result = validator.inspect_artifacts(self.doc, shallow)
            self.assertFalse(result["git_revisions_verified"])
            self.assertEqual(result["unverified_revisions"], [self.revision])
            changed = copy.deepcopy(self.doc)
            changed["experiments"][0]["artifacts"][0]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "sha256 mismatch"):
                validator.inspect_artifacts(changed, shallow)

    def test_existing_noncommit_revision_is_rejected(self):
        blob = self.git("rev-parse", "HEAD:reports/result.json").decode("ascii").strip()
        with self.assertRaisesRegex(ValueError, "does not name a commit"):
            validator.inspect_artifacts(document(blob), self.repo)

    def test_unavailable_git_does_not_claim_archive_success(self):
        with mock.patch.object(validator.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "git is unavailable"):
                validator.inspect_artifacts(self.doc, self.repo)


class ExistingChecksTests(unittest.TestCase):
    def test_duplicate_keys_remain_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            json.loads('{"id":1,"id":2}', object_pairs_hook=validator.unique_object)

    def test_quote_mismatch_remains_rejected(self):
        d = {"message": "actual words", "evidence": {"json_pointer": "/message", "quote": "changed"}}
        with self.assertRaisesRegex(ValueError, "quote differs"):
            validator.inspect_value(d, d)


if __name__ == "__main__":
    unittest.main()
