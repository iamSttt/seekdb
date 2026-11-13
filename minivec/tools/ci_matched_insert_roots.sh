#!/usr/bin/env bash
# Two matched original root builds; no algorithm, flags or training changes.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(git rev-parse --show-toplevel)
cd "$repo"
out="$repo/matched-insert-root-results"
test ! -e "$out"
mkdir "$out"
container=''
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ -n "$container" ]]; then docker rm -f "$container" > "$out/container-cleanup.log" 2>&1;fi
  python3 minivec/tools/summarize_matched_insert_roots.py --repo "$repo" --output "$out" --exit-code "$code"
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  exit "$code"
}
trap finish EXIT
python3 minivec/tools/prepare_matched_insert_roots.py --stage prepare --repo "$repo" --output "$out" \
  2>&1 | tee "$out/prepare.log"
lscpu > "$out/lscpu.txt"
docker version > "$out/docker-version.txt"
for label in r2 n2; do
  dir="$out/$label"
  mkdir "$dir"
  image_tag="minivec:matched-$label-$(git rev-parse --short=12 HEAD)"
  # No build args: identical original Dockerfile defaults, separate fresh PGO.
  docker build --no-cache --platform linux/amd64 --progress=plain -t "$image_tag" "$out/$label-context" \
    2>&1 | tee "$dir/docker-build.log"
  docker image inspect "$image_tag" > "$dir/image-inspect.json"
  docker image inspect "$image_tag" --format '{{.Id}}' > "$dir/image-id.txt"
  image=$(cat "$dir/image-id.txt")
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$dir/minivec"
  docker rm "$container" > "$dir/extraction-container-removal.log"
  container=''
  chmod 755 "$dir/minivec"
  sha256sum "$dir/minivec" > "$dir/binary-sha256.txt"
  python3 protocol_check/run.py --image "$image" --handshake-only --output "$dir/image-handshake" 2>&1 | tee "$dir/image-handshake.log"
  python3 protocol_check/run.py --image "$image" --output "$dir/image-public" 2>&1 | tee "$dir/image-public.log"
  python3 minivec/tests/image_interactive.py --image "$image" --output "$dir/image-interactive" 2>&1 | tee "$dir/image-interactive.log"
  python3 minivec/tests/json_protocol.py --binary "$dir/minivec" --timeout 15 2>&1 | tee "$dir/json.log"
  python3 minivec/tests/test_typed_response.py --binary "$dir/minivec" --output "$dir/utf8.json" 2>&1 | tee "$dir/utf8.log"
  python3 minivec/tests/topk_bounds.py --binary "$dir/minivec" 2>&1 | tee "$dir/topk.log"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$dir/minivec" 2>&1 | tee "$dir/topk-parallel.log"
done
python3 minivec/tools/prepare_matched_insert_roots.py --stage bind --repo "$repo" --output "$out" \
  2>&1 | tee "$out/bind.log"
cc --version > "$out/launcher-compiler.txt"
cc -O2 -static minivec/tools/cgroup_exec.c -o "$out/cgroup_exec" 2>&1 | tee "$out/launcher-build.log"
sha256sum minivec/tools/cgroup_exec.c "$out/cgroup_exec" > "$out/launcher-sha256.txt"
for cpus in 1 2; do
  sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$repo/minivec/tools/small_compute.py" \
    --tools "$repo/minivec/tools" --identities "$out/identities.json" --launcher "$out/cgroup_exec" \
    --output "$out/measurements-${cpus}cpu.json" --allow-version-contrast --matched-insert-roots \
    --db-cpu-count "$cpus" --query-mode exact --memory-max 2147483648 --swap-max 0 \
    --cpu-quota-us "$((cpus * 100000))" --cpu-period-us 100000 --case-timeout 600 \
    2>&1 | tee "$out/measurements-${cpus}cpu.log"
done
