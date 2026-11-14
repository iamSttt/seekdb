# Retained-R CI integration

Invoke after the ordinary root image build and correctness gates, using that image's actual extracted ELF. For the current `ci_from_best_1114.sh` variable layout, the call is:

```bash
bash "$repo/minivec/tools/retained_guards1114/ci_retained_guards.sh" \
  "$repo" "$scratch/candidate-binary" "$RUNTIME_COMMIT" \
  "$out/screen.json.d/candidate-file-identity.json" "$out/retained-r"
```

The identity JSON must bind `source_commit` (or `runtime_commit`) and `binary_sha256` (or `image_binary_sha256`) to this exact ELF. The proof is an identity binding, not a substitute for the earlier correctness results. The output directory must not already exist. Linux, Docker, cgroup v2, a C compiler and passwordless `sudo -n` are required. Call with `bash`; no executable file-mode change is necessary.

The adapter first verifies all 54 copied bundle files, the unchanged frozen plan and candidate identity. It then delegates to the original `run_retained_guards.sh`: fresh `git archive` of R commit `33a52a7dd4426af059ec2334b7c93a07c38c93c9`, original root Docker `--no-cache` build, all 28 original inputs, six successful current PGO training-oracle records and extracted ELF SHA `acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54` are mandatory. A nonmatching rebuild blocks; no imported/local ELF substitution.

The original 12-pair R protection schedule and thresholds remain unchanged: four default pairs on each CPU count, two spill-high-q16 pairs on CPU1 and two wide768 pairs on CPU2. `plan-only.json`, vendor10, historical policies, both R proofs and the original reference context are copied byte-for-byte. `copied-sources.json` records source and destination hashes. The sole copied-code adaptation normalizes historical Windows manifest path separators on Linux. Scoped `.gitattributes` preserves all copied bytes in Git checkout.

Keep the complete output tree as a CI artifact, including build log, image inspection, extracted R ELF, current build proof, `guards.json`, its `.d` raw measurements and logs, `execution.json` and `ci-gate.json`. Successful process completion alone does not pass numerical guards: this CI adapter exits **20** when the original report completes but a regression guard triggers or evidence is incomplete. Other build/measurement failures also remain nonzero. No missing guard, sample deletion or replacement run can become a pass.

`ci-gate.json` only certifies this retained-guard stage. It always leaves release, automatic-adoption and full-24-pair flags false. This later stage does not retroactively reproduce the original interleaved EE/R experiment. No Docker/PGO build or new database pair was run while preparing this integration; the copied `reader-checks.json` is historical reader validation only.

Preparation-only check (no Docker or database execution):

```bash
python3 -B minivec/tools/retained_guards1114/verify_bundle.py
```
