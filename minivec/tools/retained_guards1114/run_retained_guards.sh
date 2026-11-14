#!/usr/bin/env bash
# Prepared standalone driver; no experiment has been executed by creating it.
set -euo pipefail
bundle=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(realpath "$1")
candidate=$(realpath "$2")
candidate_source=$3
candidate_evidence=$(realpath "$4")
out=$(realpath -m "$5")
test ! -e "$out"
mkdir -p "$out"
reference=33a52a7dd4426af059ec2334b7c93a07c38c93c9
finish() {
  local code=$?
  trap - EXIT
  python3 - "$out" "$code" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);code=int(sys.argv[2])
guard=p/'guards.json'
data=json.loads(guard.read_bytes()) if guard.exists() else {}
result={'exit_code':code,'status':'completed' if code==0 else 'failed_or_incomplete',
        'numerical_retained_guards_passed':code==0 and data.get('numerical_retained_guards_passed') is True,
        'release_gate_passed':False,'automatic_adoption':False,
        'scope':'Only retained protection; failure, missing guard or missing rebuilt-reference identity is never pass.'}
(p/'execution.json').write_text(json.dumps(result,indent=2)+'\n')
PY
  exit "$code"
}
trap finish EXIT
export PYTHONDONTWRITEBYTECODE=1
git -C "$repo" cat-file -e "$reference^{commit}" || git -C "$repo" fetch --no-tags --depth=1 origin "$reference"
mkdir "$out/reference-context"
git -C "$repo" archive "$reference" Dockerfile .dockerignore minivec/src minivec/third_party \
  minivec/tools/pgo/train.cpp minivec/tools/pgo/run-training.sh | tar -x -C "$out/reference-context"
image="minivec:retained-guards-1114-${GITHUB_RUN_ID:-local}"
docker build --no-cache --platform linux/amd64 --progress=plain -t "$image" "$out/reference-context" \
  2>&1 | tee "$out/reference-build.log"
container=$(docker create "$image")
docker cp "$container:/minivec" "$out/reference-binary"
docker rm "$container"
chmod +x "$out/reference-binary"
docker image inspect "$image" > "$out/reference-image.json"
python3 "$bundle/verify_reference.py" --context "$out/reference-context" --binary "$out/reference-binary" \
  --build-log "$out/reference-build.log" --image-json "$out/reference-image.json" --output "$out/reference-build-proof.json"
cc --version > "$out/launcher-compiler.txt"
cc -O2 -Wall -Wextra -static "$bundle/vendor/cgroup_exec.c" -o "$out/cgroup-exec"
sudo -n --preserve-env=PATH python3 "$bundle/retained_guards.py" \
  --baseline "$out/reference-binary" --baseline-build-proof "$out/reference-build-proof.json" \
  --candidate "$candidate" --candidate-source "$candidate_source" --candidate-evidence "$candidate_evidence" \
  --launcher "$out/cgroup-exec" --output "$out/guards.json"
