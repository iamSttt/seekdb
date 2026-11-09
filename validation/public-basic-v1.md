# Official public SQL case validation

The organizer released `minivectordb-public-basic-v1` at commit
`a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c`. The unmodified official fixtures
were downloaded from the [pinned mysqltest directory](https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest).

On 2026-11-09, the executable extracted from the Linux amd64 image in the
[container validation](container-build.md) passed all public cases on the
supplied Ubuntu 22.04 cloud host. The image archive matched SHA-256
`aef664f6e4345a995478c326f520a986521e9dd0fda160f3bc4eb25f040577d4`.

| Official case | Statements | Result |
| --- | ---: | --- |
| `sql_crud` | 9 | Pass |
| `sql_comparisons` | 7 | Pass |
| `sql_ordering` | 7 | Pass |
| `sql_empty` | 4 | Pass |
| `vector_euclidean` | 5 | Pass |

The runner uses a fresh data directory for each case and output mode. It
checks 17 queries in both default TSV and diagnostic table output, 10
affected-row assertions from the actual table-mode acknowledgements, and
successful execution of the 5 statements whose expected object is empty.
Unordered rows preserve multiplicity. Distance comparisons use relative and
absolute tolerances of `1e-5`.

This release defines sample SQL and expected results. Official process
framing, the complete driver, and the hidden functional and performance
tests remain outside this validation.
