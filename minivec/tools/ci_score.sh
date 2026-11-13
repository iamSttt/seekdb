#!/usr/bin/env bash
# Reusable Linux validation entry: verify this checkout, never infer a formal score.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
# Preserve an explicitly empty FEATURE_FLAGS for an unselected-feature build.
if [[ -z ${FEATURE_FLAGS+x} ]]; then FEATURE_FLAGS='-DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON -DMVDB_TYPED_WIDE_VECTOR_ONLY -DMVDB_MULTI_POSTING_REUSE_ONLY';fi
export FEATURE_FLAGS
if [[ "${MVDB_INSERT_COPY_SCREEN:-0}" = 1 && ( "${MVDB_SMALL_BRIDGE:-0}" = 1 || "${MVDB_FINAL_PERFORMANCE:-0}" = 1 ) ]]; then
  echo 'INSERT screen cannot be combined with another performance plan' >&2;exit 2
fi
read -r -a feature_flags <<< "$FEATURE_FLAGS"
for flag in "${feature_flags[@]}"; do
  case "$flag" in
    -DMVDB_ENABLE_SPILL_FILTER|-DMVDB_ENABLE_TYPED_JSON|-DMVDB_TYPED_WIDE_VECTOR_ONLY|-DMVDB_MULTI_POSTING_REUSE_ONLY) ;;
    *) echo "Unsupported retained FEATURE_FLAGS entry: $flag" >&2;exit 2;;
  esac
done
python3 minivec/tools/validate_iteration_material.py
python3 -B minivec/tests/test_iteration_tools.py
python3 -B minivec/tests/test_report_matrix.py
export RUNTIME_COMMIT
RUNTIME_COMMIT=$(git rev-parse HEAD)
export IMAGE="${IMAGE:-minivec:verify-${RUNTIME_COMMIT:0:12}}"
# Public drivers require fresh outputs; preserve earlier runs for inspection.
test ! -e protocol-results
mkdir protocol-results
export MINIVEC_REPORTS="$repo/protocol-results"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-checks-XXXXXX")
dump_reports() {
  local result=$?
  trap - EXIT
  set +e
  python3 - <<'PYREPORT'
import json, os
from pathlib import Path
root = Path(os.environ['MINIVEC_REPORTS'])
for path in sorted(root.rglob('*')):
    if not path.is_file() or path.suffix not in {'.json','.txt'}:
        continue
    print('MINIVEC_REPORT_BEGIN '+json.dumps(path.relative_to(root).as_posix()))
    print(path.read_text(encoding='utf-8', errors='replace'))
    print('MINIVEC_REPORT_END')
PYREPORT
  local report_code=$?
  if [[ "$report_code" != 0 ]]; then
    echo "Validation report collection failed: exit $report_code" >&2
    if [[ "$result" == 0 ]]; then result=$report_code;fi
    printf '{"exit_code":%d,"report_collection_exit_code":%d}\n' \
      "$result" "$report_code" > "$MINIVEC_REPORTS/report-collection-status.json"
  fi
  exit "$result"
}
trap dump_reports EXIT

git rev-parse HEAD > protocol-results/runtime-commit.txt
git ls-files -z Dockerfile .dockerignore minivec/src minivec/third_party minivec/Makefile minivec/tests minivec/tools/ci_score.sh minivec/tools/ci_final_performance.sh minivec/tools/pgo/train.cpp minivec/tools/pgo/run-training.sh STDIO_PROTOCOL.md protocol_check mysqltest | xargs -0 sha256sum > protocol-results/input-sha256.txt
lscpu > protocol-results/lscpu.txt
docker version > protocol-results/docker-version.txt
g++ --version > protocol-results/compiler.txt
# Historical checksum-header regression only. Active db.cpp uses retained CRC.
bash minivec/tools/ci_crc_checks.sh --unit
# Build the submitted root Dockerfile
docker build --platform linux/amd64 --progress=plain --build-arg "FEATURE_FLAGS=$FEATURE_FLAGS" -t "$IMAGE" . 2>&1 | tee protocol-results/docker-build.log
docker image inspect "$IMAGE" > protocol-results/image-inspect.json
docker image inspect "$IMAGE" --format '{{.Id}}' | tee protocol-results/image-id.txt
RUNTIME_IMAGE_ID=$(cat protocol-results/image-id.txt)

# Official image handshake and unchanged 20 public cases
python3 protocol_check/run.py --image "$RUNTIME_IMAGE_ID" --handshake-only --output protocol-results/image-handshake 2>&1 | tee protocol-results/image-handshake.log
python3 protocol_check/run.py --image "$RUNTIME_IMAGE_ID" --output protocol-results/image-public 2>&1 | tee protocol-results/image-public.log

