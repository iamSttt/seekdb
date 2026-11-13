# Retained exact-distance cost diagnostic

Run `bash ci_exact_cost.sh --repo /absolute/repo --output /absolute/new/exact-cost-results`.
The output directory must not exist. Linux requires Docker, root cgroup-v2 delegation
through passwordless sudo, Python 3, taskset and a C compiler for the existing launcher.
The fixed GCC 13.3 compiler image builds one static, non-PGO shadow executable plus
a small empty clock probe. No production file, Dockerfile or default flag is changed.

The preparer imports actual retained R/acdd and its 20 source files from the immutable
bulk-loop archive, checks the archive, manifest and individual source/ELF hashes, and
compares all 20 shared sources to canonical R33a Git objects. Nineteen are byte equal;
`exec.cpp` differs by a disabled bulk-vector branch and diagnostic counters compiled
out of production. Both SHA values and the complete diff are saved. The shadow uses
the exact historical R source associated with acdd, not a claim that current HEAD or
canonical R33a is byte-identical. The runner and transport come from the current
checkout, with hashes retained separately.

Two first lifecycles execute default 10k rows × 128 dimensions, 100-row transactions,
1000 exact TopK10 queries, half with a BIGINT equality filter. The actual R and shadow
use the existing parse/query=1 controls. DB processes get one verified CPU affinity,
one-CPU quota, 2 GiB memory and zero swap; driver/oracle remain outside. Every request
response is compared to actual R and three TopK results use the existing independent
oracle. There is no restart, update or performance-adoption ratio. Native single-task
coverage is checked at ready and phase boundaries.

`measurements.json` records all query/function counts and per-request diagnostics:
int8 invalid/cap fallback/kernel/reject, float invalid/kernel/reject, long-double
attempt/certificate, scalar-to-integer fallback, integer nonfinite fallback, and
double conservative fallback. SQL parse and CRC cover simple ingestion hotspots.
Function sample CPU uses CLOCK_THREAD_CPUTIME_ID, every 256th distance/bound call,
every 16th CRC and every parse. Scalar itself is count-only to avoid nesting a
synchronized outer timer around long double. Samples are periodic and potentially
biased, inclusive where nested, and change code layout/cache/optimization. Rough
extrapolations cannot be added or interpreted as release CPU percentages. The empty
clock probe is retained, never subtracted. Short functions near clock overhead and
rare integer fallbacks cannot be reliably timed this way.

The historical gcda evidence is separate: the actual instrumented training ELF
contains 216,113 successful positive long-double certificates and 60 scalar-to-exact
fallbacks (216,173 scalar calls). All 60 fallback calls visit equal coordinate bits,
with 10,944 skipped coordinates and zero nonzero integer accumulation paths. These
are combined 16/128/512 training counts, not default-query CPU samples. They suggest
testing long-double certification cost before changing the rare integer fallback.

Native compilation and execution remain pending until the dedicated Linux run.
The local `preflight.py` checks only archive/source preparation and reader rejection.
