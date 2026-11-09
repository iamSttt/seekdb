# MiniVectorDB container validation

The build context was exported from `d90dd9028217e47caa01fc5332f37eacc7d601ca`.
The Linux amd64 build used the root Dockerfile and its original `.dockerignore`.
The pinned GCC image index resolved to the amd64 manifest
`sha256:50e85821c43ea2fd683bf5ae41f5d6981c41bab0641e7640962759291ceb7f78`.

The available Linux host prohibits creating mount and user namespaces. The
build used Kaniko v1.24.0 in a disposable chroot, with a registry mirror for
the original pinned base. The build exited successfully, including the
Dockerfile's built-in CRUD smoke check, and exported a Docker image archive.
The archive SHA-256 was `aef664f6e4345a995478c326f520a986521e9dd0fda160f3bc4eb25f040577d4`.
Its image config SHA-256 was `0519ab342590c0357c83d6dca2293ad1c3fe2ce991305d40fd85e2c83cb03ba6`.

The archive has `linux/amd64` metadata, `/data` as its volume and working
directory, and `/minivec --data /data` as its entrypoint. The executable is
x86-64 ELF with no dynamic interpreter. The entrypoint ran from the
extracted scratch root in a chroot with the configured working directory.
It passed live stdin, quoted delimiter, fragmented statement, EOF and
restart checks. The extracted executable also passed 10 independent
regression groups and all 9 SQL scenarios. Docker Engine was unavailable.
See the [build log](raw/docker-build.log) and [test log](raw/docker-image-check.log).

## Official test availability

On 2026-11-09, OceanBase's official `2026-competition-round2` branch pointed
to `c842a368c3394748d0499386c18f6b465242e141`. The complete GitHub tree response
contained 50 entries and `truncated: false`. The root `Dockerfile` and
`conversation.json` were empty. The inherited `mysqltest_for_seekdb.py` runs
seekdb's MySQL tests. The inspected branch tree and published release assets
did not provide MiniVectorDB's frozen stdin/stdout protocol or its driver.
[Official branch](https://github.com/oceanbase/seekdb/tree/2026-competition-round2)
and [complete tree](https://api.github.com/repos/oceanbase/seekdb/git/trees/c842a368c3394748d0499386c18f6b465242e141?recursive=1).

The smoke and SQL regression tests exercise the existing semicolon/TSV
adapter. The frozen driver's response framing, acknowledgements, escaping,
error format and mode selection still need verification with official material.
