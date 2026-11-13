# Quarter-plus-margin standalone micro draft — NOT EXECUTED

This is a separately identified alternative to the historical half-error candidate. Its only executable algorithm change versus that header is `error*0.5L` to `error*(0.25L+0x1p-30L)` after the unchanged old-success branch and existing strict FP guard. Header comments describe the new bound. The current production half-bound code and its active CI remain unchanged.

`PROOF.md` includes both worst downward roundings: A=RN(SK), then RN(old_error*c), with old_error=A*4u and exact c=(1+2^-28)/4. At n<=2^32 and PC64/RN, E/B is at least approximately 1.000000003608875. Unlike power-of-two halving, the final multiply can set FE_INEXACT. Numeric squared-distance equality and unchanged FP controls are required; complete sticky-status equality is not claimed. Unsupported environments keep the existing old behavior. No new FMA/reduction/parameter search is introduced.

## Fixed future invocation

`python3 run_linux.py --output /out/results` inside the caller-provided fixed `gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818` environment. No new Docker/workflow is supplied here. The existing runner captures affinity/resource files and compiler; the caller remains responsible for enforcing 1 CPU, 2 GiB, swap 0, network none, read-only source and writable output. It must not claim caps merely from intended settings.

Reference remains the canonical original Git header SHA7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9. The same 6748 independent integer/Fraction fixtures are copied byte-for-byte from the historical standalone raw artifact; binary SHA74e1b99088943e073b5f4821844aabc7001a500ae2af9ef0c7dc309d8ed6262e. `make_fixtures.py` is unchanged provenance, but the runner now copies the pinned oracle instead of regenerating it.

Normal O3 and ASan/UBSan O1 correctness tests are byte-identical to the parent, including exact canonical squared output bits, midpoint cases, extreme float bits, all tails, unsupported controls, masked-exception guard checks and nonfinite legacy compatibility. Their old `tighter_bound` marker names are intentionally unchanged; the summary's experiment_id and source hashes distinguish this candidate. No old PASS record is imported.

Uninstrumented whole-call micro source, wrappers and corpus are unchanged: fullmantissa128/fullmantissa512/grid128, 512 rows, 64 repetitions per arm, AB/BA/AB/BA, exactly 3x4 pairs. The old comparator is the original canonical distance, NOT the half-bound version. Comparisons with earlier micro timings across runs are not a paired quarter-versus-half result. All guard/multiply/comparison/fallback costs stay inside each timed call. Counter shadow is a separate binary and remains untimed; its `tight_rescue` means rescue by this quarter-plus-margin candidate. No additional scenario, threshold tuning or favorable rerun policy is added.

Failures/unsupported exits retain logs and are not PASS; build and native attempt counts are distinct from completed counts. The inherited root oracle failure is preserved in `inherited-tighter-only-attempt1-audit.json` and `history.json`: one root PGO and preceding DB gates occurred, paired performance0, projected sqrt oracle wrong. It is not a failure or success of this new standalone candidate. Earlier half-bound micro results are not new results here.

Preparation consists only of identity checks, exact byte copying and necessary Python AST/source checks. Native builds0, native runs0, DB0, PGO0, CI0. Manifest lists the unchanged parent files and changed/new members. This draft is waiting for the coordinator's decision; no execution or production adoption is authorized by its existence.
