# MiniVectorDB — 2026 competition round 2

This is a from-scratch single-process C++17 database candidate. It does not use
SQLite, RocksDB, Faiss, VSAG or HNSWlib to implement database/index operations.

## Build and run

```sh
docker build --platform linux/amd64 -t minivec .
docker run --rm -i -v minivec-data:/data minivec
```

Run the live stdin and restart smoke test on Linux with a disposable volume:

```sh
volume=$(docker volume create)
python3 minivec/tests/container_smoke.py -- \
  docker run --rm -i --network none -v "$volume:/data" minivec
docker volume rm "$volume"
```

See [container validation](validation/container-build.md) for the verified
build, runtime checks and official protocol availability.

The Dockerfile pins the GCC 13.3.0 build image by digest and produces a static
binary in a scratch runtime. There are no runtime network or package-manager
dependencies. The program never starts a network listener. All database, WAL,
rebuildable index and temporary sort files are under `/data`.

Local Linux build and tests:

```sh
cd minivec
make -j4
make -j4 test
python3 tools/harness.py run tests/cases
./test_sort --stress-fanin
./test_query --stress-topk
./minivec --data /path/to/data --mode exact
```

## Protocol status — must be checked against the official driver

The published brief delegates statement framing, response framing, error codes,
readiness, exit and exact/ann selection to a separate frozen package. The
[official public SQL cases](https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest)
now define sample input and result semantics. The official README specifies
that the complete driver and process framing are outside that release.

Run the public fixtures against the CLI after downloading that `mysqltest`
directory and building the executable:

```sh
python3 minivec/tools/run_public_cases.py --binary minivec/minivec --cases /path/to/mysqltest
```

The runner validates affected rows using diagnostic table output and checks
queries in both table and TSV modes. See the [public case results](validation/public-basic-v1.md).

Current adapter: semicolon-terminated SQL on stdin, tab-separated result headers
and rows on stdout, no output for successful mutations, `ERROR <code> (...)` for
errors; readiness and diagnostics on stderr. EOF closes normally and rolls back
an unfinished transaction. SIGTERM/SIGINT also close normally. Newlines within a
statement do not execute incomplete prefixes. Results are flushed after each
statement. The current TSV adapter does not define escaping for embedded tabs or
newlines; it must be adapted to the actual official framing when supplied.

`--mode exact` is the default. `--mode ann` is accepted and currently uses the
same exact search, returning recall 1 instead of an approximation. This CLI is a
local adapter choice, not a claim about the unpublished official mode syntax.
`MVDB_OUT=table` enables diagnostic human-readable tables (not the test protocol).

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
16 MiB budget. Snapshot parsing releases consumed mapping pages. Small TopK keeps
keys rather than whole rows; large ordered outputs use bounded runs and bounded
merge fan-in under the data directory. Output is streamed in chunks.

These are payload budgets, not a claim of a universal fixed process/cgroup memory
bound. A single enormous SQL statement or enormous field still needs memory;
formal resource limits have not been supplied. Local RLIMIT_AS tests and process
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

The requested branch is `2026-competition-round2`. Full official functional
admission remains unverified without the official driver/hidden cases. Performance
and evidence each contribute up to 40 rank-based points. No local result proves
80 points or first place. The two public documents disagree on evidence dimension
count and the default final-version selection; explicitly select the final commit.

`agent/conversation.json` contains attributed, redacted actual interactions and
observed evidence. Agent reasoning, implementation and tests are not represented
as student-authored understanding. The inherited vector.cpp IVF implementation is
experimental, covered separately by unit tests, and not used by the SQL executor.

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
