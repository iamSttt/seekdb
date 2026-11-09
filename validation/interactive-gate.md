# Interactive and storage admission checks

This follow-up starts from `0fb1c1950a69b6f0701aa17ec50d6bff7c88ff84`.
It addresses failures reproduced locally and expands executable checks without
using a formal submission attempt.

## Reproductions and changes

The previous image stalled with stdin open after a malformed bare vector
containing a quote. A semicolon inside a rejected bare vector could also split
the input and execute an embedded CREATE statement. The scanner now matches
the SQL tokenizer's bracket-depth handling. Quotes, comment markers and
semicolons within a bare vector stay in that token until the parser rejects
the invalid value. The live smoke suite exercises these cases across pipe
writes and checks that the following SQL still executes.

The new storage regression compiled against the previous source exited with
status 134 after an uncaught row-cache I/O exception. INSERT now prepares its
rebuildable payload before publishing the WAL record. Preparation and WAL
failures undo the staged row, and statement rollback preserves earlier writes
in an explicit transaction. Tests cover single-row and multi-row cache
failures, partial WAL writes, direct API calls, SQL calls, retry, commit,
rollback and restart.

The public-case runner now exercises status responses one statement at a time
with stdin held open. It collects each small response using a documented
25 ms quiet window, then checks all returned rows and rejects trailing output.
It uses SIGTERM for normal restart and reopens the same directory. Legacy TSV
is also checked in batch mode. The quiet window is a local testing convention;
formal response framing still requires the organizer's driver.

The standalone disk-index test creates 50,000 rows with 128-dimensional
vectors and 512-byte strings. It asserts that both row payload and Flat index
files are active, checks filtered and unfiltered TopK against a separate model,
and exercises index changes, rollback, failed inserts, acknowledged commits
followed by SIGKILL, SIGTERM rollback, and ANN restart. Each database process
runs under a 160 MiB RLIMIT_AS cap. The host's cgroup hierarchy is read-only,
so these results do not establish a cgroup-wide memory bound.

## Observed results

All candidate checks passed, including the source test suite, 9 legacy SQL
scenarios, all 20 public cases and 95 SQL statements in live status/batch TSV
mode, and 13 independent regression groups. The final image passed fragmented
vector/error-recovery checks, escaping checks and commit-acknowledgement crash
recovery. The 50,000-row disk-index test also passed against that image.

The Dockerfile was built with Kaniko in an isolated chroot and the exported
amd64 root filesystem was launched using its configured entrypoint. The image
archive SHA-256 is
`35fce91ef5f1a7f93490400d88b3e2e3fbec327cd2bc9dbec75539163e1b6c7c`.
The tested source and regression files were byte-compared with the uploaded
build context before committing.

These runs establish the reproduced repairs and local adapter behavior. The
formal driver's response framing, readiness and error mapping remain
unverified, as do the unpublished formal case inputs.

[Previous scanner failures](raw/interactive-old-scanner.log) ·
[Previous recovery failure](raw/interactive-old-recovery.log) ·
[Source suite](raw/interactive-source.log) · [Build](raw/interactive-build.log) ·
[Image checks](raw/interactive-image.log) ·
[Disk-index checks](raw/interactive-spilled-index.log)
