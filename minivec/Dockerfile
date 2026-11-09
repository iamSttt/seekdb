# Fully pinned compiler and C++ runtime; no package-manager downloads.
FROM gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818 AS build
WORKDIR /src
COPY src ./src
RUN g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc src/*.cpp -o /minivec \
 && strip /minivec \
 && mkdir /data /smoke \
 && printf 'CREATE TABLE t(id BIGINT PRIMARY KEY); INSERT INTO t VALUES(7); SELECT id FROM t;\n' | MVDB_OUT=tsv-status /minivec --data /smoke > /smoke.out \
 && printf 'Query OK, 0 rows affected\nQuery OK, 1 row affected\nid\n7\n' > /smoke.expected \
 && cmp /smoke.out /smoke.expected

FROM scratch
ENV MVDB_OUT=tsv-status
COPY --from=build /minivec /minivec
COPY --from=build /data /data
VOLUME ["/data"]
WORKDIR /data
ENTRYPOINT ["/minivec", "--data", "/data"]
