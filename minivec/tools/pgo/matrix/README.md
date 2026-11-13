# I15: isolated compiler and spill-training matrix

This directory is an experiment. The root `Dockerfile`, the production
`tools/pgo/train.cpp`, and its training script remain unchanged. No measured
benefit or official score is claimed by adding these files.

Run from a clean committed Linux checkout with Docker and Python 3:

```sh
# Earliest gate: all builds, training evidence and correctness, no timings.
bash minivec/tools/ci_pgo_matrix.sh --build-only
# Default: 15 paired measurements per standard comparison; boundary screening.
bash minivec/tools/ci_pgo_matrix.sh
# Faster preliminary timing screen; cannot establish a performance conclusion.
bash minivec/tools/ci_pgo_matrix.sh --screen
```

Each invocation requires a new `pgo-matrix-results` directory. Preserve previous
results under distinct names before another run. CI must set a 60-minute job
limit and upload `pgo-matrix-results/**` with `if: always()`. The script stops
after 55 minutes to reserve collection time; timeout means incomplete evidence.
Failed or incomplete stages retain logs, raw partial reports and checksums.

All engine sources, harnesses and training helpers come from the same archived
commit. The six images are O3, O3+LTO, O3+PGO, O3+PGO with spill training,
O3+PGO+LTO and O3+PGO+LTO with spill training. Compiler GCC 13.3.0 is digest
pinned. LTO uses `-flto=2`; PGO generation and consumption share `/src`,
`src/*.cpp`, `/minivec` and the same LTO flag. Atomic counters, one nonempty
profile per translation unit, exported profile hash checks, and
`-Werror=missing-profile -Werror=coverage-mismatch` are mandatory. There is no
profile correction or fast-math. A compilation failure is never a timing sample.

The original generator is compiled from this commit's production helper.
Seven original corpus/oracle files must compare byte-for-byte with the extended
helper. Its six original training runs are preserved. The extension adds exactly
one single-thread, three-restart lifecycle with independent seed `2026111301`,
4,096 rows, dimension 128 and 8,192-byte strings. Strings force more than 32 MiB
of payload; they are not query predicates. Before accepting training, the helper
observes the live child process's unlinked `.payload-*` and `.flat-*` descriptors
at four checkpoints, checks their actual sizes, and independently verifies all
responses. Filtering, K=1/10/100, rollback, committed scalar/vector changes,
expected errors and shutdown rollback are covered. The oracle uses bounded
dyadic coordinates whose squared-distance sum is exactly representable in
double. Spill input, expected output, actual output and live-file evidence are
exported. Separate training cost includes generation, oracle and file checks;
build cost includes compilation and training. Wide-string parsing may skew a
profile, which is why original/extended training are compared separately.

All six final release binaries must pass JSON protocol, exact top-K with one
and two query threads, and the official public protocol checks. Final images
exclude training material and profiles. Engine/source, harness, binary, image,
training corpus and profile hashes are recorded. The collector rechecks exported
responses, exact pair identities and binary hashes instead of accepting counts
or a successful exit code alone.

Five paired comparisons isolate O3→LTO, O3→PGO, O3→PGO+LTO, original→spill PGO,
and original→spill PGO+LTO. Default standard10k uses seeds 7319/8911/13337,
five alternating pairs per seed and 1,000 queries per phase. Boundary scenarios
resident30k, low16, filter60k, update10 and spill initially use one pair per seed,
64 queries per phase. Boundary outcomes are screening only; set
`PGO_MATRIX_BOUNDARY_PAIRS=5` to confirm them if the job budget permits, or use
the exported binaries with the archived harness in a separate run. Keep both
positive and negative raw results. All runs use the same two-CPU affinity and
shared cache; process RSS and IO are proxies, not official E/M/B or points.

Local tooling verification (no compiler needed):

```sh
python3 minivec/tools/pgo/matrix/test_report.py
```

These synthetic evidence-gate tests do not test or benchmark the database.
The POSIX training helper and compiler matrix require Linux CI before adoption.
