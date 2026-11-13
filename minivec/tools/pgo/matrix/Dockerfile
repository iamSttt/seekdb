# Isolated experiment only. Do not replace the root Dockerfile before evidence.
FROM gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 AS build
ARG ENABLE_PGO=0
ARG ENABLE_LTO=0
ARG PGO_SPILL_TRAINING=0
WORKDIR /src
COPY minivec/src ./src
COPY minivec/third_party ./third_party
COPY pgo/train.cpp pgo/spill-training.inc pgo/train-original.cpp pgo/run-training.sh /training/
RUN set -eu; \
    flags='-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc'; \
    case "$ENABLE_LTO" in 0) ;; 1) flags="$flags -flto=2" ;; *) echo 'ENABLE_LTO must be 0 or 1' >&2; exit 1 ;; esac; \
    case "$PGO_SPILL_TRAINING" in 0|1) ;; *) echo 'PGO_SPILL_TRAINING must be 0 or 1' >&2; exit 1 ;; esac; \
    case "$ENABLE_PGO" in \
      0) test "$PGO_SPILL_TRAINING" = 0; g++ $flags src/*.cpp -o /minivec ;; \
      1) g++ -O2 -std=c++17 -Ithird_party /training/train.cpp -o /training/train; \
         g++ -O2 -std=c++17 -Ithird_party /training/train-original.cpp -o /training/train-original; \
         mkdir /pgo; \
         g++ $flags -fprofile-generate=/pgo -fprofile-update=atomic src/*.cpp -o /minivec; \
         bash /training/run-training.sh; \
         g++ $flags -fprofile-use=/pgo -Werror=missing-profile -Werror=coverage-mismatch src/*.cpp -o /minivec ;; \
      *) echo 'ENABLE_PGO must be 0 or 1' >&2; exit 1 ;; \
    esac; \
    strip /minivec; \
    mkdir /data /smoke; \
    printf '{"id":1,"op":"shutdown"}\n' | /minivec --data /smoke > /smoke.out; \
    printf '{"type":"ready"}\n{"id":1,"ok":true}\n' > /smoke.expected; \
    cmp /smoke.out /smoke.expected

FROM scratch AS release
COPY --from=build /minivec /minivec
COPY --from=build /data /data
VOLUME ["/data"]
WORKDIR /data
ENTRYPOINT ["/minivec", "--data", "/data"]
