#!/usr/bin/env python3
"""Fail-closed evidence collection for the isolated same-source compiler matrix."""
import hashlib
import itertools
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

VARIANTS = ("o3", "lto", "pgo", "pgo_spill", "pgo_lto", "pgo_lto_spill")
COMPARISONS = (("o3", "lto"), ("o3", "pgo"), ("o3", "pgo_lto"),
               ("pgo", "pgo_spill"), ("pgo_lto", "pgo_lto_spill"))
BOUNDARIES = ("resident30k", "low16", "filter60k", "update10", "spill")
SEEDS = (7319, 8911, 13337)


def digest(path):
    if not path.is_file():
        return None
    result = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def load(path, default=None):
    try:
        return json.loads(read(path)) if path.is_file() else default
    except (ValueError, OSError) as error:
        return {"parse_error": str(error)}


def save(path, value):
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temp.replace(path)


def lines(path):
    return [json.loads(line) for line in read(path).splitlines()]


def checksums(path):
    return [{"sha256": line.split(None, 1)[0], "path": line.split(None, 1)[1].strip().lstrip("*")}
            for line in read(path).splitlines()]


def init(out, revision):
    save(out / "context.json", {
        "kind": "same_source_compiler_and_training_matrix", "source_commit": revision,
        "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "platform": platform.platform(), "cpu_info": read(out / "lscpu.txt"),
        "compiler": "gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818",
        "common_flags": "-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc",
        "lto_flag": "-flto=2", "profile_generate_flags": ["-fprofile-generate=/pgo", "-fprofile-update=atomic"],
        "profile_use_flags": ["-fprofile-use=/pgo", "-Werror=missing-profile", "-Werror=coverage-mismatch"],
        "compile_workdir": "/src", "both_compile_output_paths": "/minivec", "source_glob": "src/*.cpp",
        "variants": list(VARIANTS), "comparisons": COMPARISONS,
        "training_seeds": [2026111201, 2026111301], "evaluation_seeds": SEEDS,
        "limitations": ["Experimental: the production Dockerfile and training corpus are unchanged.",
            "No score conversion. Process RSS/IO are proxies; cache is shared, not evicted.",
            "All pairs and failures are retained; build failures/timeouts are not performance samples.",
            "Boundary screen is three pairs by default, not a confirmed performance result.",
            "55-minute script limit leaves five minutes in a 60-minute CI job for evidence upload.",
            "Wide strings force actual spill but can bias parser profiles; compare original and extended training."]})


def check_binaries(out):
    checks = {"status": "running", "checks": []}
    save(out / "correctness.json", checks)
    for variant in VARIANTS:
        binary = str(out / variant / "minivec")
        tests = [
            ("json_protocol", [sys.executable, "minivec/tests/json_protocol.py", "--binary", binary, "--timeout", "15"], {}),
            ("topk_serial", [sys.executable, "minivec/tests/topk_bounds.py", "--binary", binary], {"MVDB_QUERY_THREADS": "1"}),
            ("topk_two_threads", [sys.executable, "minivec/tests/topk_bounds.py", "--binary", binary], {"MVDB_QUERY_THREADS": "2"}),
            ("official_public", [sys.executable, "protocol_check/run.py", "--command", json.dumps([binary, "--data", "{data_dir}"]),
                                 "--output", str(out / variant / "public")], {})]
        for label, command, overrides in tests:
            stage = "correctness-" + variant + "-" + label
            (out / "stage.txt").write_text(stage)
            started = time.monotonic()
            try:
                run = subprocess.run(command, capture_output=True, timeout=240, env={**os.environ, **overrides})
                code, stdout, stderr = run.returncode, run.stdout, run.stderr
            except subprocess.TimeoutExpired as error:
                code, stdout, stderr = None, error.stdout or b"", error.stderr or b""
            (out / variant / (label + ".stdout.log")).write_bytes(stdout)
            (out / variant / (label + ".stderr.log")).write_bytes(stderr)
            checks["checks"].append({"variant": variant, "test": label, "returncode": code,
                "elapsed_seconds": time.monotonic() - started, "environment_overrides": overrides,
                "stdout_sha256": hashlib.sha256(stdout).hexdigest(), "stderr_sha256": hashlib.sha256(stderr).hexdigest()})
            print(stage, "returncode", code, flush=True)
            if code != 0:
                checks["status"] = "failed"
                print(stdout.decode("utf-8", "replace"), stderr.decode("utf-8", "replace"), flush=True)
            save(out / "correctness.json", checks)
            if code != 0:
                raise SystemExit("Matrix correctness gate failed: " + stage)
    checks["status"] = "passed"
    save(out / "correctness.json", checks)


