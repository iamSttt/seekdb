# One independent seed19087 confirmation

This is prepared tooling, not a benchmark result. Do not change the original `research1114` tools or combine seed19087 observations with seed7319. Run this stage once for the frozen candidate after the initial EE screen and retained-R guards have passed. A failure remains a failure; there is no retry loop, sample exclusion or automatic adoption.

On the **initial combined-candidate CI runner**, save the actual runner instance:

```bash
python3 -B "$repo/minivec/tools/confirmation1114/runner_identity.py" capture "$out/runner.json"
```

Keep that file with the full initial `screen.json` and its raw measurements. The confirmation requires a different `GITHUB_RUN_ID` and kernel `boot_id`, with confirmation attempt 1. Missing earlier runner evidence blocks the new-runner claim; it must not be reconstructed after the earlier runner is gone. Different boot identities verify separate boots, not independence of the underlying cloud hardware.

On a **new Linux CI runner/run**, rebuild/extract the unchanged candidate and EE root images, retain their ordinary correctness/build proof files, and require the two actual ELF hashes to equal their respective initial-screen hashes. No new runtime change may be slipped into confirmation. Each proof binds `source_commit`/`runtime_commit` and `binary_sha256`/`image_binary_sha256`.

Compile the unchanged launcher and make one call:

```bash
cc -O2 -Wall -Wextra -static "$repo/minivec/tools/confirmation1114/vendor/cgroup_exec.c" -o "$scratch/cgroup-exec"
bash "$repo/minivec/tools/confirmation1114/ci_confirmation.sh" \
  "$scratch/baseline-binary" "$scratch/candidate-binary" "$RUNTIME_COMMIT" \
  "$out/baseline-file-identity.json" "$out/candidate-file-identity.json" \
  "$initial_artifacts/screen.json" "$initial_artifacts/runner.json" \
  "$scratch/cgroup-exec" "$out/confirmation19087"
```

The output directory must be new. Preserve the entire tree: `confirmation.json`, `runner.json`, `bound-plan.json`, `screen.json`, `screen.json.d` and `screen.log`. Before executing a pair, `bound-plan.json` records the frozen schedule, both actual runtime source commits and ELF hashes, proof hashes and initial-screen hash. Runtime identity comes from the checked initial artifacts and root proof, never the driver checkout's Git HEAD. Thus the driver may live at a later tooling commit while `git archive` of the original combined runtime builds the same binary. A failed/incomplete or numerically negative confirmation exits nonzero while preserving its raw evidence. The initial R protection result is a separate prerequisite checked by the caller; this EE confirmation cannot replace it or claim a full historical 24-pair reproduction. All release/automatic-adoption flags stay false.

`plan.json` fixes six standard10k pairs per CPU count (12 total), seed19087, exact mode, 2 GiB memory, swap0 and the same one/two-CPU quotas and AB/BA order. The original `best_screen_1114.py::summarize` is copied verbatim: per CPU, lifecycle wall and CPU paired medians must be at most 0.95, query wall and CPU medians at most 0.90, each metric must improve in at least five of six pairs, and both AB and BA medians must be strictly below 1. The original metric extraction, phase boundaries, correctness oracle, all samples and restart checks remain.

The only changed measurement helper is the existing prepared `W/confirmation19087-draft/minivec/tools/small_compute.py` (SHA256 `6b7121ec7e6ca96c6c122d7d8be3d63c9cf222a620a7a1f6ceea279121a2c8fb`): an explicit highest-primary confirmation flag propagates seed19087 to the plan, generator and workload identity. Ordinary mode remains seed7319. Nine other vendored files are byte-identical to the frozen originals; the original helper, normalized human-readable patch, original manifest and statistics source are retained in `evidence/`. Source hashes and frozen plan are independently checked before and after execution.

Preparation-only check, with no database or Docker run:

```bash
python3 -B minivec/tools/confirmation1114/verify_sources.py
```
