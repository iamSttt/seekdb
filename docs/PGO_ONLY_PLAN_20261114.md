# PGO-only candidate: prepared, not measured

This isolated tree starts from `760f66220f2c7186b4ba295f9e3065650e549f9e`.
All 16 runtime source files and both third-party files remain byte-for-byte
equal to that commit. The only runtime build changes are the root Dockerfile,
the two original R33 training inputs, and the Docker context allowlist needed
to copy those inputs. No R feature macros, runtime implementation, parser,
resident filter cache, or output writer have been imported.

The compiler digest and all non-PGO program flags remain unchanged. The
Dockerfile is R33's Dockerfile with only its `FEATURE_FLAGS` argument and
assignment removed. `ENABLE_PGO=0` remains a diagnostic control; the proposed
default is 1. The release stage still contains only `/minivec` and empty `/data`.

## Why this is an experiment

Run `43548698762` established that 760f passes the root and specialized
correctness checks and all initial EE primary thresholds, but fails 11
retained-R protections. Its wide768 query-wall ratio is 1.540233 and its spill
first-lifecycle ratio is 1.197015 (candidate/R; lower is faster). R uses PGO and
other runtime changes, so this comparison cannot attribute the gap to PGO.
This candidate isolates the build factor before considering runtime imports.

Earlier same-source PGO measurements were not uniformly positive. The R33
archive contains [the original report](https://github.com/iamSttt/seekdb/blob/33a52a7dd4426af059ec2334b7c93a07c38c93c9/docs/ITERATION2_20261112.md)
and [the final 105-pair summary](https://github.com/iamSttt/seekdb/blob/33a52a7dd4426af059ec2334b7c93a07c38c93c9/validation/iteration2-final-performance.json):

| Earlier same-source PGO case | Baseline / PGO ratio | Scope |
| --- | ---: | --- |
| Default 10k query | 1.014600 | Final 58354ff versus 5a67f16, 15 pairs |
| Spill query | 0.995485 | Same final experiment |
| Spill restart query | 0.966053 | Includes restart/rebuild |
| Spill index build | 0.963603 | Same final experiment |
| Spill lifecycle | 0.999375 | Same final experiment |
| Spill query in earlier dot-source isolation | 0.9703 | Three seeds all regressed; different runtime |

These are native process measurements, not official E, and do not establish
the current candidate's performance. They justify retaining cold, build,
restart, writes, and complete lifecycle protections even when steady-state
default queries improve.

## Frozen training and build identity

The original R33 training generator and shell script are copied without edits.
Seed 2026111201 remains distinct from evaluation seeds. The generator uses
16/128/512 dimensions, independent exact dyadic distance and PK ordering,
filters, updates, rollback, and restart. The original script runs three phases
at one and two query/parse threads, checks every response, checks profiling
errors and feedback inventory, and refuses missing/mismatched feedback.
Ordinary expected responses are compared completely; intentional SQL-error
fixtures use the original structured error predicate, not exact message text.

The dyadic distance oracle is independent of database distance code. Each
coordinate is an integer in [-24570,24570] divided by 4096; the maximum squared
distance numerator over 512 coordinates is below 2^41 over denominator 2^24,
so the generator's binary64 arithmetic is exact for this corpus. This proof
does not cover arbitrary hidden-test vectors; existing full-range oracles
remain mandatory.

Prepare two fresh `--no-cache` PGO builds, A then B, with the same context,
compiler, flags, and unchanged training. Retain both image IDs, final ELF
hashes, profile hashes, corpus hashes, six training verification records,
training stderr, and full build logs. The A label is fixed before measuring;
neither build may be selected after seeing speed. Profile scheduling and
startup calibration can change feedback despite deterministic inputs.

If A and B have different ELF hashes, record that fact and block the existing
same-ELF independent confirmation. Do not weaken that confirmation rule or
let one build represent later official rebuilds. A deterministic-build change
or rejection of PGO must be considered separately. No training behavior is
changed in this candidate to preempt that issue.

## Measurements and protections

The machine-readable frozen plan is
`validation/pgo-only-plan-20261114.json`. For each build, first run actual-image
handshake, all 20 public cases, live JSONL, exact TopK, differential numeric
responses, NUL atomicity, restart, and the ordinary root/special guards. No
timing from a failed correctness build is eligible for acceptance.

Compare each PGO build to a freshly rebuilt exact 760f non-PGO control to
isolate the factor. Use the original retained workload definitions and seeds:
four default10k pairs at each CPU count, two spill_high_q16 pairs at one CPU,
and two wide768 pairs at two CPUs, alternating AB/BA. This is 12 pairs per
build. Preserve every raw metric, both order medians, and slower samples.
Apply the original stable-regression rule: timing threshold 1.02, memory
threshold 1.05, with its original pair/order requirements. Do not drop default
ingest, first-query, spill lifecycle, or wide output/memory metrics.

Keep the original EE primary12 and retained-R12 gates unchanged for the later
final combination. This PGO-only experiment runs neither whole-program gate:
the 760f candidate still lacks other measured R mechanisms, and the immediate
question is this build factor's correctness, identity and same-source effect.
An isolated factor gain cannot replace those final gates. Both fresh build
products must clear the applicable correctness, identity and same-source
protections; different ELF products cannot enter the existing same-ELF
independent confirmation. The previously frozen new-run/new-boot, seed19087
independent confirmation remains required before recommending a final candidate.
No ratios are multiplied and no official score is predicted.

## Static checks and integration boundary

Run `python3 -B minivec/tools/pgo_only1114/verify_source.py` to verify source,
training and build hashes without building or starting a database. The new
worktree's automatic CRLF conversion was undone using the exact 760f Git
blobs; this is why the manifest binds bytes, not only a normalized Git diff.

The inherited `ci_from_best_1114.sh` rejects Docker/build-input differences
against EE and is bypassed only by this independent branch's explicit
`pgo-only-plan` route. `ci_pgo_only_1114.sh` now binds the allowed changes and
uses the original retained workload/scheduler/counters/oracles/guard rules,
with identity-only hooks selecting the fresh exact760f control. The adapter
does not modify the original measurement modules. Its manifest freezes the
scripts and policies before any native execution. Synthetic static tests check
equal-build acceptance, unequal-build blocking, missing feedback rejection,
and the exact twelve-pair source-factor plan, without running a database.

Execution order is two no-cache builder stages, release extraction and exact
builder/release equality, A/B ELF comparison, fresh exact760f baseline, both
actual-image correctness gates, one source safety regression, then twelve
source-factor pairs for each build. A numerical failure of A does not discard
B; both complete raw result sets remain available. Each release stage reuses
its just-built stage; it never knowingly retrains or selects another product.
The independent branch may now be committed and dispatched after static review.
This document records the pre-run plan, not a successful native build,
correctness/performance result, or submission recommendation.
