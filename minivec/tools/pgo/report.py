#!/usr/bin/env python3
"""PGO experiment gates and bounded, downloadable evidence; no score inference."""
import hashlib
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

COMPILER = "gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818"
FLAGS = "-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc"
BOUNDARIES = ["resident30k", "resident50k", "low16", "filter60k", "update10", "spill"]


def digest(path):
    if not path.is_file():
        return None
    result = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_text(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else None


def read_json(path):
    text = read_text(path)
    if text is None:
        return None
    try:
        return json.loads(text)
    except ValueError as error:
        return {"parse_error": str(error), "file": path.name}


def write_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def checksum_list(path):
    entries = []
    for line in (read_text(path) or "").splitlines():
        value, name = line.split(None, 1)
        entries.append({"sha256": value, "path": name.strip().lstrip("*")})
    return entries


def text(value):
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else value or ""


def check_binaries(out):
    """Executed from the archived harness, before any benchmark process starts."""
    checks = {"status": "running", "checks": [],
              "scope": "Both extracted release binaries; timings here are correctness gates only."}
    write_json(out / "correctness.json", checks)
    for mode in ("baseline", "pgo"):
        binary = str(out / (mode + ".bin"))
        tests = [
            ("json_protocol", [sys.executable, "minivec/tests/json_protocol.py", "--binary", binary, "--timeout", "15"], {}),
            ("topk_serial", [sys.executable, "minivec/tests/topk_bounds.py", "--binary", binary], {"MVDB_QUERY_THREADS": "1"}),
            ("topk_two_threads", [sys.executable, "minivec/tests/topk_bounds.py", "--binary", binary], {"MVDB_QUERY_THREADS": "2"}),
            ("official_public", [sys.executable, "protocol_check/run.py", "--command",
                                 json.dumps([binary, "--data", "{data_dir}"]), "--output", str(out / ("public-" + mode))], {}),
        ]
        for label, command, overrides in tests:
            stage = "correctness-" + mode + "-" + label
            (out / "stage.txt").write_text(stage)
            started = time.monotonic()
            try:
                run = subprocess.run(command, capture_output=True, timeout=240,
                                     env={**os.environ, **overrides})
                code, stdout, stderr, failure = run.returncode, text(run.stdout), text(run.stderr), None
            except subprocess.TimeoutExpired as error:
                code, stdout, stderr, failure = None, text(error.stdout), text(error.stderr), "timeout_240_seconds"
            (out / (stage + ".stdout.log")).write_text(stdout, encoding="utf-8")
            (out / (stage + ".stderr.log")).write_text(stderr, encoding="utf-8")
            print(stage, flush=True)
            if stdout:
                print(stdout, end="" if stdout.endswith("\n") else "\n", flush=True)
            if stderr:
                print(stderr, end="" if stderr.endswith("\n") else "\n", file=sys.stderr, flush=True)
            checks["checks"].append({"variant": mode, "test": label, "returncode": code,
                "failure": failure, "elapsed_seconds": time.monotonic() - started,
                "environment_overrides": overrides,
                "stdout_sha256": hashlib.sha256(stdout.encode()).hexdigest(),
                "stderr_sha256": hashlib.sha256(stderr.encode()).hexdigest(),
                "stdout_tail": stdout[-4096:], "stderr_tail": stderr[-4096:]})
            if code != 0:
                checks["status"] = "failed"
            write_json(out / "correctness.json", checks)
            if code != 0:
                raise SystemExit("PGO correctness gate failed: " + stage)
    checks["status"] = "passed"
    write_json(out / "correctness.json", checks)


def collect(out, revision, exit_code):
    """Also works before the build finishes: absent evidence remains unproven."""
    out.mkdir(parents=True, exist_ok=True)
    stage = (read_text(out / "stage.txt") or "setup").strip()
    image_ids = {}
    for mode in ("baseline", "pgo"):
        image = read_json(out / (mode + "-image.json"))
        image_ids[mode] = image[0].get("Id") if isinstance(image, list) and image else None
    context = {
        "kind": "same_source_PGO_experiment", "source_commit": revision,
        "github_run_id": os.environ.get("GITHUB_RUN_ID"), "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "tracked_worktree_status": read_text(out / "source-worktree-status.txt"),
        "platform": platform.platform(), "cpu_info": read_text(out / "lscpu.txt"),
        "measured_affinity": read_json(out / "affinity.json"),
        "compiler_image": COMPILER, "common_flags": FLAGS,
        "variant_flags": {"baseline": [], "pgo_generate": ["-fprofile-generate=/pgo", "-fprofile-update=atomic"],
                          "pgo_use": ["-fprofile-use=/pgo", "-Werror=missing-profile", "-Werror=coverage-mismatch"]},
        "compile_workdir": "/src", "source_glob": "src/*.cpp", "both_compile_output_paths": "/minivec",
        "image_ids": image_ids,
        "binary_sha256": {mode: digest(out / (mode + ".bin")) for mode in ("baseline", "pgo")},
        "training": read_json(out / "training/manifest.json"),
        "standard": {"seeds": [7319, 8911, 13337], "pairs_per_seed": 5, "queries_per_phase": 1000, "total_pairs": 15},
        "boundaries": {"seeds": [7319, 8911, 13337], "pairs_per_seed": 5, "queries_per_phase": 64,
                       "scenarios": BOUNDARIES, "total_pairs": 90},
        "limitations": ["Same-runner shared-cache paired comparison; not formal scoring or E/M/B.",
                        "Process RSS/IO are proxies. No outlier deletion or score extrapolation.",
                        "Restart includes index rebuild; first post-UPDATE query includes lazy rebuild.",
                        "Training inputs/expected responses are deterministic, profile counters need not be.",
                        "Training runs only during image build; final release excludes corpus, helper and feedback."]}
    write_json(out / "context.json", context)
    verification = []
    for line in (read_text(out / "training/verification.jsonl") or "").splitlines():
        try:
            verification.append(json.loads(line))
        except ValueError as error:
            verification.append({"parse_error": str(error)})
    expected_names = {f"responses-{threads}-{phase}.jsonl" for threads in (1, 2) for phase in (0, 1, 2)}
    manifest = context["training"] or {}
    requests = manifest.get("phase_requests", [])
    training_ok = (len(verification) == 6 and len(requests) == 3 and
                   {entry.get("actual_file") for entry in verification} == expected_names and
                   all(entry.get("status") == "passed" and
                       entry.get("responses") == requests[int(entry["actual_file"].split("-")[-1].split(".")[0])] + 1
                       for entry in verification))
    profile_entries = checksum_list(out / "training/profile-sha256.txt")
    source_entries = checksum_list(out / "source-sha256.txt")
    profile_checks = [{**entry, "copied_sha256": digest(out / "training/profile-data" / entry["path"].removeprefix("/pgo/"))}
                      for entry in profile_entries]
    source_tus = sum(entry["path"].endswith(".cpp") for entry in source_entries)
    profiles_ok = bool(profile_checks) and len(profile_checks) == source_tus and all(
        item["sha256"] == item["copied_sha256"] for item in profile_checks)
    packaging = {}
    for mode in ("baseline", "pgo"):
        names = [(name[2:] if name.startswith("./") else name).lstrip("/")
                 for name in (read_text(out / (mode + "-files.txt")) or "").splitlines()]
        packaging[mode] = bool(names) and "minivec" in names and not any(
            name == "pgo" or name.startswith("pgo/") or name == "training" or name.startswith("training/") for name in names)
    correctness = read_json(out / "correctness.json") or {}
    tests_ok = correctness.get("status") == "passed" and len(correctness.get("checks", [])) == 8
    benchmark_counts = {}
    for name, expected in (("standard", 15), ("boundaries", 90)):
        report = read_json(out / (name + ".json")) or {}
        benchmark_counts[name] = {"status": report.get("status"), "pairs": len(report.get("pairs", [])), "expected_pairs": expected}
    benchmarks_ok = all(record["status"] == "passed" and record["pairs"] == record["expected_pairs"]
                        for record in benchmark_counts.values())
    proof = {"status": "passed" if exit_code == 0 and training_ok and profiles_ok and tests_ok and
             all(packaging.values()) and (benchmarks_ok or stage == "complete-build-only") else "incomplete_or_failed",
        "same_source_commit": revision, "independent_training_oracle_passed": training_ok,
        "training_runs": verification, "training_expected_response_counts": [n + 1 for n in requests],
        "profile_files_match_source_translation_unit_count_and_hashes": profiles_ok,
        "source_translation_units": source_tus, "profile_files": profile_checks,
        "strict_feedback_compilation_and_release_build_completed": image_ids.get("pgo") is not None,
        "missing_profile_and_coverage_mismatch_configured_as_errors": True,
        "profile_correction_enabled": False, "fast_math_enabled": False,
        "release_excludes_training_and_profile_data": packaging,
        "both_release_binaries_passed_eight_correctness_gates": tests_ok,
        "benchmark_counts": benchmark_counts, "requested_pair_counts_completed": benchmarks_ok}
    write_json(out / "proof.json", proof)
    status = {"exit_code": exit_code, "stage": stage, "proof_status": proof["status"]}
    write_json(out / "run-status.json", status)
    artifacts = []
    for path in sorted(out.rglob("*")):
        if path.is_file() and path.name != "hashes.json":
            artifacts.append({"path": path.relative_to(out).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)})
    write_json(out / "hashes.json", {"source_inputs": source_entries,
        "experiment_inputs": checksum_list(out / "draft-sha256.txt"),
        "training_inputs": checksum_list(out / "training/corpus-sha256.txt"), "artifacts": artifacts,
        "note": "hashes.json excludes itself; paths in compiler-stage manifests identify build-local files."})
    return proof["status"] == "passed"


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "check-binaries":
        check_binaries(Path(sys.argv[2]).resolve())
    elif len(sys.argv) == 5 and sys.argv[1] == "collect":
        passed = collect(Path(sys.argv[2]).resolve(), sys.argv[3], int(sys.argv[4]))
        if int(sys.argv[4]) == 0 and not passed:
            raise SystemExit("PGO proof is incomplete despite a zero process exit code")
    else:
        raise SystemExit("usage: report.py check-binaries OUT | report.py collect OUT COMMIT EXIT_CODE")
