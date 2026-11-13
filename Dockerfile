# Fixed compiler; build-time oracle-checked PGO. Release contains only the database.
FROM gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 AS build
ARG ENABLE_PGO=1
ARG FEATURE_FLAGS="-DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON -DMVDB_TYPED_WIDE_VECTOR_ONLY -DMVDB_MULTI_POSTING_REUSE_ONLY -DMVDB_ENABLE_BATCH8_QUANTIZED"
WORKDIR /src
COPY minivec/src ./src
COPY minivec/third_party ./third_party
COPY minivec/tools/pgo/train.cpp /training/train.cpp
COPY minivec/tools/pgo/run-training.sh /training/run-training.sh
RUN set -eu; \
    flags='-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc'; \
    flags="$flags $FEATURE_FLAGS"; \
    case "$ENABLE_PGO" in \
      0) g++ $flags src/*.cpp -o /minivec ;; \
      1) g++ -O2 -std=c++17 -Ithird_party /training/train.cpp -o /training/train; \
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
