# Short-decimal v2: 19 digits and two parser loops

This independent directory changes no production files. V1 remains archived and unadopted: its correctness passed, but actual ingest did not improve and parent ablation showed query regression. V2 is compared to the unchanged EE/9ed parser, not to that slower v1 version.

The v2 header admits at most 19 total digits, at most 18 fraction digits, and rejects rounded double values within four double grid positions of a float midpoint. The prefix loop tries fast values until the first decline, then the byte-for-byte original loop parses the entire remainder. There is no short-path decision inside that remainder loop. See `CERTIFICATE.md` for proof, the two new near-midpoint counterexamples, unsigned conversion assumptions, and exact parser-state equivalence.

Files retain the v1 names for easy CI reuse:

- `short_decimal.h`: 19-digit bounded scanner, FP environment gate, near-midpoint guard.
- `make_snapshot.py`, `parser_snapshot.h`, `source-provenance.json`: pinned original parser and two-loop candidate. The unchanged original function must hash to `28f4031db532fe1e5f70bdb9e21e10b798e9ec26421925976224cca76655bc97`; passing an already changed v1 source is an error.
- `make_oracle.py`, `oracle.tsv`, `oracle.json`: 554,561-token integer-rational oracle plus separately labeled Python diagnostic.
- `test_short_decimal.cpp`: native differential/oracle, boundary, high-uint64, partial-error and environment tests.
- `bench_short_decimal.cpp`: the same 24 v1 scenarios and data generation; instrumentation remains outside timings.
- `freeze.json`: source hashes and preregistered test/build plan, written before native measurements.

Prepare from an unchanged EE/9ed source file:

```sh
python3 make_snapshot.py --source /path/to/unchanged/minivec/src/sql.cpp
python3 make_oracle.py --output oracle.tsv
```

Use release flags for the primary native test and microbenchmark, matching the original Docker build. Do not add rounding flags to performance runs:

```sh
g++ -O3 -std=c++17 -fno-math-errno -Wall -Wextra -Wpedantic test_short_decimal.cpp -o test-short-decimal
./test-short-decimal oracle.tsv
g++ -O3 -std=c++17 -fno-math-errno -Wall -Wextra -Wpedantic bench_short_decimal.cpp -o bench-short-decimal
taskset -c "$AVAILABLE_CPU" ./bench-short-decimal --pairs 8 --target-ms 25 > micro.jsonl
objdump -d -C bench-short-decimal > micro-disassembly.txt
```

Separate strict correctness and sanitizer builds may add `-frounding-math`:

```sh
g++ -O3 -std=c++17 -fno-math-errno -frounding-math test_short_decimal.cpp -o test-short-decimal-strict
./test-short-decimal-strict oracle.tsv
g++ -O1 -g -std=c++17 -fno-math-errno -frounding-math -fsanitize=address,undefined -fno-omit-frame-pointer test_short_decimal.cpp -o test-short-decimal-asan
ASAN_OPTIONS=detect_leaks=1 ./test-short-decimal-asan oracle.tsv
```

Disassembly must preserve the correctly rounded uint64-to-double conversion, binary64 division, and binary64-to-binary32 conversion. Inspect both the signed-range and unsigned-high-bit conversion branches. All 24 micro scenarios, including d16 repr17 and fallback-heavy negative scenarios, remain mandatory. Output `baseline_over_candidate > 1` favors v2. The full environment check, prefix scan and allocation remain timed; counts and full parsed-bit comparisons run separately. No timing claim exists yet.

Before adopting production integration, also require actual-ELF JSONL bitwise readback/atomicity/restart tests, existing protocol/SQL/public/recovery gates, actual release-image identity, and paired comparisons against both EE and 9ed. Include the new offset-minus-one and offset-plus-one midpoint decimal spellings in those real-ELF tests. Changes to trigger thresholds after seeing measurements require a separately labeled experiment rather than rewriting this premeasurement plan.
