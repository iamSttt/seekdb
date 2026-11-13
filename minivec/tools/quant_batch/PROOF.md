# Fixed eight-row integer-distance prototype

This is an isolated prototype. No production path, index metadata, heap, cap,
quantization rule, traversal order, cache or invalidation rule has changed.

## Exact integer arithmetic

For signed int8 input, a coordinate difference is an integer in [-255,255].
It fits signed int16. A squared difference is at most 65025, and each
`madd_epi16` lane adds two such squares, at most 130050. The exceptional
signed multiply-add overflow case involving -32768 cannot occur.

In each AVX2 16-coordinate iteration, each of eight int32 lanes receives two
squares. For n <= 65536, one lane accumulates at most
`4096 * 2 * 65025 = 532684800 < 2^31`. A batch has eight independent such
accumulators, one per row; no contributions cross row boundaries. Query
sign extension is the same as the original row kernel, once per coordinate
block, shared by all eight row calculations.

Horizontal additions are hardware integer additions modulo 2^32. The exact
complete sum is at most `65536 * 65025 = 4261478400 < 2^32`. Thus the final
uint32 result is exact even when a signed interpretation of the last sum
would be negative. Residual coordinates use signed int differences, whose
products fit int, and uint32 addition. Their full sum obeys the same bound.
The proof covers the full int8 domain, wider than production codes [-127,127].

The intrinsic loop loads only while `i + 16 <= n`. The scalar tail reads
exactly coordinates i..n-1. For count 1..7, the prototype calls the unchanged
AVX2 row kernel for exactly count rows. Only output slots 0..count-1 are
written. Count 0 needs no buffers; dimension 0 writes zero without reading
or doing arithmetic on null row/query pointers. The checked entry rejects
count > 8, n > 65536, insufficient stride, missing required pointers and
size_t row-offset overflow before any write. A valid allocation and
non-overlapping output remain caller preconditions, like the old row kernel.

## Order and threshold invariance for a future integration

Computing an integer row distance does not inspect or modify the threshold.
If the existing scan consumes the staged results in exactly the old ordinal
order, and invokes the unchanged cap preparation/comparison with the current
threshold for each row, every rejection decision is unchanged. Inductively,
equal state before a row plus equal integer distance gives equal rejection,
the same exact-distance fallback and heap update, and equal state before the
next row. Threshold monotonicity is consistent with this, but is not needed
for equality: using the current cap, rather than a batch-start cap, is the
essential requirement. No row may be dropped/reordered or assigned a cap
computed for a different quantization scale.

The first heap-fill rows or a currently unusable cap may not call the old
kernel at all. Eager staging can therefore add work without changing
results. Future integration must measure this overhead and preserve these
fallback branches. This prototype does not implement or test that production
integration. Filtered, disk, local-scale, parallel traversal and other paths
are outside its proposed first integration scope.

## What the local tests do and do not establish

The independent oracle accumulates differences and squares in uint64,
checking the final uint32 range. It covers extrema, zeros, seeded random
codes, dimensions through 65536, unaligned/padded rows, every count 0..8,
invalid inputs, output sentinels, and Linux protected pages immediately
after query and final row bytes. Both original AVX2 and native dispatch
are checked as additional references. A separate generic shrinking-cap
model checks row consumption order; it is not the DB's float-bound/TopK test.

Native O3 and ASan/UBSan tests are prerequisites to timing. They have not
been executed when this source package is prepared. SIMD code generation,
register spills, CPU clock behavior and useful whole-query speedup require
actual Linux evidence. No speedup follows from this arithmetic proof.