# Interactive image requests with stdin kept open
python3 minivec/tests/image_interactive.py --image "$RUNTIME_IMAGE_ID" --output protocol-results/image-interactive 2>&1 | tee protocol-results/image-interactive.log

# Existing numeric transaction recovery and sorting regressions
make -C minivec -j2 FEATURE_FLAGS="$FEATURE_FLAGS" test 2>&1 | tee protocol-results/source-regressions.log
sha256sum ./minivec/minivec > protocol-results/source-binary-sha256.txt
MVDB_PARSE_THREADS=1 ./minivec/test_sql 2>&1 | tee protocol-results/parser-serial.log
MVDB_PARSE_THREADS=2 ./minivec/test_sql 2>&1 | tee protocol-results/parser-parallel.log
g++ "${feature_flags[@]}" -O2 -std=c++17 -pthread -Iminivec/src -DMVDB_DISABLE_QUANTIZED_BOUND \
  minivec/tests/test_quantized_bound.cpp -o "$scratch/test-no-quantization"
"$scratch/test-no-quantization" 2>&1 | tee protocol-results/no-quantization.log
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --handshake-only --output protocol-results/source-handshake 2>&1 | tee protocol-results/source-handshake.log
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source-public 2>&1 | tee protocol-results/source-public.log

# Validate the binary extracted from the actual image
container=$(docker create "$RUNTIME_IMAGE_ID")
docker cp "$container:/minivec" "$scratch/image-binary"
docker rm "$container"
sha256sum "$scratch/image-binary" | tee protocol-results/image-binary-sha256.txt
python3 minivec/tests/json_protocol.py --binary "$scratch/image-binary" --timeout 15 2>&1 | tee protocol-results/image-binary-json.log
python3 minivec/tests/test_typed_response.py --binary "$scratch/image-binary" --output protocol-results/image-binary-utf8.json 2>&1 | tee protocol-results/image-binary-utf8.log
python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee protocol-results/image-binary-topk.log
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee protocol-results/image-binary-topk-parallel.log


# Native tests above include all eight staged-WAL cases. Run them explicitly
# again under sanitizers, without weakening error or durability assertions.
reports="$PWD/protocol-results"

# Source-level const-AST, destination-type, rollback and capacity ownership.
# These fixtures supplement, never replace, actual-image JSON gates above.
for mode in normal sanitized; do
  insert_options=(-O2)
  if [[ "$mode" == sanitized ]]; then
    insert_options=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie)
  fi
  g++ "${feature_flags[@]}" "${insert_options[@]}" -std=c++17 -pthread -Iminivec/src \
    minivec/tests/test_insert_owned_copy.cpp minivec/src/{db,exec,sql,value,flat_index}.cpp -o "$scratch/insert-owned-copy-$mode"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 \
    "$scratch/insert-owned-copy-$mode" 2>&1 | tee "$reports/insert-owned-copy-$mode.log"
done

# AST ownership must survive destroyed SQL input, including worker failures.
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_sql.cpp minivec/src/sql.cpp minivec/src/value.cpp -o "$scratch/sql-san"
for threads in 1 2; do
  MVDB_PARSE_THREADS="$threads" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/sql-san" 2>&1 | tee "$reports/sql-$threads-san.log"
done
# Exercise the AVX2 quantized fallback even when this runner supports AVX512.
# Native/release checks above retain normal CPU dispatch.
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -DMVDB_DISABLE_AVX512_QUANTIZED -Iminivec/src minivec/tests/test_quantized_bound.cpp -o "$scratch/quantized-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/quantized-san" 2>&1 | tee "$reports/quantized-san.log"
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_recovery_regressions.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o "$scratch/recovery-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/recovery-san" --wal-staging 2>&1 | tee "$reports/recovery-san.log"
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_query_pool_safety.cpp -Wl,--wrap=pthread_create -o "$scratch/query-pool-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" 2>&1 | tee "$reports/query-pool-san.log"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" --create-fails 2>&1 | tee "$reports/query-pool-create-fails-san.log"
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_cross_table_spill.cpp minivec/src/db.cpp minivec/src/flat_index.cpp minivec/src/value.cpp -o "$scratch/cross-spill-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/cross-spill-san" 2>&1 | tee "$reports/cross-spill-san.log"

# Budget and transactional before-image changes need their own sanitizer checks.
for name in index_budget resident_update payload_batch filter_paths; do
  extra=()
  if [ "$name" != filter_paths ]; then extra=(-DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite); fi
  if [ "$name" = index_budget ]; then extra+=(-DMVDB_DISABLE_AVX512_QUANTIZED); fi
  g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src "minivec/tests/test_${name}.cpp" minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp "${extra[@]}" -o "$scratch/${name}-san"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 "$scratch/${name}-san" 2>&1 | tee "$reports/${name}-san.log"
  if [ "$name" = index_budget ]; then
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 "$scratch/${name}-san" --ann-budget 2>&1 | tee "$reports/${name}-ann-san.log"
  fi