def responses_match(expected_path, actual_path):
    """Recheck complete exported responses, including deliberate SQL failures."""
    if not expected_path.is_file() or not actual_path.is_file():
        return False
    with expected_path.open(encoding="utf-8") as expected, actual_path.open(encoding="utf-8") as actual:
        count = 0
        for want_line, got_line in itertools.zip_longest(expected, actual):
            if want_line is None or got_line is None:
                return False
            want, got = json.loads(want_line), json.loads(got_line)
            if want.get("expect_error", False):
                error = got.get("error", {})
                matched = (set(got) == {"id", "ok", "error"} and got["id"] == want["id"] and
                           got["ok"] is False and isinstance(error, dict) and isinstance(error.get("code"), str) and
                           bool(error["code"]) and isinstance(error.get("message"), str))
            else:
                matched = got == want
            if not matched:
                return False
            count += 1
    return count > 0


def training_proof(directory, spill, source_count):
    manifest = load(directory / "corpus/manifest.json", {})
    requests = manifest.get("phase_requests", [])
    records = lines(directory / "verification.jsonl")
    expected = {f"responses-{threads}-{phase}.jsonl": phase for threads in (1, 2) for phase in (0, 1, 2)}
    base_ok = (manifest.get("seed") == 2026111201 and len(requests) == 3 and len(records) == 6 and
               {record.get("actual_file") for record in records} == set(expected) and
               all(record.get("status") == "passed" and record.get("responses") == requests[expected[record["actual_file"]]] + 1
                   for record in records) and
               all(responses_match(directory / f"corpus/phase-{phase}.expected.jsonl", directory / name)
                   for name, phase in expected.items()))
    original = load(directory / "original-corpus-match.json", {})
    original_ok = original.get("status") == "passed" and original.get("files_compared") == 7
    profile_checks = [{**entry, "copied_sha256": digest(directory / "profile-data" / entry["path"].removeprefix("/pgo/"))}
                      for entry in checksums(directory / "profile-sha256.txt")]
    profiles_ok = len(profile_checks) == source_count > 0 and all(
        entry["sha256"] == entry["copied_sha256"] for entry in profile_checks)
    corpus_checks = [{**entry, "copied_sha256": digest(directory / entry["path"].removeprefix("/training/"))}
                     for entry in checksums(directory / "corpus-sha256.txt")]
    corpus_ok = len(corpus_checks) == 7 and all(entry["sha256"] == entry["copied_sha256"] for entry in corpus_checks)
    spill_ok, spill_detail = True, None
    if spill:
        sm = load(directory / "spill-corpus/manifest.json", {})
        sr = lines(directory / "spill-verification.jsonl")
        observations = load(directory / "spill-evidence/checkpoints.json", [])
        checkpoints = sm.get("checkpoints", [])
        spill_counts = sm.get("phase_requests", [])
        sc = checksums(directory / "spill-corpus-sha256.txt")
        spill_ok = (sm.get("seed") == 2026111301 and sm.get("rows") == 4096 and sm.get("dimensions") == 128 and
            sm.get("tag_bytes") == 8192 and len(spill_counts) == 3 and len(sr) == 3 and len(sc) == 7 and
            {entry.get("actual_file") for entry in sr} == {f"phase-{phase}.actual.jsonl" for phase in range(3)} and
            all(entry["sha256"] == digest(directory / entry["path"].removeprefix("/training/")) for entry in sc) and
            all(entry.get("status") == "passed" and entry.get("responses") == spill_counts[int(entry["actual_file"].split("-")[1].split(".")[0])] + 1 for entry in sr) and
            all(responses_match(directory / f"spill-corpus/phase-{phase}.expected.jsonl", directory / f"spill-evidence/phase-{phase}.actual.jsonl") for phase in range(3)) and
            isinstance(observations, list) and len(checkpoints) == len(observations) == 4 and
            all({key: observed.get(key) for key in ("phase", "request_id", "stage")} == checkpoint
                and {file.get("kind") for file in observed.get("files", [])} == {"payload", "flat"}
                and all(file.get("unlinked") is True and file.get("bytes", 0) >=
                    (32 * 1024 * 1024 + 1 if file["kind"] == "payload" else 4096 * (128 * 4 + 16))
                    for file in observed["files"])
                for observed, checkpoint in zip(observations, checkpoints)))
        spill_detail = {"manifest": sm, "verification": sr, "checkpoints": observations,
                        "phase_timings": load(directory / "spill-evidence/timings.json")}
    cost = load(directory / "training-cost.json", {})
    cost_ok = (cost.get("spill_enabled") == int(spill) and
               all(isinstance(cost.get(key), int) and cost[key] >= 0 for key in
                   ("base_nanoseconds", "spill_extension_nanoseconds", "total_nanoseconds")) and
               cost["total_nanoseconds"] == cost["base_nanoseconds"] + cost["spill_extension_nanoseconds"])
    return {"passed": base_ok and original_ok and profiles_ok and corpus_ok and spill_ok and cost_ok,
            "base_oracle": base_ok, "original_corpus_unchanged": original_ok,
            "copied_corpus_hashes_match": corpus_ok, "profile_hashes_and_translation_unit_count_match": profiles_ok,
            "profile_files": profile_checks, "spill_passed": spill_ok if spill else None, "spill": spill_detail,
            "training_cost_valid": cost_ok, "training_cost": cost}


