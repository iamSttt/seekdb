# syntax=docker/dockerfile:1
#
# MiniVectorDB -- competition entry image.
#
# The grader talks to the database over stdin/stdout (no TCP, no network), so the
# runtime image carries the binary, libstdc++ and /data -- nothing else.
#
#   docker build -t minivec .                                              # context = this dir
#   docker build -t minivec work/scaffold                                  # context = repo root
#   docker run  -i --rm -v minivec-data:/data minivec < tests/cases/01_basic_crud.sql
#
# LAYOUT TOLERANCE (why the build stage probes instead of COPYing a fixed path):
# the grader builds from the repository root, where the source lives in
# `minivec/`; a local `docker build .` from inside `minivec/` has the source at
# the context root. The build stage accepts BOTH, because a build that only
# works in one of them is exactly the failure this file exists to avoid.
#
# ---------------------------------------------------------------------------
# stage 1: build (toolchain lives here and is never shipped)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential make coreutils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Everything the build is allowed to see is whitelisted in .dockerignore, so
# this copy is small, deterministic and contains no tests, no .git, no binary.
COPY . /ctx

# Normalise both layouts into /build/{Makefile,src}, then fail loudly (with a
# listing) if the context does not contain a buildable tree.
# The Makefile fallback exists because .gitignore (upstream seekdb rule) ignores
# every file named `Makefile`: if the recipe is ever dropped from the published
# tree again, the image must still build instead of scoring zero.
RUN set -eu; \
    mkdir -p /build/src; \
    if [ -d /ctx/minivec/src ]; then src=/ctx/minivec; \
    elif [ -d /ctx/src ]; then src=/ctx; \
    else echo 'build: no src/ under /ctx/minivec/ or /ctx -- wrong build context?' >&2; \
         ls -la /ctx >&2; exit 1; fi; \
    cp -a "$src/src/." /build/src/; \
    if [ -f "$src/Makefile" ]; then cp -a "$src/Makefile" /build/Makefile; \
    else echo 'build: WARNING no Makefile in the context -- writing the standard recipe' >&2; \
         printf 'CXX ?= g++\nCXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Isrc -pthread\nSRC := $(wildcard src/*.cpp)\nBIN := minivec\n\nall: $(BIN)\n\n$(BIN): $(SRC)\n\t$(CXX) $(CXXFLAGS) -o $@ $(SRC)\n\nclean:\n\trm -f $(BIN)\n\n.PHONY: all clean\n' > /build/Makefile; fi; \
    n_cpp=$(ls -1 /build/src/*.cpp 2>/dev/null | wc -l); \
    printf 'build: source=%s cpp_files=%s first_makefile_line=%s\n' "$src" "$n_cpp" "$(head -1 /build/Makefile)"; \
    [ "$n_cpp" -gt 0 ] || { echo 'build: no src/*.cpp in the context' >&2; ls -la /build/src >&2; exit 1; }

RUN make -j"$(nproc)" \
 && test -x ./minivec \
 && echo "build: built $(stat -c %s ./minivec) bytes"

# Ship the stripped bytes we actually tested: strip first, smoke-test second.
RUN set -eu; \
    strip --strip-unneeded ./minivec 2>/dev/null || strip ./minivec; \
    test -x ./minivec; \
    echo "build: stripped to $(stat -c %s ./minivec) bytes"

# --- runtime dependency guard ---------------------------------------------
# The runtime image carries libstdc++6 + libgcc-s1 and nothing else, so any
# other shared library the binary grows (libnuma, libz, ...) would crash at
# grading time. Fail here instead, and name the library.
RUN set -eu; \
    ldd ./minivec > /tmp/ldd.txt; \
    cat /tmp/ldd.txt; \
    if grep -q 'not found' /tmp/ldd.txt; then \
      echo 'smoke FAILED: unresolved shared library' >&2; exit 1; fi; \
    unexpected=$(grep -oE '/[^ ]+\.so[^ ]*' /tmp/ldd.txt | sed 's#.*/##' \
      | grep -vE '^(libstdc\+\+|libgcc_s|libm|libc|libpthread|libdl|librt|libatomic|ld-linux)' || true); \
    if [ -n "$unexpected" ]; then \
      echo "smoke FAILED: the runtime stage would miss: $unexpected" >&2; exit 1; fi; \
    echo 'deps OK: libstdc++/libgcc_s/libc only'

