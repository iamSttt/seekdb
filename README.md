# MiniVectorDB — 2026 competition round 2

This is a from-scratch single-process C++17 database candidate. It does not use
SQLite, RocksDB, Faiss, VSAG or HNSWlib to implement database/index operations.

## Build and run

```sh
docker build --platform linux/amd64 -t minivec .
docker run --rm -i -v minivec-data:/data minivec
```

Use the official protocol checks documented below. Latest JSONL validation is
recorded in [validation/jsonl-admission.md](validation/jsonl-admission.md).
The 2026-11-11 exact-query optimization and paired Docker measurements are in
[validation/score-20261111.md](validation/score-20261111.md).
The final runtime passed checks on two independent runners, with paired median
query speedups of 1.707x and 1.909x against the formal-admission baseline. These
are local synthetic results, not platform scores; one seed regressed against
the immediately preceding candidate.

The Dockerfile pins the GCC 13.3.0 build image by digest and produces a static
binary in a scratch runtime. There are no runtime network or package-manager
dependencies. The program never starts a network listener. All database, WAL,
rebuildable index and temporary sort files are under `/data`.

Local Linux build and tests:

```sh
cd minivec
make -j4
make -j4 test
MVDB_PROTOCOL=sql python3 tools/harness.py run tests/cases
./test_sort --stress-fanin
./test_query --stress-topk
./minivec --data /path/to/data --mode exact
```

## Official JSONL protocol

The default entrypoint implements the organizer's protocol published in commit
`f4d4e26653af823a7601668802f80e374ad6643c` on 2026-11-10. The verbatim protocol,
public fixtures and diagnostic driver are included in `STDIO_PROTOCOL.md`,
`mysqltest/`, and `protocol_check/` for reproducible local checks.

After opening and recovering `/data`, the program writes `{"type":"ready"}`.
Each physical input line is one JSON request with `id`, `op`, and, for execute,
`sql` and optional `search_mode`. Each response preserves `id`, includes boolean
`ok`, and contains typed `columns`/`rows`, integer `affected_rows`, or a structured
error. BIGINT values remain integers, strings remain strings, and vector cells
are arrays. Output is one compact JSON line, flushed per request; diagnostics
are sent only to stderr. A shutdown request rolls back pending changes, closes
storage, acknowledges success, then exits. Durable commits survive SIGKILL.

Run the official checks against the default submitted image:

```sh
python3 protocol_check/run.py --image minivec --handshake-only --output protocol-results/handshake
python3 protocol_check/run.py --image minivec --output protocol-results/public
```

Use a fresh output directory for each run. The organizer's driver applies
2 CPU, 2 GiB RAM, a read-only root filesystem, and writable `/data`. It verifies
startup, per-request responses, all public fixtures, normal restart and mode
switching. It does not contain the hidden formal cases or calculate a score.
The published error-code mapping is currently empty; failures return stable,
nonempty string codes based on the engine errors.

For source development on Linux:

```sh
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source
python3 minivec/tests/json_protocol.py --binary minivec/minivec
```

The earlier plain-SQL adapter remains available explicitly with `--protocol sql`
or `MVDB_PROTOCOL=sql` for engine regressions and older benchmarks. Its
`MVDB_OUT=tsv`, `tsv-status`, and `table` options do not select the formal protocol.
Historical TSV validation reports are not evidence of JSONL compatibility.
`exact` and `ann` requests both use the exact Flat index, which has recall 1.

Bounded ascending TopK uses an outward-rounded lower distance bound to reject
only candidates proven strictly worse than the heap's exact boundary. Ties and
uncertain candidates still use the exact float32 squared-distance accumulator;
descending and unbounded sorts retain the full exact calculation. The error
argument is documented beside the implementation and independently tested with
integer/Fraction reference distances.

Eligible resident indexes also retain bounded int8 codes at a power-of-two
scale. Their integer distance is only a rejection filter: an outward-rounded
triangle-inequality bound must prove a candidate strictly worse before skipping
its exact calculation. Out-of-range queries, unsuitable dimensions/scales,
unsupported floating environments and unavailable code storage use the original
exact path. This is not approximate nearest-neighbor ranking. SIMD dispatch and
bounded posting/norm-order caches change traversal cost, not result semantics.

## SQL and semantics

- CREATE TABLE with one inline BIGINT PRIMARY KEY, BIGINT, VARCHAR(n), VECTOR(n),
  and at most one VECTOR column.
- Multi-row INSERT, SELECT projections and `*`, UPDATE, DELETE, AND conditions,
  comparisons, one ORDER BY key/expression, LIMIT.
- `L2_DISTANCE(vector_column, '[...]')` in projections or ORDER BY; filtering is
  applied before TopK. Ties use ascending primary key, including DESC ordering.
- BIGINT ordering preserves all 64 bits; VARCHAR length/order use Unicode code
  points; finite float32 vectors are stored and double arithmetic computes L2.
- BEGIN/COMMIT/ROLLBACK, read-your-writes, statement-level rollback inside a
  transaction. Primary-key updates, NULL, AUTO_INCREMENT, JOIN, aggregation,
  subqueries and unsupported constructs return errors.
- CREATE VECTOR INDEX builds a real exact Flat index; small indexes use
  contiguous vectors and keys, large indexes use sequential disk records.
  Mutations/undo invalidate derived structures; queries rebuild against current
  visible data, and startup rebuilds persisted index definitions before readiness.