def benchmark_proof(report, baseline_sha, candidate_sha, scenarios, pairs, queries):
    expected = {(scenario, seed, pair) for scenario in scenarios for seed in SEEDS for pair in range(pairs)}
    records = report.get("pairs", [])
    keys = {(entry.get("scenario"), entry.get("seed"), entry.get("pair_index")) for entry in records}
    binaries = report.get("binaries", {})
    passed = (report.get("status") == "passed" and len(records) == len(expected) and keys == expected and
              report.get("seeds") == list(SEEDS) and report.get("pairs_per_seed") == pairs and
              report.get("queries_per_phase") == queries and baseline_sha is not None and candidate_sha is not None and
              binaries.get("baseline", {}).get("sha256") == baseline_sha and
              binaries.get("candidate", {}).get("sha256") == candidate_sha and
              all(entry.get("all_responses_identical") is True and
                  entry.get("baseline", {}).get("response_sha256") is not None and
                  entry["baseline"]["response_sha256"] == entry.get("candidate", {}).get("response_sha256") for entry in records))
    return {"passed": passed, "status": report.get("status"), "pairs": len(records), "expected_pairs": len(expected),
            "summary": report.get("summary"), "error": report.get("error"), "binary_hashes": binaries}


def collect(out, revision, exit_code):
    out.mkdir(parents=True, exist_ok=True)
    stage = read(out / "stage.txt").strip() or "setup"
    source_count = sum(entry["path"].startswith("minivec/src/") and entry["path"].endswith(".cpp")
                       for entry in checksums(out / "source-sha256.txt"))
    binary_hashes = {variant: digest(out / variant / "minivec") for variant in VARIANTS}
    variants = {}
    for variant in VARIANTS:
        directory = out / variant
        image = load(directory / "image.json", [])
        names = [name.removeprefix("./").lstrip("/") for name in read(directory / "release-files.txt").splitlines()]
        packaged = "minivec" in names and not any(name.split("/")[0] in ("training", "pgo", "src") for name in names)
        training = training_proof(directory / "training", variant.endswith("_spill"), source_count) if variant.startswith("pgo") else None
        variants[variant] = {"binary_sha256": binary_hashes[variant],
            "image_id": image[0].get("Id") if isinstance(image, list) and image else None,
            "release_excludes_training_and_feedback": packaged, "training": training,
            "build_cost": load(directory / "build-cost.json")}
    checks = load(out / "correctness.json", {})
    entries = checks.get("checks", [])
    tests = {"json_protocol", "topk_serial", "topk_two_threads", "official_public"}
    checks_ok = (checks.get("status") == "passed" and len(entries) == 24 and
                 {(entry.get("variant"), entry.get("test")) for entry in entries} == set(itertools.product(VARIANTS, tests)) and
                 all(entry.get("returncode") == 0 for entry in entries))
    builds_ok = checks_ok and all(value["binary_sha256"] and value["image_id"] and
        value["release_excludes_training_and_feedback"] and (value["training"] is None or value["training"]["passed"])
        for value in variants.values())
    plan = load(out / "measurement-plan.json", {})
    measurement = {}
    for baseline, candidate in COMPARISONS:
        name = baseline + "-vs-" + candidate
        for suite, scenarios, pairs, queries in (
            ("standard", ["standard10k"], plan.get("standard_pairs_per_seed", 5), plan.get("standard_queries", 1000)),
            ("boundaries", BOUNDARIES, plan.get("boundary_pairs_per_seed", 1), plan.get("boundary_queries", 64))):
            key = name + "-" + suite
            measurement[key] = benchmark_proof(load(out / (key + ".json"), {}), binary_hashes[baseline], binary_hashes[candidate], scenarios, pairs, queries)
    complete = (stage == "complete" and all(item["passed"] for item in measurement.values()))
    passed = exit_code == 0 and builds_ok and (complete or stage == "complete-build-only")
    proof = {"status": "passed" if passed else "incomplete_or_failed", "source_commit": revision,
        "source_commit_record_matches": read(out / "source-commit.txt").strip() == revision,
        "exit_code": exit_code, "stage": stage, "source_translation_units": source_count,
        "all_six_builds_and_24_correctness_gates_passed": builds_ok, "variants": variants,
        "measurement_plan": plan, "measurements": measurement, "all_planned_pairs_completed": complete,
        "screen_only": plan.get("mode") == "--screen", "boundary_screen_only": plan.get("boundary_pairs_per_seed", 1) == 1,
        "official_score": None}
    if not proof["source_commit_record_matches"]:
        proof["status"] = "incomplete_or_failed"; passed = False
    save(out / "proof.json", proof)
    save(out / "run-status.json", {"exit_code": exit_code, "stage": stage, "proof_status": proof["status"]})
    artifacts = [{"path": path.relative_to(out).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
                 for path in sorted(out.rglob("*")) if path.is_file() and path.name != "hashes.json"]
    save(out / "hashes.json", {"source_inputs": checksums(out / "source-sha256.txt"),
        "build_inputs": checksums(out / "build-input-sha256.txt"), "harness_inputs": checksums(out / "harness-sha256.txt"), "artifacts": artifacts})
    return passed


if __name__ == "__main__":
    command = sys.argv[1]
    out = Path(sys.argv[2]).resolve()
    if command == "init" and len(sys.argv) == 4:
        init(out, sys.argv[3])
    elif command == "check-binaries" and len(sys.argv) == 3:
        check_binaries(out)
    elif command == "collect" and len(sys.argv) == 5:
        passed = collect(out, sys.argv[3], int(sys.argv[4]))
        if int(sys.argv[4]) == 0 and not passed:
            raise SystemExit("PGO matrix evidence is incomplete despite zero process exit code")
    else:
        raise SystemExit("usage: report.py init OUT COMMIT | check-binaries OUT | collect OUT COMMIT EXIT_CODE")
