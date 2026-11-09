# Input and transaction resource checks

Two reproducible paths could keep the previous image at `2c28c10` from returning
a response. A transactional UPDATE retained each full old row on the heap, even
when the live table contained one row. A statement spanning multiple reads was
scanned from its beginning on every read.

The candidate stores old UPDATE images in the existing disk payload file and
keeps offsets for rollback. It only spills a resident table after finding a
matching row. The input scanner carries its position and quote/comment state
across reads. The image retains semicolon framing and `tsv-status` responses.

Under a 128 MiB Linux process address-space limit, the previous image aborted
on update 7,922 of one 16 KiB row in a transaction. The candidate completed all
12,000 updates, restored the original value on rollback and retained the next
committed value across restart. This limit measures process address space. Disk
payloads still consume storage and are subject to the platform's disk budget.

The same 25,165,846-byte SQL input returned the expected query result in 13.289 s
on the previous image and 0.113 s on the candidate in a local check. The
candidate passed the C++ test suite, all 32 official public statements and
the nine local SQL scenarios. Its rebuilt Linux amd64 image also passed
live acknowledgements, fragmented input, rollback, normal restart, SIGKILL
recovery and ten independent query and transaction regression groups.

Image archive SHA-256 `aa4602721bc2d7b7b15c1e7518e042ff089ffb8571f680c0a9d48b96cccf22e6`.
[Build log](raw/robustness-build.log) and [image checks](raw/robustness-image.log).

The [official public fixture release](https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest)
provides SQL and expected results. The hidden driver and formal admission
remain unverified. The previously published failure record did not identify
its first failing assertion, so these fixes address the reproduced failures.
