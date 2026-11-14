#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 9 ]]; then
  echo 'usage: ci_confirmation.sh EE_ELF CANDIDATE_ELF FULL_COMMIT EE_PROOF CANDIDATE_PROOF INITIAL_SCREEN INITIAL_RUNNER LAUNCHER NEW_OUTPUT' >&2
  exit 64
fi
bundle=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
python3 "$bundle/verify_sources.py"
sudo -n --preserve-env=PATH,GITHUB_RUN_ID,GITHUB_RUN_ATTEMPT,GITHUB_JOB,RUNNER_NAME,RUNNER_OS \
  python3 -B "$bundle/run_confirmation.py" \
  --baseline "$1" --candidate "$2" --candidate-source "$3" \
  --baseline-proof "$4" --candidate-proof "$5" \
  --initial-screen "$6" --initial-runner "$7" --launcher "$8" --output "$9"
