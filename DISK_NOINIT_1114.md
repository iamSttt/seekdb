# Disk scan buffer source factor — frozen before execution

This experiment changes only the two disk branches of `FlatIndex::scan` and
`FlatIndex::scan_filtered`: one value-initialized `vector<float>` per call becomes
one local throwing `unique_ptr<float[]>(new float[n])` allocation. The row count,
capacity, layout, block size, transfer calls, predicate/visitor order, and all
resident code are unchanged. An empty count does not allocate. No shared or
thread-local production buffer is introduced, and no build flags change.

`transfer` retries interrupted or short positive reads until every requested byte
is filled; EOF and other errors throw before the caller reaches any row in that
block. Only the initialized `count * stride` prefix is inspected, so the tail of
the reusable allocation is never read. The float array has object lifetime and
alignment before `pread`; the metadata words are accessed through `memcpy`.
For each call, the old zeroing covers at most approximately 1 MiB unless a single
record exceeds that size. It occurs once per scan, not once per block. Removing
it does not remove allocation, disk reads, distance computation, or full-row I/O.

The fixture runs on both frozen 760f and the candidate. It poisons allocations,
checks float bit patterns and metadata across empty, block-edge and tail counts,
tests 1/128/768 dimensions and a record larger than the block target, then injects
EINTR, short reads, EOF and EIO after a partial first or second block. The failed
block must produce no predicate or visitor callbacks; earlier complete blocks
may have done so. Allocation failure, user callback exceptions, early stop,
nested scans and overlapping concurrent scans also leave later calls correct.
ASan/UBSan are additional checks; they are not a general uninitialized-read oracle.

The unchanged root Docker/public/source and safety gates run first. The original
retained schedule then runs exactly twelve source-factor pairs against 760f:
four default10k128 pairs per CPU1/CPU2, two spill_high_q16 pairs on CPU1, and two
wide768 pairs on CPU2. Every original 31 protection metric and adverse sample is
retained. Additionally, **each** of the two spill pairs must have candidate/control
ratios at most 0.97 for initial-sixteen-query wall time and CPU, and at most 0.95
for first-query wall time. No threshold, workload, pair or order may be changed
after seeing results. These two spill pairs are a screening condition, not a
release confirmation or an official-score prediction.

Risks to measure include allocator/page-first-touch behavior, cache state and
code layout despite the untouched resident path. Removing an approximately 1 MiB
memset may yield little benefit while the full index still streams from disk.
Throwing allocation and automatic cleanup remain; no allocation-failure fallback
is added. No native result or speedup is claimed in this pre-execution document.
