#!/usr/bin/env bash
# Independent EE-root vs imported retained/CRC diagnostics; never formal scoring.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
test ! -e protocol-results
mkdir protocol-results
out="$repo/protocol-results/highest-compare"
mkdir "$out"
container=''
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ -n "$container" ]]; then docker rm -f "$container" > "$out/container-cleanup.log" 2>&1;fi
  python3 minivec/tools/summarize_highest_compare.py --repo "$repo" --output "$out" --exit-code "$code"
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  exit "$code"
}
trap finish EXIT
python3 minivec/tools/prepare_highest_compare.py --stage prepare --repo "$repo" --output "$out" 2>&1 | tee "$out/prepare.log"
lscpu > "$out/lscpu.txt"
docker version > "$out/docker-version.txt"
image_tag="minivec:highest-$(git rev-parse --short=12 HEAD)"
# The archive contains the original EE Dockerfile and complete wildcard source.
# No build-args or current feature flags, no PGO training, no source substitutions.
docker build --no-cache --platform linux/amd64 --progress=plain -t "$image_tag" "$out/ee-context" 2>&1 | tee "$out/ee-docker-build.log"
docker image inspect "$image_tag" > "$out/ee-image-inspect.json"
docker image inspect "$image_tag" --format '{{.Id}}' > "$out/ee-image-id.txt"
image=$(cat "$out/ee-image-id.txt")
mkdir "$out/programs/ee"
container=$(docker create "$image")
docker cp "$container:/minivec" "$out/programs/ee/minivec"
docker rm "$container" > "$out/extraction-container-removal.log"
container=''
chmod 755 "$out/programs/ee/minivec"
sha256sum "$out/programs/ee/minivec" > "$out/ee-binary-sha256.txt"
python3 protocol_check/run.py --image "$image" --handshake-only --output "$out/ee-image-handshake" 2>&1 | tee "$out/ee-image-handshake.log"
python3 protocol_check/run.py --image "$image" --output "$out/ee-image-public" 2>&1 | tee "$out/ee-image-public.log"
python3 minivec/tests/json_protocol.py --binary "$out/programs/ee/minivec" --timeout 15 2>&1 | tee "$out/ee-json.log"
python3 minivec/tests/topk_bounds.py --binary "$out/programs/ee/minivec" 2>&1 | tee "$out/ee-topk.log"
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$out/programs/ee/minivec" 2>&1 | tee "$out/ee-topk-parallel.log"
python3 minivec/tools/prepare_highest_compare.py --stage bind --repo "$repo" --output "$out" --image "$image" 2>&1 | tee "$out/bind.log"
cc --version > "$out/launcher-compiler.txt"
cc -O2 -static minivec/tools/cgroup_exec.c -o "$out/cgroup_exec" 2>&1 | tee "$out/launcher-build.log"
sha256sum minivec/tools/cgroup_exec.c "$out/cgroup_exec" > "$out/launcher-sha256.txt"
for label in retained crc; do
  for cpus in 1 2; do
    extra=()
    if [[ "$cpus" == 2 ]]; then extra=(--protection-only);fi
    sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$repo/minivec/tools/small_compute.py" \
      --tools "$repo/minivec/tools" --identities "$out/ee-vs-$label/identities.json" \
      --launcher "$out/cgroup_exec" --output "$out/ee-vs-$label/measurements-${cpus}cpu.json" \
      --allow-version-contrast --db-cpu-count "$cpus" "${extra[@]}" \
      --query-mode exact --memory-max 2147483648 --swap-max 0 \
      --cpu-quota-us "$((cpus * 100000))" --cpu-period-us 100000 --case-timeout 600 \
      2>&1 | tee "$out/ee-vs-$label/measurements-${cpus}cpu.log"
  done
done