- SHOW TABLES and DROP TABLE are additional local conveniences.

## Storage, recovery and memory

The source of truth is a checksummed snapshot plus WAL. Successful commits sync
before returning. A failed statement undoes only its own changes and truncates
its own WAL suffix. Rolled-back DDL never becomes authoritative through a separate
catalog. Recovery replays only complete committed transactions. WAL recovery
streams transaction records instead of retaining an entire transaction body.
Checkpoints use a new file, sync, atomic rename and directory sync before WAL
truncation. Repeated interruption of recovery is covered by crash tests.

Row payloads spill from heap to rebuildable files at about 32 MiB per table,
with a 64 MiB global resident payload budget. Metadata such as PK/slot/undo entries
still scales with row/transaction count. Flat index resident payload has a shared
16 MiB budget. Snapshot parsing uses bounded streaming reads. Small TopK keeps
keys rather than whole rows; large ordered outputs use bounded runs and bounded
merge fan-in under the data directory. Output is streamed in chunks.

These are payload budgets, not a claim of a universal fixed process/cgroup memory
bound. A single enormous SQL statement or enormous field still needs memory.
The published Docker diagnostic driver enforces 2 CPUs and 2 GiB RAM; this is
the limit used by the current paired checks. Local RLIMIT_AS tests and process
RSS are not substitutes for cgroup memory.peak including page cache.

## Example

```sql
CREATE TABLE docs(id BIGINT PRIMARY KEY, category BIGINT, title VARCHAR(32), v VECTOR(2));
INSERT INTO docs VALUES (2,1,'second','[1,0]'),(1,1,'first','[0,1]');
SELECT id,title FROM docs ORDER BY id;
CREATE VECTOR INDEX docs_v ON docs(v);
SELECT id,L2_DISTANCE(v,'[0,0]') AS distance FROM docs WHERE category=1 ORDER BY distance LIMIT 10;
BEGIN;
UPDATE docs SET title='changed' WHERE id=1;
SELECT title FROM docs WHERE id=1;
ROLLBACK;
BEGIN;
DELETE FROM docs WHERE id=2;
COMMIT;
```

Restart with the same data directory: row 1 retains title `first`; row 2 is absent.

## Validation and scoring

`validation/report.json` records observed local results and limitations.
`tools/benchmark_local.py` runs 10k/128d/1000-query local A/B workloads with a
separate float64 oracle, filters, distance validation and restart validation.
Its write acknowledgement SELECT and RSS/IO proxies are explicitly not official
T/B/M. `tools/score_metrics.py` accepts complete official metrics and computes E;
it cannot derive a leaderboard rank or points from speedup alone.

The requested branch is `2026-competition-round2`. Formal submission `d10abbc`
failed the MySQL admission gate despite passing all 20 published v4 examples.
The result did not identify the failing SQL or provide an execution log.
The subsequent repairs and their regression evidence are recorded in
[admission fixes](validation/admission-fixes.md). Formal submission
`fa562be89069a84215a6196c9e24e4c6274f3862` passed the MySQL admission gate on
2026-11-10 at 21:33 (Asia/Shanghai); the platform displayed raw performance
E `2180.028190`. The newer runtime `3e23e7ce2cb932b514c622d20a25265c8fce9c69`
has passed the recorded local/public checks, but has no new formal score here.
The stated bounded E formula and that raw platform display use different
apparent scales; we do not convert local speedups into a projected platform E.
The user's 5000+ target remains unverified. Performance
and evidence each contribute up to 40 rank-based points. No local result proves
80 points or first place. The two public documents disagree on evidence dimension
count and the default final-version selection; explicitly select the final commit.

`agent/conversation.json` contains attributed, redacted actual interactions and
observed evidence. Agent reasoning, implementation and tests are not represented
as student-authored understanding. It is the default submission evidence and is
kept synchronized with the versioned material checked by CI. The formal baseline's
AgentPrompt result displayed `—`/invalid and did not contribute evidence points.
The platform's generic message lists several possible causes (invalid or pending
review result, outside 0–100, or missing summary); it does not establish one cause.
The inherited vector.cpp IVF implementation is
experimental, covered separately by unit tests, and not used by the SQL executor.

The follow-up [interactive checks](validation/interactive-gate.md) cover malformed
vector framing, failed row-cache/WAL writes, and a 50,000-row disk-backed index.
Run the latter on Linux with
`python3 minivec/tests/spilled_index.py --binary /path/to/minivec`.

## Integration with intermediate remote work

Remote commit `6e948c3` remains a parent of this submission. Its table-level
primary key, bare SELECT alias and moving input cursor ideas are retained,
and primary-key UPDATE/DELETE use direct lookup. Its standalone experiments,
evidence tooling and documentation are preserved for reference. The current
transaction-safe core supersedes that intermediate implementation: use a fresh
/data directory, because its MVS2 snapshots are not compatible with this MVS1
engine. The inherited `agent/raw/00-scaffold-seed.md` is a historical paraphrase,
not verified student testimony; it is excluded from the current evidence JSON.
The inherited conversation generator/CI uses an older schema and must not
replace the current attributed evidence. Root Dockerfile is the submission build;
`minivec/Dockerfile` supplies the same build for a minivec-only context.