done
g++ "${feature_flags[@]}" -std=c++17 -O1 -pthread -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/src/*.cpp -o "$scratch/minivec-ubsan"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/json_protocol.py --binary "$scratch/minivec-ubsan" --timeout 15 2>&1 | tee "$reports/fullbinary-json-ubsan.log"

# Retained spill cache and typed/DOM boundary failures use the selected flags.
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src \
  minivec/tests/test_typed_response.cpp minivec/src/{db,exec,sql,value,flat_index}.cpp -o "$scratch/typed-response-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 "$scratch/typed-response-san" 2>&1 | tee "$reports/typed-response-san.log"
g++ "${feature_flags[@]}" -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src \
  -DMVDB_SPILL_BYTES=1 -DMVDB_TEST_WRAP_PWRITE minivec/tests/test_spill_filter.cpp \
  minivec/src/{db,exec,sql,value,flat_index}.cpp -Wl,--wrap=pread -Wl,--wrap=pwrite -o "$scratch/spill-filter-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 "$scratch/spill-filter-san" 2>&1 | tee "$reports/spill-filter-san.log"

if [[ " ${feature_flags[*]} " == *" -DMVDB_MULTI_POSTING_REUSE_ONLY "* ]]; then
  for mode in normal sanitized; do
    reuse_options=(-O2)
    if [[ "$mode" == sanitized ]]; then
      reuse_options=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie)
    fi
    g++ "${feature_flags[@]}" "${reuse_options[@]}" -std=c++17 -pthread -DMVDB_FILTER_DIAGNOSTICS -Iminivec/src \
      minivec/tests/test_filter_reuse.cpp minivec/src/{db,exec,sql,value,flat_index}.cpp -o "$scratch/filter-reuse-$mode"
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 180 \
      "$scratch/filter-reuse-$mode" 2>&1 | tee "$reports/filter-reuse-$mode.log"
  done
fi

# Cleanup must not change any official fixtures or source while tests run.
git diff --exit-code -- STDIO_PROTOCOL.md protocol_check mysqltest minivec/src minivec/third_party Dockerfile .dockerignore
if [ "${MVDB_FINAL_PERFORMANCE:-0}" = 1 ]; then
  bash minivec/tools/ci_final_performance.sh "$scratch/image-binary" \
    "$(cat protocol-results/image-id.txt)"
fi
python3 - <<'PYSUMMARY'
import json, os, re, shlex
from pathlib import Path
utf8=json.loads(Path('protocol-results/image-binary-utf8.json').read_text())
expected_utf8={(raw,dimension,path) for raw in ['ff','c080','eda080','e4b8','f4908080','80']
               for dimension in [None,127,128] for path in ['ORDER BY id LIMIT 10','unbounded','ORDER BY id LIMIT 4096']}
actual_utf8={(check['malformed_hex'],check['projected_vector_dimension'],check['path']) for check in utf8['checks']}
if (utf8.get('status')!='passed' or len(utf8['checks'])!=54 or actual_utf8!=expected_utf8 or
        any(check.get('status')!='passed' for check in utf8['checks'])):
    raise SystemExit('Actual image UTF8 atomicity evidence is incomplete')
image_binary_sha=Path('protocol-results/image-binary-sha256.txt').read_text().split()[0]
insert_checks={}
for mode in ('normal','sanitized'):
    matches=re.findall(r'^PASS INSERT owned-copy (\d+) checks$',
                       Path('protocol-results/insert-owned-copy-'+mode+'.log').read_text(),re.M)
    if len(matches)!=1 or int(matches[0])<1:
        raise SystemExit('INSERT ownership normal/sanitizer evidence is incomplete')
    insert_checks[mode]=int(matches[0])
crc_unit=json.loads(Path('protocol-results/crc-checks/unit/summary.json').read_text())
if (crc_unit.get('status')!='passed' or crc_unit.get('exit_code')!=0 or
        set(crc_unit.get('unit_checks',{}))!={'normal','sanitized'} or
        any(n<34000 for n in crc_unit['unit_checks'].values())):
    raise SystemExit('CRC normal/sanitizer evidence is incomplete')
report = {
    'runtime_commit': os.environ['RUNTIME_COMMIT'],
    'image_tag': os.environ['IMAGE'],
    'selected_feature_flags': shlex.split(os.environ['FEATURE_FLAGS']),
    'source_image_sanitizer_feature_selection_shared': True,
    'image_binary_sha256': image_binary_sha,
    'insert_owned_copy_checks': insert_checks,
    'source_binary_sha256': Path('protocol-results/source-binary-sha256.txt').read_text().split()[0],
    'image_id': Path('protocol-results/image-id.txt').read_text().strip(),
    'scope': 'current-checkout build and regression verification; not formal scoring',
    'passed': True,
    'checks': {
        'root_docker_build': True,
        'ieee_crc_byte_oracle_normal_and_sanitized': True,
        'insert_owned_copy_normal_and_sanitized': True,
        'image_official_handshake_and_public_cases': True,
        'source_official_handshake_and_public_cases': True,
        'live_stdin_errors_utf8_fragment_shutdown_restart_commit_sigkill': True,
        'make_test': True,
        'native_spill_cache_budget_writer_and_54_utf8_cases': True,
        'actual_image_54_utf8_atomicity_cases': True,
        'selected_spill_and_utf8_asan_ubsan': True,
        'parser_ownership_serial_parallel_and_quantized_bound_asan_ubsan': True,
        'image_binary_json_and_exact_topk_serial_parallel': True,
        'eight_wal_failure_cases_asan_ubsan': True,
        'query_pool_both_modes_asan_ubsan': True,
        'cross_table_spill_asan_ubsan': True,
        'full_binary_json_ubsan': True,
        'index_budget_and_resident_update_asan_ubsan': True
    },
    'sanitizer_quantized_dispatch': 'AVX512 quantized disabled for quantized_bound and index_budget to cover the AVX2 fallback; native and release tests retain default dispatch. Experimental dot/norm runtime was reverted.',
    'limitations': ['No hidden formal cases or score.',
                    'No new paired performance measurement.',
                    'Feature flags and source equivalence do not imply identical PGO binary hashes across rebuilds.',
                    'WAL fixture injects short/failed writes; not fdatasync or allocation failure.',
                    'ASan leak detection disabled; memory-safety and undefined-behavior checks enabled.']
}
if '-DMVDB_MULTI_POSTING_REUSE_ONLY' in report['selected_feature_flags']:
    for mode in ('normal', 'sanitized'):
        if 'PASS ' not in Path('protocol-results/filter-reuse-' + mode + '.log').read_text():
            raise SystemExit('Selected reuse gate lacks diagnostic/sanitizer success evidence')
    report['checks']['compound_reuse_diagnostic_and_sanitizer'] = True
if os.environ.get('MVDB_FINAL_PERFORMANCE') == '1':
    performance = json.loads(Path('protocol-results/final-performance/summary.json').read_text())
    if performance.get('status') != 'passed':
        raise SystemExit('Same-image paired measurements did not pass')
    if performance.get('total_pairs')!=120 or performance.get('expected_total_pairs')!=120:
        raise SystemExit('Expected the retained-feature 120-pair final validation plan')
    if (performance.get('candidate_image_id')!=report['image_id'] or
            performance['binary_sha256_after']['candidate']!=image_binary_sha):
        raise SystemExit('Paired measurements do not bind the verified root image binary')
    report['checks']['same_image_binary_paired_measurements'] = True
    report['performance_verified_pairs'] = performance['total_pairs']
    report['performance_evidence'] = 'final-performance/summary.json'
    report['performance_candidate_binary_sha256'] = performance['binary_sha256_after']['candidate']
    report['limitations'].remove('No new paired performance measurement.')
    report['limitations'].append('Local paired measurements bind this image/binary; future PGO rebuilds may differ.')
Path('protocol-results/verification-summary.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
PYSUMMARY

# Measure the already-gated actual root image once; keep its pre-performance
# proof immutable and mark the combined result pending until all 20 pairs pass.
if [ "${MVDB_SMALL_BRIDGE:-0}" = 1 ]; then
  cp protocol-results/verification-summary.json protocol-results/root-gates-summary.json
  python3 - <<'PYPENDING'
import json
from pathlib import Path
p=Path('protocol-results/verification-summary.json')
r=json.loads(p.read_bytes())
r['passed']=False
r['checks']['actual_root_small_compute_20_pairs']=False
r['small_compute']={'status':'pending','expected_pairs':20,'completed_pairs':0}
p.write_text(json.dumps(r,indent=2)+'\n')
PYPENDING
  bash minivec/tools/ci_small_root.sh "$scratch/image-binary" "$(cat protocol-results/image-id.txt)"
fi

if [ "${MVDB_INSERT_COPY_SCREEN:-0}" = 1 ]; then
  cp protocol-results/verification-summary.json protocol-results/root-gates-summary.json
  python3 - <<'PYINSERTPENDING'
import json
from pathlib import Path
p=Path('protocol-results/verification-summary.json')
r=json.loads(p.read_bytes())
r['passed']=False
r['checks']['actual_root_insert_copy_24_pairs']=False
r['insert_copy_screen']={'status':'pending','expected_pairs':24,'completed_pairs':0}
p.write_text(json.dumps(r,indent=2)+'\n')
PYINSERTPENDING
  bash minivec/tools/ci_insert_copy_root.sh "$scratch/image-binary" "$(cat protocol-results/image-id.txt)"
fi
