#!/usr/bin/env bash
# CI adapter only. Original plan, measurements, reference build and rules stay fixed.
set -euo pipefail
if [[ $# != 5 ]]; then
  echo 'usage: ci_retained_guards.sh REPO ACTUAL_CANDIDATE_ELF FULL_COMMIT CANDIDATE_PROOF NEW_OUTPUT' >&2
  exit 64
fi
bundle=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
python3 "$bundle/verify_bundle.py"
python3 "$bundle/ci_gate.py" preflight --candidate "$2" --source "$3" --proof "$4"
set +e
bash "$bundle/run_retained_guards.sh" "$@"
code=$?
set -e
# Always produce a fail-closed CI summary when the output directory exists.
if [[ -d "$5" ]]; then
  set +e
  python3 "$bundle/ci_gate.py" finish --output "$5"
  gate_code=$?
  set -e
else
  gate_code=20
fi
if [[ $code != 0 ]]; then exit "$code"; fi
exit "$gate_code"
