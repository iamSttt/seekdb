# Independent EE sampling

Run after, and separately from, correctness and paired timing jobs:

```sh
bash minivec/tools/ci_profile_ee_1114.sh \
  /absolute/actual-ee /absolute/unstripped-ee \
  /absolute/cgroup_exec /absolute/protocol-results/ee-profile
```

The caller supplies the actual extracted original EE ELF. Its SHA256 must be
`9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd`.
The supplied unstripped executable must produce exactly those bytes when stripped.
Symbols are added to a private perf build-ID cache; the sampled program remains
the original stripped ELF. No source instrumentation, production rebuild, syscall
policy change or `sysctl` adjustment is performed by these scripts.

The fixed workload is twelve fresh full lifecycles at one logical CPU and a
one-CPU quota, 2 GiB and swap0. Each lifecycle reuses the unchanged historical
10k ×128 generator, seed7319, 100-row transactions, 1000 exact queries, sampled
independent TopK oracle and full restart validation. Startup/index/restart are
not sampled. Ingest starts immediately before the first BEGIN and ends before
CREATE VECTOR INDEX. Query starts after that index response and ends before the
first shutdown. Existing lifecycle timers include observer overhead and are not
performance-comparison results.

`perf record` attaches to the actual PID, inheriting its threads, and collects
flat user-space CPU-clock IP samples at999Hz. Acknowledged perf control-fd
enable/disable commands bracket both phases. Hardware cycles/instructions are
attempted separately with perf stat. Unsupported hardware counters remain
explicitly blocked; software profiles can still be useful independently. No
off-CPU waits, reliable call chains or exact function CPU-time claims are made.

A phase needs at least300 aggregate samples and at most20% unresolved samples
for the summary to label its flat profile usable. Insufficient samples or symbol
coverage remains inconclusive; the program does not repeat adaptively. All runs,
negative or unavailable evidence, raw perf data, tool commands, host/limits,
response hashes and oracle records are retained. No failure is replaced by a
new host or a favorable sample.

`summary.json` and `profile-compact.txt` identify capabilities and status.
`perf-raw.tar.gz.b64.txt` transports the raw data through the existing JSON/text
CI log trap. Decode it and verify compressed SHA256/bytes against
`perf-raw-archive.json` before extraction. The compressed payload cap is8MiB;
exceeding it emits an explicit raw-not-transported status, never a truncated
archive or a claim that raw evidence was recovered. A prepared transport is not
confirmation that the parent downloaded it. The shell exits zero even for a
blocked optional diagnostic so earlier CI gates survive; inspect the summary.

The implementation follows upstream perf documentation for
[record control and inheritance](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-record.txt),
[stat control and output](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-stat.txt),
and [private build-ID cache selection](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf.txt).
Local preparation checks are not evidence of a successful Linux profiling run.
