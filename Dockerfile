# syntax=docker/dockerfile:1
#
# MiniVectorDB -- competition entry image.
#
# The grader talks to the database over stdin/stdout (no TCP), so the runtime
# image has no ports, no compiler and needs no network: it only needs the
# binary, libstdc++ and a data directory.
#
#   docker build -t minivec .
#   docker run -i --rm -v minivec-data:/data minivec < tests/cases/01_basic_crud.sql
#
# ---------------------------------------------------------------------------
# stage 1: build (has the toolchain; never shipped)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# copy build inputs first so a source-only change does not re-copy the tree
COPY minivec/Makefile ./
COPY minivec/src ./src

RUN make -j"$(nproc)" \
 && test -x ./minivec \
 && (strip ./minivec || true)

# smoke test: the binary must start, read stdin to EOF and exit 0
RUN mkdir -p /tmp/smoke \
 && printf 'SHOW TABLES;\n' | ./minivec --data /tmp/smoke > /tmp/smoke.out \
 && test -s /tmp/smoke.out

# ---------------------------------------------------------------------------
# stage 2: runtime (no compiler, no network needed, stdin/stdout only)
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# libstdc++6/libgcc-s1 for the C++ runtime; ca-certificates is harmless and
# keeps the base consistent with the build stage.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 libgcc-s1 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/minivec ./minivec
RUN chmod 0755 ./minivec

# data lives on a volume so restarts keep their contents
RUN mkdir -p /data
VOLUME ["/data"]

# stdout is the protocol; stderr carries diagnostics only.
# ENTRYPOINT keeps stdin open for the driver (run with `docker run -i`).
ENTRYPOINT ["./minivec", "--data", "/data"]
