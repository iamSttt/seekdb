# Prepared retained-R guard stage

This standalone bundle does not modify the active repository or historical measurement tools. It has not built Docker/PGO or run a database. `plan-only.json` is preparation, not a passing guard report.

Run on the Linux cgroup-v2 runner only after the candidate's root image and correctness gates are available:

```sh
bash /path/retained-guards-1114/run_retained_guards.sh \
  /path/current-repo /path/actual-candidate-ELF FULL_CANDIDATE_COMMIT \
  /path/candidate-root-build-or-gates-proof.json /path/new-retained-guard-output
```

The candidate proof must bind `source_commit` or `runtime_commit` and `binary_sha256` or `image_binary_sha256` to the actual candidate. The wrapper records this binding; it does not independently infer that an unspecified full correctness gate inventory passed. Keep the existing root gate evidence with the artifacts.

The shell obtains the exact commit `33a52a7dd4426af059ec2334b7c93a07c38c93c9` with `git archive`, including Dockerfile, `.dockerignore`, all `minivec/src`, third-party files, and both PGO training files. It builds the unchanged original root Dockerfile with `--no-cache`, checks all 28 input hashes, extracts `/minivec`, requires six successful training-oracle records, and requires ELF SHA256 `acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54`. A mismatched build is blocked; the known local/imported ELF is not substituted. There are two retained historical R2 proofs confirming that canonical source and ELF pairing; their unchanged JSON and complete source hashes are retained here. Earlier e020 archive provenance is not used as a claim that its source is byte-identical to canonical33.

Then the unchanged vendored `small_compute.py` runs `--highest-primary-reference retained`: CPU1 has four `standard10k` pairs and two `spill_high_q16` pairs; CPU2 has four `standard10k` and two `wide768` pairs. Each group alternates AB/BA, seed7319, exact mode, 2 GiB memory, swap0, one/two CPU quota and matching affinity. All 12 pairs, all response/oracle/restart checks, raw phase counters, timings, memory, I/O and negative samples remain. The vendor manifest is checked before and after execution. No environment variable beginning `MVDB_` is inherited by the driver.

Rules are copied from the original preregistration and evaluator, including the explicitly documented spill field-name erratum:

- Default, separately per CPU: a metric blocks only when its overall, AB-only and BA-only paired-ratio medians are all strictly above 1.02 for time/CPU, or 1.05 for memory.
- Spill/wide protection: the same measured metric must exceed its threshold in both individual AB/BA pairs to block.
- Spill's query guard is the **initial sixteen** `query_before_mutations` requests and their matching CPU interval; later cycles remain reported and count in full lifecycle metrics, but are not silently substituted into that initial query guard.
- Wide's seven query phases are summed within each run before forming the ratio; no phase-ratio averaging. There is no wide RSS, pure spill-ingest CPU, or first-request CPU guard because those were not measured.
- Missing metrics, missing groups/pairs, zero denominators, nonfinite values, failed identity, mode, oracle, resource or timeout checks never pass. No sample deletion, replacement runs, host selection or automatic adoption.

This is only the original **R-protection 12-pair portion** of the historical 24-pair screen. Running it later than EE is a separately scheduled stage and cannot be described as reproducing the original interleaved four-block 24-pair experiment. It cannot change earlier EE results, satisfy EE primary criteria, establish independent confirmation, or authorize a release or formal submission. `release_gate_passed` and `full_24_pair_gate_passed` remain false even when every retained guard is complete with no regression trigger. A completed process exit0 can still report `retained_regression_block`; inspect `numerical_retained_guards_passed` rather than treating command completion as numerical acceptance.

Prepared evidence: `reference-inputs.json` (28 hashes), `prepared.json`, two historical R2 proofs, unchanged rule/erratum files, and byte-identical vendor10. `check_reader.py` reads prior raw measurements only and compares all guard metric summaries with the original independent reader, then verifies missing/zero-denominator cases fail. It does not produce new performance evidence.
