# Official public v4 admission checks

The official branch published `minivectordb-public-v4` at commit
`9f6df2ba5acf871376e22b64a3d0724e4b575044`. It contains 20 cases and 95 SQL
statements, including expected errors, normal restart and mode switching.
[Pinned fixture release](https://github.com/oceanbase/seekdb/tree/9f6df2ba5acf871376e22b64a3d0724e4b575044/mysqltest)

The previous `188bdef` image passed 17 cases and failed `sql_comparisons`,
`sql_ordering` and `sql_bigint`. All three failed on `CREATE TABLE` because
the parser rejected the unquoted column name `value`. The candidate accepts
`value` as a column while preserving the contextual `INSERT ... VALUE` form.
It also accepts ordinary column names such as `count` while rejecting
unsupported aggregate calls.

The candidate parses vector literals directly to float32, eliminating a
reproduced double-rounding error. The decimal `16777217.000000001` rounds to
`16777218` at float32 precision. The previous implementation stored
`16777216`. On normal shutdown, the CLI explicitly rolls back an open
transaction before closing its data directory.

The local runner executes the official fixture SQL and reads the image's TSV
status output. It uses the same data directory across restarts and selects
ANN at launch for the public mode switch, which occurs outside a transaction.
The public rules require a nonzero error code until the mapping is frozen.
The runner follows that rule and checks that subsequent SQL still succeeds.
The organizer lists 29 formal cases and withholds the specific inputs for 9
of them. The public package leaves the full process protocol to the official
driver, so formal admission remains unverified.

The amd64 image built from the submission Dockerfile passed all 20 v4 cases,
including 31 query results, 31 affected-row assertions and 4 expected errors.
The extracted image's entrypoint and environment also passed live SQL framing
and acknowledgement checks, along with recovery after `SIGKILL` following a
commit acknowledgement. The image executable passed 12 additional regression
groups. The source build also passed all 5 v1 cases and 9 legacy harness cases.

The Dockerfile was built using Kaniko and the image entrypoint was exercised in
a chroot using the exported root filesystem. The image archive SHA-256 is
`6350c7b953ed5ad21fd22e405ca401cd89fd3e656800d8c5f2dedacf59da80b3`.

[Baseline run](raw/v4-baseline.log) · [Build log](raw/v4-build.log) ·
[Source checks](raw/v4-source.log) · [Image checks](raw/v4-image.log)
