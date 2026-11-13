#!/usr/bin/env bash
# I14: full-response equivalence and failure atomicity before wider output tests.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/projection-report-results"
test ! -e "$out"
mkdir -p "$out/binaries"
build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-projection-build-XXXXXX")
stage=build
finish() {
 code=$?;trap - EXIT;set +e
 printf '{"exit_code":%d,"stage":"%s"}\n' "$code" "$stage" > "$out/run-status.json"
 python3 - "$out" <<'PY'
import sys
from pathlib import Path
for p in sorted(Path(sys.argv[1]).glob('*.json')):
 print('MINIVEC_PROJECTION_BEGIN '+p.name)
 print(p.read_text())
 print('MINIVEC_PROJECTION_END')
PY
 exit "$code"
}
trap finish EXIT
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818
docker pull "$compiler"
for side in base typed; do
 flags=()
 [[ "$side" == typed ]] && flags=(-DMVDB_ENABLE_TYPED_JSON)
 docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$repo,target=/src,readonly" \
  --mount "type=bind,source=$out,target=/out" --mount "type=bind,source=$build,target=/build" \
  --workdir /src/minivec --entrypoint /bin/bash "$compiler" -c '
  set -euo pipefail
  side=$1;shift
  common=(-O3 -std=c++17 -static -pthread -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -Isrc -Ithird_party)
  mkdir -p /build/$side
  for source in db exec sql value flat_index; do
   g++ "${common[@]}" "$@" -c src/$source.cpp -o /build/$side/$source.o
  done
  g++ "${common[@]}" "$@" src/main.cpp /build/$side/*.o -o /out/binaries/$side.bin
  g++ "${common[@]}" "$@" tests/test_typed_response.cpp /build/$side/*.o -o /build/$side/test_typed_response
  /build/$side/test_typed_response
  ' _ "$side" "${flags[@]}"
 stage="correctness-$side"
 python3 minivec/tests/test_typed_response.py --binary "$out/binaries/$side.bin" --output "$out/$side-failures.json"
 python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15
done
stage=sanitizer
g++ -O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fno-pie -no-pie \
 -fsanitize=address,undefined -DMVDB_ENABLE_TYPED_JSON -Iminivec/src minivec/tests/test_typed_response.cpp \
 minivec/src/{db,exec,sql,value,flat_index}.cpp -o "$build/typed-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$build/typed-san"
stage=paired-projections
python3 minivec/tools/benchmark_projection.py --baseline "$out/binaries/base.bin" \
 --candidate "$out/binaries/typed.bin" --output "$out/projection.json" --pairs 1 --seeds 7319 8911 13337
stage=completed