# --- build-time smoke test -------------------------------------------------
# The build FAILS if the binary does not answer. Two probes: SHOW TABLES must
# come back on stdout with exit 0, and a create/insert/select round trip must
# return the inserted rows without an ERROR line.
RUN set -eu; \
    rm -rf /tmp/smoke; mkdir -p /tmp/smoke; \
    printf 'SHOW TABLES;\n' > /tmp/smoke.sql; \
    out=$(timeout 30 ./minivec --data /tmp/smoke < /tmp/smoke.sql 2>/tmp/smoke.err) && rc=0 || rc=$?; \
    printf 'smoke[show tables]: exit=%s stdout=%s bytes\n' "$rc" "$(printf '%s' "$out" | wc -c)"; \
    printf '%s\n' "$out" | head -5; \
    [ "$rc" -eq 0 ] || { echo "smoke FAILED: SHOW TABLES exited $rc" >&2; cat /tmp/smoke.err >&2; exit 1; }; \
    [ -n "$out" ] || { echo 'smoke FAILED: SHOW TABLES produced no stdout answer' >&2; cat /tmp/smoke.err >&2; exit 1; }; \
    if printf '%s' "$out" | grep -qiE 'segmentation|core dumped|terminate called|AddressSanitizer'; then \
      echo 'smoke FAILED: crash marker in the SHOW TABLES answer' >&2; printf '%s\n' "$out" >&2; exit 1; fi; \
    rm -rf /tmp/crud; mkdir -p /tmp/crud; \
    printf 'CREATE TABLE smoke (id BIGINT PRIMARY KEY, n BIGINT);\nINSERT INTO smoke VALUES (1, 10), (2, 20);\nSELECT * FROM smoke;\n' > /tmp/crud.sql; \
    out=$(timeout 30 ./minivec --data /tmp/crud < /tmp/crud.sql 2>/tmp/crud.err) && rc=0 || rc=$?; \
    printf 'smoke[crud]: exit=%s\n' "$rc"; \
    printf '%s\n' "$out"; \
    [ "$rc" -eq 0 ] || { echo "smoke FAILED: CRUD exited $rc" >&2; cat /tmp/crud.err >&2; exit 1; }; \
    if printf '%s' "$out" | grep -qiE '^ERROR [0-9]|segmentation|core dumped'; then \
      echo 'smoke FAILED: CRUD round trip answered with an error' >&2; exit 1; fi; \
    if ! printf '%s' "$out" | grep -q '10' || ! printf '%s' "$out" | grep -q '20'; then \
      echo 'smoke FAILED: inserted rows are missing from SELECT' >&2; exit 1; fi; \
    echo 'smoke OK: SHOW TABLES + CRUD round trip answered'

# ---------------------------------------------------------------------------
# stage 2: runtime (no compiler, no ports, no network at run time)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# libstdc++6 pulls libgcc-s1; that pair is the whole C++ runtime dependency.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 libgcc-s1 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /build/minivec /app/minivec
RUN chmod 0755 /app/minivec \
 && mkdir -p /data \
 && test -x /app/minivec

# data lives on a volume so a restart keeps its contents
VOLUME ["/data"]

# stdout is the protocol, stderr carries diagnostics. The driver runs
# `docker run -i ...`, which keeps stdin open for the whole session.
# No EXPOSE / no CMD on purpose: nothing is listened on and the entrypoint is
# the entire contract.
ENTRYPOINT ["./minivec", "--data", "/data"]
