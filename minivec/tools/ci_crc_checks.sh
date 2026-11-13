#!/usr/bin/env bash
# Two distinct stages: CRC unit tests, then old/new persistent-format interchange.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(git rev-parse --show-toplevel)
cd "$repo"
mode=${1:-}
case "$mode" in
  --unit) [[ $# == 1 ]]; stage=unit ;;
  --compat) [[ $# == 4 ]]; stage=compat ;;
  *) echo 'Usage: ci_crc_checks.sh --unit | --compat ACTUAL_BINARY IMAGE_ID RETAINED_DIR' >&2; exit 2 ;;
esac
out="$repo/protocol-results/crc-checks/$stage"
test ! -e "$out"
mkdir -p "$out"
finish() {
  local code=$?
  trap - EXIT
  set +e
  python3 - "$out" "$stage" "$code" <<'PYCOLLECT'
import hashlib,json,re,sys
from pathlib import Path
out=Path(sys.argv[1]);stage=sys.argv[2];code=int(sys.argv[3])
summary={'status':'failed_or_incomplete','stage':stage,'exit_code':code,
         'formal_submission':False,'performance_measurements':0}
try:
    if stage=='unit':
        checks={}
        for name in ('normal','sanitized'):
            text=(out/(name+'.log')).read_text()
            found=re.fullmatch(r'PASS crc32 IEEE slicing-by-8 ([0-9]+) checks\n?',text)
            if not found or int(found.group(1))<34000: raise ValueError('incomplete CRC unit evidence: '+name)
            checks[name]=int(found.group(1))
        summary['unit_checks']=checks
    else:
        result=json.loads((out/'persistence.json').read_bytes())
        if result.get('status')!='passed' or len(result.get('checks',[]))!=18:
            raise ValueError('incomplete interchange evidence')
        identity=json.loads((out/'identity.json').read_bytes())
        for role in ('baseline','candidate'):
            if result['binaries'][role]['sha256']!=identity[role+'_binary_sha256']:
                raise ValueError('actual protocol executable differs')
        summary.update(compatibility_checks=18,actual_reader_binary_sha256={
            role:result['binaries'][role]['sha256'] for role in ('baseline','candidate')},
            fixture_writer_binary_sha256={role:result['binaries'][role+'_writer']['sha256'] for role in ('baseline','candidate')},
            candidate_image_id=identity['candidate_image_id'],candidate_source_commit=identity['candidate_source_commit'],
            fixture_writer_scope='Standalone format-fixture helpers, not the actual PGO database executables.')
    if code!=0: raise ValueError('stage command failed')
    summary['status']='passed'
except Exception as error:
    summary['error']=type(error).__name__+': '+str(error)
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
manifest={'artifacts':[{'path':p.relative_to(out).as_posix(),'bytes':p.stat().st_size,
    'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
    for p in sorted(out.rglob('*')) if p.is_file() and p.name!='hashes.json']}
(out/'hashes.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(summary))
sys.exit(0 if summary['status']=='passed' else 1)
PYCOLLECT
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  exit "$code"
}
trap finish EXIT
g++ --version > "$out/compiler.txt"
git rev-parse HEAD > "$out/harness-commit.txt"

if [[ "$stage" == unit ]]; then
  python3 - "$repo" "$out" <<'PYUNIT'
import hashlib,json,subprocess,sys
from pathlib import Path
repo=Path(sys.argv[1]);out=Path(sys.argv[2])
paths=('minivec/src/crc32_ieee.h','minivec/tests/test_crc32_ieee.cpp','minivec/tools/ci_crc_checks.sh')
records={}
for name in paths:
    raw=(repo/name).read_bytes()
    expected=subprocess.check_output(['git','show','HEAD:'+name],cwd=repo)
    if raw!=expected: raise ValueError('CRC unit source differs from Git: '+name)
    target=out/'source'/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    records[name]=hashlib.sha256(raw).hexdigest()
(out/'source-sha256.json').write_text(json.dumps(records,indent=2)+'\n')
PYUNIT
  g++ -O2 -std=c++17 -Wall -Wextra -Iminivec/src minivec/tests/test_crc32_ieee.cpp \
    -o "$out/crc-normal" 2>&1 | tee "$out/normal-build.log"
  timeout 120 "$out/crc-normal" 2>&1 | tee "$out/normal.log"
  g++ -O1 -g -std=c++17 -Wall -Wextra -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie -Iminivec/src \
    minivec/tests/test_crc32_ieee.cpp -o "$out/crc-sanitized" 2>&1 | tee "$out/sanitized-build.log"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    timeout 120 "$out/crc-sanitized" 2>&1 | tee "$out/sanitized.log"
  exit 0
fi

binary=$(realpath "$2")
image=$3
retained=$(realpath "$4")
python3 - "$repo" "$out" "$binary" "$image" "$retained" <<'PYPREPARE'
import hashlib,json,re,subprocess,sys
from pathlib import Path,PurePosixPath
repo,out,binary=map(Path,sys.argv[1:4]);image=sys.argv[4];retained=Path(sys.argv[5])
old_commit='7ca3baad3f4c71970b01d194e8eee88a3606ff3b'
baseline_sha='acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
flags=['-DMVDB_ENABLE_SPILL_FILTER','-DMVDB_ENABLE_TYPED_JSON','-DMVDB_TYPED_WIDE_VECTOR_ONLY','-DMVDB_MULTI_POSTING_REUSE_ONLY']
def sha(raw): return hashlib.sha256(raw).hexdigest()
def git(*args): return subprocess.check_output(['git',*args],cwd=repo)
def require(ok,why):
    if not ok: raise ValueError(why)
revision=git('rev-parse','HEAD').decode().strip()
require(re.fullmatch('sha256:[0-9a-f]{64}',image) is not None,'complete image ID required')
provenance_raw=(retained/'provenance.json').read_bytes();provenance=json.loads(provenance_raw)
ids=json.loads((retained/'identities.json').read_bytes())
prior=ids['baseline'];baseline=retained/prior['binary']
require(provenance['status']=='verified' and provenance['harness_matches_git'] is True,'retained import not verified')
require(prior['evidence_file']=='provenance.json' and prior['evidence_sha256']==sha(provenance_raw),'retained proof hash differs')
require(provenance['source_commit']==prior['source_commit']=='e020fdeb67dd8b64b77f0f8c2905f137f0112bba','old actual source differs')
require(provenance['binary_sha256']==prior['binary_sha256']==sha(baseline.read_bytes())==baseline_sha,'old actual binary differs')
require(provenance['feature_flags']==flags,'retained feature policy differs')
gate_raw=(repo/'protocol-results/root-gates-summary.json').read_bytes();gate=json.loads(gate_raw)
candidate_sha=sha(binary.read_bytes())
require(gate.get('passed') is True and all(x is True for x in gate['checks'].values()),'actual root gates incomplete')
require(gate['runtime_commit']==revision and gate['image_id']==image and gate['selected_feature_flags']==flags,'actual root identity differs')
require(gate['image_binary_sha256']==candidate_sha,'actual root binary differs')
require((repo/'protocol-results/image-id.txt').read_text().strip()==image,'image record differs')
require((repo/'protocol-results/image-binary-sha256.txt').read_text().split()[0]==candidate_sha,'root extraction hash differs')
source_hashes={}
for role,commit in (('baseline',old_commit),('candidate',revision)):
    paths=git('ls-tree','-r','--name-only',commit,'--','minivec/src').decode().splitlines()
    require(paths,'missing complete source snapshot')
    source_hashes[role]={}
    for path in paths:
        relative=PurePosixPath(path)
        require(relative.as_posix()==path and not relative.is_absolute() and '..' not in relative.parts and '\\' not in path,
                'unsafe Git source path')
        raw=git('show',commit+':'+path)
        if role=='candidate': require((repo/path).read_bytes()==raw,'candidate source differs from Git')
        target=out/'source'/role/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
        source_hashes[role][path]=sha(raw)
require(source_hashes['baseline']['minivec/src/db.cpp']==provenance['source_and_training_sha256']['minivec/src/db.cpp'],
        'old writer storage source differs from retained baseline proof')
tools={}
for path in ('minivec/tests/crc_persistence_fixture.cpp','minivec/tests/check_crc_persistence.py','minivec/tools/ci_crc_checks.sh'):
    raw=(repo/path).read_bytes();require(raw==git('show',revision+':'+path),'compatibility harness differs from Git')
    target=out/'source/common'/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw);tools[path]=sha(raw)
(out/'retained-provenance.json').write_bytes(provenance_raw)
(out/'root-gates-summary.json').write_bytes(gate_raw)
identity={'status':'verified','baseline_binary_sha256':baseline_sha,'baseline_source_commit':prior['source_commit'],
    'candidate_binary_sha256':candidate_sha,'candidate_source_commit':revision,'candidate_image_id':image,
    'old_fixture_source_commit':old_commit,'new_fixture_source_commit':revision,'feature_flags':flags,
    'fixture_compile_options':['-O2','-std=c++17','-pthread'],
    'fixture_source_sha256':tools['minivec/tests/crc_persistence_fixture.cpp'],'source_sha256':source_hashes,'harness_sha256':tools,
    'scope':'Two standalone fixture writers, not PGO programs. Actual protocol readers are the retained imported executable and the current root-image executable.'}
(out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
(out/'baseline-reader-path.txt').write_text(str(baseline)+'\n')
print(json.dumps({key:identity[key] for key in ('status','baseline_binary_sha256','candidate_binary_sha256','candidate_image_id')}))
PYPREPARE

flags=(-DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON -DMVDB_TYPED_WIDE_VECTOR_ONLY -DMVDB_MULTI_POSTING_REUSE_ONLY)
fixture="$out/source/common/minivec/tests/crc_persistence_fixture.cpp"
for role in baseline candidate; do
  source="$out/source/$role/minivec/src"
  g++ -O2 -std=c++17 -pthread "${flags[@]}" -I"$source" "$fixture" \
    "$source/db.cpp" "$source/flat_index.cpp" "$source/value.cpp" \
    -o "$out/$role-fixture-writer" 2>&1 | tee "$out/$role-writer-build.log"
done
baseline=$(cat "$out/baseline-reader-path.txt")
python3 "$out/source/common/minivec/tests/check_crc_persistence.py" \
  --baseline "$baseline" --candidate "$binary" \
  --baseline-writer "$out/baseline-fixture-writer" --candidate-writer "$out/candidate-fixture-writer" \
  --output "$out/persistence.json" 2>&1 | tee "$out/persistence.log"
