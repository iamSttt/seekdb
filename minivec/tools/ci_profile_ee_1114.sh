#!/usr/bin/env bash
# Independent actual-EE profiling. Does not alter production Docker or ci_score.
# Usage: bash ci_profile_ee_1114.sh EE_ELF UNSTRIPPED_ELF CGROUP_EXEC OUTPUT_DIR
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
binary=$(realpath "$1")
symbols=$(realpath "$2")
launcher=$(realpath "$3")
output=$(realpath -m "$4")
test ! -e "$output"
setup="${output}-setup"
test ! -e "$setup"
mkdir -p "$setup"

# Only the ephemeral runner gets packages. No sysctl, kernel or Docker change.
perf_path=$(command -v perf || true)
if [[ -z "$perf_path" ]] || ! "$perf_path" --version > "$setup/perf-original.txt" 2>&1; then
  set +e
  sudo -n apt-get update > "$setup/apt-update.txt" 2>&1
  update_status=$?
  sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y linux-tools-common linux-tools-generic > "$setup/apt-install.txt" 2>&1
  install_status=$?
  set -e
  printf 'apt_update_exit=%s\napt_install_exit=%s\n' "$update_status" "$install_status" > "$setup/install-status.txt"
fi

# Ubuntu's /usr/bin/perf wrapper may reject a custom host kernel. Use the first
# installed real perf tool whose --version succeeds; never select a CPU/runner.
if [[ -z "$perf_path" ]] || ! "$perf_path" --version > "$setup/perf-selected.txt" 2>&1; then
  perf_path=''
  while IFS= read -r candidate; do
    if "$candidate" --version > "$setup/perf-selected.txt" 2>&1; then
      perf_path=$candidate
      break
    fi
  done < <(find /usr/lib/linux-tools -type f -name perf -executable 2>/dev/null | sort)
fi

if [[ -z "$perf_path" ]]; then
  mkdir -p "$output"
  printf '%s\n' '{"status":"blocked","reason":"No runnable perf tool after bounded runner-only installation","performance_comparison":false,"source_instrumentation":false,"sysctl_changed":false,"completed_repetitions":0,"raw_archive_transport_prepared":false}' > "$output/summary.json"
  printf '%s\n' 'Profiling blocked: no runnable perf tool. Existing correctness and benchmark results are unaffected.'
  exit 0
fi

set +e
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$script_dir/profile_ee_1114.py" \
  --binary "$binary" --symbols "$symbols" --launcher "$launcher" \
  --perf "$perf_path" --output "$output" > "$setup/profile-execution.txt" 2>&1
profile_status=$?
set -e
printf 'profile_exit=%s\n' "$profile_status" > "$setup/profile-status.txt"
if [[ ! -f "$output/summary.json" ]]; then
  mkdir -p "$output"
  printf '%s\n' '{"status":"blocked","reason":"Profiler process did not produce a summary; inspect sibling setup logs","performance_comparison":false,"source_instrumentation":false,"completed_repetitions":0,"raw_archive_transport_prepared":false}' > "$output/summary.json"
fi
# A diagnostic capability failure must not erase or reclassify existing gates.
# The summary has the actual blocked/failed/inconclusive status; do not call this
# shell's zero exit an observed successful profile.
printf 'EE profile diagnostic exit=%s; inspect %s\n' "$profile_status" "$output/summary.json"
exit 0
