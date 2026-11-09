# Admission repairs after d10abbc

The formal result for `d10abbc835b5ac3c369859a08326ef7f75971b82`
reported failure of the MySQL admission gate. The visible detail did not name
the failing case or SQL and did not expose an execution log. The following
defects were reproduced independently against that image. Their presence in
the hidden run cannot be determined from that result.

## Repairs

- Equal mathematical distances could differ by one double ULP because of the
  accumulation order, selecting primary key 2 ahead of 1. The distance kernel
  now certifies the rounding of an extended-precision calculation and falls
  back to an exact integer sum when the error interval crosses a rounding
  boundary. Finite float32 inputs are summed exactly before final binary64
  rounding. Distinct exact results that round to the same double remain tied.
- VECTOR predicates bypassed dimension and finite-value validation. A
  mismatched predicate could delete a row and commit. SELECT, UPDATE and DELETE
  now share validation before execution, including LIMIT 0 and predicates
  whose earlier scalar term cannot match. Failed statements preserve earlier
  successful work in the transaction.
- Loading a snapshot required one address-space mapping of the entire file.
  Recovery now uses a 64 KiB read buffer with the existing disk encoding and
  an initial streaming CRC pass. Snapshot validation rejects truncation,
  invalid lengths, duplicate primary keys and trailing data.
- Raw tab/newline fields made different rows indistinguishable. The image's
  `tsv-status` mode now escapes tab, newline, carriage return, NUL and backslash
  in fields. Legacy `tsv` and human table modes retain their existing behavior.

The escaping is a local adapter decision based on the MySQL batch convention,
with carriage-return escaping as well. It is not evidence that the organizer's
driver uses this convention.
[MySQL batch output](https://dev.mysql.com/doc/refman/8.4/en/mysql-command-options.html#option_mysql_raw)

## Remaining integration uncertainty

The official branch remained at `9f6df2ba5acf871376e22b64a3d0724e4b575044`
during this repair. Its v4 public package contains 20 cases and 95 statements.
The README describes 29 formal cases and delegates process framing and error
mapping to the full driver. The inspected branch and release assets did not
provide that driver. The local adapter's readiness, response boundaries,
escaping and mode switching remain unverified against the formal runner.
[Pinned public package](https://github.com/oceanbase/seekdb/tree/9f6df2ba5acf871376e22b64a3d0724e4b575044/mysqltest)

The local image builder is Kaniko in an isolated chroot. Image checks exercise
the exported root filesystem and configured entrypoint through chroot. Docker
Engine is unavailable on this cloud host. No formal submission was performed
during these repairs.

## Observed validation

The final Dockerfile build and exported image checks passed. The image passed
all 20 public v4 cases and 95 SQL statements, 13 additional regression groups,
live status/escaping checks, and recovery following a SIGKILL after a commit
acknowledgement. Direct SQL against the static image returned primary key 1
for both the large-coordinate and fractional equal-distance reproductions.

The snapshot test built and recovered 256 MiB of payload under the same
160 MiB RLIMIT_AS limit, including repeated interruption of recovery and a
corrupt-checksum rejection. Snapshot recovery reported 36,376 KiB peak RSS.
The final image also reopened a 177,014,777-byte snapshot written by d10abbc
under the 160 MiB address limit. These measurements are process address/RSS
checks, separate from cgroup-wide memory accounting.

The distance kernel matched an independent exact-integer oracle bit for bit
on 6,000 randomized full-exponent and common-scale float32 cases. Source tests
also cover rounding midpoints, subnormals, cancellation and nearby distinct
distances. The image archive SHA-256 is
`bc9564fa848eab7815ecce56e6874f67db0088dd76da0635fdb401cbec6eae17`.

The precision repair has a measured performance cost. On the same local
10,000-row, 128-dimensional, indexed workload with 100 queries and seed 7319,
query time increased from 0.062837 seconds to 0.313059 seconds (4.98 times).
Both runs returned recall 1.0 for filtered and unfiltered queries and passed
restart checks. This candidate prioritizes the reproduced correctness defects;
these local timings cannot establish the formal timeout margin or score.

[Build](raw/admission-build.log) · [Source suite](raw/admission-source.log) ·
[Image suite](raw/admission-image.log) · [Large recovery](raw/admission-large.log) ·
[Image distance checks](raw/admission-distance-image.log) ·
[Distance oracle](raw/admission-distance-oracle.log) ·
[Compatibility](raw/admission-compatibility.log) ·
[Previous benchmark](raw/admission-bench-old.json) ·
[Candidate benchmark](raw/admission-bench-final.json)
