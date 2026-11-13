# Standalone batch8 integer-distance experiment

Prepared in `work`, with zero production edits, DB runs, PGO builds or native
executions. This candidate is distinct from coordinate-block early exit:
eight consecutive rows share query sign extension, while computing the same
complete integer distances. Fixed batch size 8; no parameter search.

Reference headers are exact exported E020 source, checked against the
previous six-commit source audit by SHA256, length and Git blob hash. The
reference manifest identifies each byte sequence. Historical searches in
current source, formal-six source and prior experiment notes found no same
cross-row mechanism. This is a bounded code/history search, not a claim
about every unpublished branch or experiment.

On a Linux x86_64 host/container with g++ and Python 3:

```sh
python3 run_linux.py --output /out/results --pairs 4 --queries 64
```

The output directory must not already exist. The parent Docker wrapper is
responsible for its fixed GCC image, one CPU quota/affinity, 2 GiB memory,
zero swap and network disabled. The runner selects one already-allowed CPU,
records `/proc/self/status`, cgroup CPU/memory limits when available,
`lscpu`, compiler, commands, source/binary hashes, all stdout and failure
logs. Missing resource files are explicitly reported. Unsupported AVX2 or
failed gates never become a successful microbenchmark result.

Normal O3 and ASan/UBSan correctness checks must have identical positive
check counts. Only then does the runner build the O3 microbenchmark. No
`-march=native`, LTO or PGO is used. Four AB/BA-balanced pairs cover each of
n=128/512 and 10000/10003 rows, 64 distinct fixed seeded queries. The corpus
is generated before timing and warmed on both paths. Every timed path scans
the same immutable corpus and must return the same positive checksum.

Primary comparison: original AVX2 single-row function pointer against AVX2
batch-eight function pointer. If native dispatch selects AVX512, an extra,
separately labelled native-baseline/AVX2-batch control is retained. It must
not be merged into the same-ISA mechanism comparison. Both harness entry
points are opaque to GCC interprocedural specialization. Timer calls occur
once per complete scan, never inside a distance calculation. All raw pairs,
absolute wall/process CPU and negative ratios remain in `summary.json`.

These are integer-kernel scans, with no SQL, heap, threshold computation,
quantization, float distances, I/O or query worker activity. Synthetic
full-range codes are not the formal workload distribution. Speedup here is
only evidence to consider a carefully gated resident/unfiltered prototype,
not evidence of a higher formal score. A practical AVX512 control that loses
would be a blocker for unconditional AVX2 integration on those machines.

The proposed future path stages 32 bytes of uint32 results and consumes them
in original ordinal order, with each row's current unchanged cap and
fallback. All other query paths initially retain their old implementation.
This source package does not implement that production change.
