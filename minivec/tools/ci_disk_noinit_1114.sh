#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?}/disk-noinit"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-disk-noinit-XXXXXX")
baseline=760f66220f2c7186b4ba295f9e3065650e549f9e
python3 minivec/tools/disk_noinit1114/verify_inputs.py > "$out/frozen-inputs.json"
git fetch --no-tags --depth=1 origin "$baseline"
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline"
git diff --exit-code "$baseline" HEAD -- Dockerfile .dockerignore minivec/third_party
python3 - "$baseline" <<'PY'
import subprocess,sys
names=subprocess.check_output(['git','diff','--name-only',sys.argv[1],'HEAD','--','minivec/src']).decode().splitlines()
assert names==['minivec/src/flat_index.cpp'],names
PY

# Run the identical safety fixture against both implementations, then the
# candidate with ASan/UBSan. Poisoned allocations are test-only, never timed.
g++ -std=c++17 -O3 -fno-math-errno -pthread -Iminivec/src \
  minivec/tests/test_scan_noinit.cpp "$scratch/baseline/minivec/src/flat_index.cpp" \
  -Wl,--wrap=pread -o "$scratch/scan-control" > "$out/control-native-build.txt" 2>&1
timeout 180 "$scratch/scan-control" > "$out/control-native.json"
g++ -std=c++17 -O3 -fno-math-errno -pthread -Iminivec/src \
  minivec/tests/test_scan_noinit.cpp minivec/src/flat_index.cpp \
  -Wl,--wrap=pread -o "$scratch/scan-candidate" > "$out/candidate-native-build.txt" 2>&1
timeout 180 "$scratch/scan-candidate" > "$out/candidate-native.json"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer \
  -fsanitize=address,undefined -no-pie -Iminivec/src \
  minivec/tests/test_scan_noinit.cpp minivec/src/flat_index.cpp \
  -Wl,--wrap=pread -o "$scratch/scan-sanitized" > "$out/sanitized-build.txt" 2>&1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  timeout 180 "$scratch/scan-sanitized" > "$out/sanitized.json"
sha256sum "$scratch/scan-control" "$scratch/scan-candidate" "$scratch/scan-sanitized" > "$out/native-binary-sha256.txt"

docker build --platform linux/amd64 --progress=plain -t minivec:disk-noinit-parent "$scratch/baseline" 2>&1 | tee "$out/parent-build.txt"
for role in baseline candidate; do
  image="$IMAGE"
  source="$RUNTIME_COMMIT"
  if [[ "$role" == baseline ]]; then image=minivec:disk-noinit-parent; source=$baseline; fi
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
  python3 - "$source" "$scratch/$role-binary" "$out/$role-proof.json" <<'PY'
import hashlib,json,sys
from pathlib import Path
p={'status':'verified_current_root_build','source_commit':sys.argv[1],
   'binary_sha256':hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest(),
   'scope':'Actual unchanged-root Docker ELF for one source factor; not formal scoring or release.'}
Path(sys.argv[3]).write_text(json.dumps(p,indent=2)+'\n')
PY
done
echo 'fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456  '"$scratch/baseline-binary" | sha256sum -c -
python3 minivec/tests/validated_request_diff.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --expected-raw-nul-changes 0 --output "$out/request-differential.json"
python3 minivec/tests/short_decimal_protocol.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --output "$out/numeric-protocol.json"
python3 minivec/tests/shared_bound_topk.py --binary "$scratch/candidate-binary" --output "$out/shared-bound-topk.json"
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo --preserve-env=PATH python3 minivec/tools/disk_noinit1114/source_factor.py \
  --baseline "$scratch/baseline-binary" --baseline-build-proof "$out/baseline-proof.json" \
  --candidate "$scratch/candidate-binary" --candidate-source "$RUNTIME_COMMIT" \
  --candidate-evidence "$out/candidate-proof.json" --launcher "$scratch/cgroup-exec" --output "$out/guards.json"
