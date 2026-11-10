# Official JSONL verification and exact TopK optimization

Verified on 2026-11-10 using organizer protocol/package commit
`f4d4e26653af823a7601668802f80e374ad6643c`. The protocol, public fixtures and
Python diagnostic driver are included unchanged in this repository.

## Failure mechanism addressed

The old default process read raw SQL and wrote TSV/text status. It did not send
the required JSON ready object, consume execute envelopes, echo request IDs,
or acknowledge shutdown. This is incompatible with the newly published actual
judge interface, independently of SQL correctness. Historical raw-SQL tests did
not validate that interface. The platform's earlier generic failure message did
not expose a failing SQL statement, so no hidden failure trace is claimed.

The default image now speaks JSONL. Query cells are serialized from engine
types, preserving BIGINT integers, strings and vector arrays. SQL failures
return one structured error and leave the process available for subsequent
requests. Normal shutdown rolls back pending transactions before acknowledging
and exiting. Diagnoses with a truncated UTF-8 suffix cannot crash error output.
The vendored general JSON parser is nlohmann/json v3.11.3, with its MIT license.
Storage, SQL execution, transactions and vector search remain our own code.

## Verification

Runtime files were tested at commit
`a665c323c34ea4b4e593eab62588a58312a2ed6e` in
[GitHub Actions run 42824224037](https://github.com/iamSttt/seekdb/actions/runs/42824224037).
This used a real Docker Engine on Ubuntu 24.04, building the submitted root
Dockerfile from source, rather than launching an exported root filesystem.

All of these passed:

- C++ engine and recovery regressions, 13 Python regression groups and nine
  legacy SQL scenarios, with explicit legacy protocol selection.
- Official source handshake/shutdown and all 20 public cases (95 SQL statements).
- Independent JSONL tests: spontaneous ready, ID types and large integers,
  typed cells, malformed requests, statement atomicity, transaction continuation,
  normal shutdown/restart, and acknowledged commits followed by SIGKILL.
- Official image handshake and all 20 public cases with the driver defaults:
  2 CPU, 2 GiB memory, 128 PID, network disabled, read-only root and writable
  `/data`. Startup/request/case timeouts remained 120/60/600 seconds.
- 50,000 random neighboring-double comparisons, 7,200 distance cases and
  63 midpoint cases, including conservative threshold checks.
- Independent integer/Fraction TopK oracle over 258 128-dimensional vectors:
  16 queries covering equal/near distances, late smaller primary keys, filters,
  ascending/descending ordering, mutations and rollback.

The tested candidate image ID was
`sha256:0ef5a2615e17af643aba3b5517426cb1f208feb0fe00665b4f183156a93d93ac`.
The final documentation/evidence update does not modify the tested Dockerfile,
build context rules, runtime sources or vendored parser.

## Exact TopK optimization and measurement

Once an ascending TopK heap is full, an inexpensive double calculation supplies
an outward-rounded lower bound. A candidate is discarded only when that bound
is strictly greater than the heap's exact worst distance. Equal or uncertain
distances still use the existing exact float32 accumulator and primary-key tie
break. Descending and unbounded queries retain their existing path. The bound
includes subtraction, multiplication, summation and final-double rounding;
the proof and conservative fallback conditions are in `distance.h`.

Two images were built on the same runner: the already validated JSONL baseline
`1419648efd4bfb3abc5753403343790d79ef2b76` and the candidate above. Each seed used
10,000 rows, 128 float32 dimensions, K=10 and 1,000 queries per exact/ann mode,
half with ordinary-column equality filters. Both images used the same official
Docker resource limits. Run order was reversed for the middle seed. Every run
checked exact/ann agreement, an independent full-scan oracle on ten queries,
primary-key ties and all 10,000 records after restart.

| Seed | Baseline exact queries | Candidate exact queries | Speedup |
| --- | ---: | ---: | ---: |
| 38021 | 1.549341 s | 0.455825 s | 3.399x |
| 7319 | 1.556824 s | 0.451866 s | 3.445x |
| 19417 | 1.537943 s | 0.451894 s | 3.403x |

Median paired speedup: **3.403x**. This is a synthetic local measurement, not
an official score or a prediction of the leaderboard. Both modes remain exact.

## Evidence and limits

[Final raw CI log](raw/jsonl-topk-ci.log) ·
[Paired summary](raw/jsonl-topk-paired.json) ·
[First official JSONL verification](raw/jsonl-ci-first.log) ·
[Neighbor optimization verification](raw/jsonl-ci-final.log)

The verification workflow remains on the separate validation branch
`codex/round2-jsonl-20261110`; a copy is stored at
`validation/ci/minivec-protocol.yml`. The submission branch does not need a
GitHub Actions workflow to build or run the database.

The published driver does not contain all 29 formal cases or the official
performance/Agent evaluator. Its error-code mapping is currently empty.
No formal submission was made during this work, and no formal pass or score is
claimed. `conversation.json` preserves actual student messages separately from
agent summaries and measured tool evidence; it does not invent student reasoning.
