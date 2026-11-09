# Functional admission follow-up

The official record for `6dd366347b093de9b9ecf2ddddd9df355bfd38ac` on
2026-11-09 at 17:18 China time reports a completed task and failed MySQL
functional admission. Performance and Agent Prompt were not scored. The
result shows no failing statement or diagnostic log.

Two problems were reproduced on the executable from the submitted build.
A successful CREATE returned no stdout acknowledgement with stdin held
open. A 4097-column table was accepted, and on restart its schema record
was treated as an invalid WAL tail, discarding a later committed table.

The new image uses `MVDB_OUT=tsv-status` to return statement acknowledgements
and affected-row counts while retaining TSV query rows. The engine validates
record limits before writing data the recovery decoder cannot read. A table
at the supported 4096-column boundary survives both WAL and snapshot reopen.

The root Dockerfile built a Linux amd64 image using its pinned GCC base and
Kaniko. Image archive SHA-256:
`0d50eb6d9d300f51c4478ede69cbc86b9b32ac76170a33cf1fc4d867a97de6a8`.
The image's declared entrypoint, working directory and environment ran in the
extracted image root on Linux. The image passed live acknowledgements, failed
batch rollback, fragmented SQL, normal restart and SIGKILL after a COMMIT
acknowledgement followed by an uncommitted insert. All 5 official public
cases (32 statements), 10 regression groups and the source test suite passed.
Disk-backed Flat, bounded sorting and TopK, and repeated interruption of
256 MiB payload recovery also passed.
[Build log](raw/gate-build.log), [image checks](raw/gate-image-test.log), and
[resource and recovery checks](raw/gate-stress.log).

The [official public fixtures](https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest)
describe SQL results and leave process framing to the full driver. The exact
acknowledgement syntax, result boundaries and readiness convention remain
unverified on that driver. These tests do not identify the organizer's
specific failing assertion or establish formal admission.
