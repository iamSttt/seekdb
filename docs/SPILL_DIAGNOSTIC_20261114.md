# Spill cache isolated native validation

The isolated `9ed5baa1b1363ebd388850e74271b353954902da` plus spill-cache candidate passed [GitHub Actions run43542440254](https://github.com/iamSttt/seekdb/actions/runs/43542440254). The tested runtime commit is `2bbe6d00f2dc97182d3021f21449473581d623d2`; its actual root Docker ELF SHA256 is `aecd9ce4887b60e12ec747677f8f02bf608a8b7173583d2ad0386b23f70b2a28`.

Only `db.cpp`, `flat_index.cpp`, and `flat_index.h` change runtime behavior relative to9ed. The candidate admits at most two non-PK BIGINT scalar caches only when both payload and vector index are disk-backed, retains the original global index budget and2MiB cache bound, and retains EE's per-record index writer. No batch writes, PGO change, resident posting change, performance matrix, or formal submission occurred.

Verified results:

- Pinned root Docker build, actual image ready handshake, all20 public cases.
- Actual image interactive/JSON semantics and exact float32 TopK oracle, serial and explicit parallel.
- Cache compression widths/extremes, allocation/budget fallback, read/write faults, remaining predicates, mutations, rollback, slot reuse and restart under16MiB/8192B/1B index budgets.
- Shared index budgets under the same three settings.
- Resident payload with disk-backed index admits no cache; true payload spill admits at most the first two columns; rollback/reopen results remain equivalent.
- Spill-filter, cross-table spill, and staged-WAL recovery regressions under ASan+UBSan. Leak detection was disabled.

The old R batch-tail expectations were adapted to this actual EE per-record writer: first/middle/last add ENOSPC, EINTR/short-write completion, incomplete-build non-publication, and finish issuing no disk write. Other cache/filter/invalidation assertions were retained. There was no runtime fix after the first diagnostic run.

[The verification receipt](../validation/spill-diagnostic-43542440254-result.json) binds65 CI input hashes to the tested Git blobs and verifies the unchanged workflow blob. [The raw archive](../validation/spill-diagnostic-43542440254-raw.zip) retains the complete downloaded log, run identity, all43 extracted reports and their extraction manifest. Archive SHA256: `c3ea29d310285d1d48a7c426e09e5e37eef2f37522b2c00d7a23e1455dff7d5a`.

This is correctness and safety evidence for this exact runtime, not a performance result or release recommendation. It does not transfer automatically to later lazy/shared-ceiling combinations, and it does not prove hidden-case or formal-score outcomes. The diagnostic-only `ci_score.sh` on this branch should not replace the main branch's validation entry when importing the three-file feature patch.
