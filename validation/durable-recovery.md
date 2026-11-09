# Recovery under interrupted or constrained writes

The `1a3a35e` image preserved the committed row after three SIGKILL tests,
but each restart retained the uncommitted tail. Its WAL grew from 194 to
12,592,388 bytes. The candidate discards a trailing transaction with no
commit record after replay, while preserving the committed prefix. It also
checks the remaining WAL file size before allocating a record buffer. With
an incomplete header advertising a 512 MiB payload and a 128 MiB address
space limit, the baseline exited with SIGABRT during startup. The rebuilt
candidate returned the committed row in that test. Its WAL returned to 194
bytes on every restart in the three SIGKILL tests.

The snapshot writer now stops serializing at the first failed write and
cleans up the incomplete temporary file. With a 256 MiB dataset, a 160 MiB
address space limit and an injected 64 KiB snapshot file limit, the candidate
reported the write failure. The prior snapshot and WAL bytes were preserved,
and recovery and a subsequent checkpoint succeeded. The baseline failed
with `std::bad_alloc` under the same limit and injected write error.

An UPDATE of a disk-backed row now stages the replacement payload before
appending the WAL record. Injected cache write failures return a statement
error and leave the old row visible. A previous successful statement in an
explicit transaction can still be committed and read after restart.

The Linux amd64 image built from the pinned GCC base passed its entrypoint
and live acknowledgement tests. All 32 public SQL statements, nine local
SQL scenarios and ten independent regression groups passed. The large
recovery test also passed six interruptions during 256 MiB WAL replay.
See the [build log](raw/durable-build.log) and [test log](raw/durable-checks.log).

The [public fixture release](https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest)
provides SQL and expected results. These local checks cover engine behavior
under the reproduced failures. Formal admission and the complete driver
remain unverified.
