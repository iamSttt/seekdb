#!/usr/bin/env bash
# Isolated scalar-certificate prototype: no database build, PGO or package installation.
set -euo pipefail
repo= output=
while (($#)); do
  case "$1" in
    --repo) repo=$2; shift 2;;
    --output) output=$2; shift 2;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
test -n "$repo" && test -n "$output"
repo=$(cd "$repo" && pwd)
source_dir="$repo/minivec/tools/compensated_distance/frozen"
test -f "$source_dir/run_linux.py"
output=$(realpath -m "$output")
case "$output/" in "$source_dir/"*) echo 'output overlaps immutable source' >&2; exit 2;; esac
test ! -e "$output"
mkdir -p "$output"
image='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
container="minivec-compensated-distance-prototype-${GITHUB_RUN_ID:-$$}-${GITHUB_RUN_ATTEMPT:-1}"
created=0
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ "$created" == 1 ]]; then
    docker inspect "$container" > "$output/container-final-inspect.json"
    docker rm -f "$container" > "$output/container-cleanup.log" 2>&1
  fi
  printf '{"exit_code":%d,"database_runs":0,"pgo_builds":0,"container_builds":0}\n' "$code" > "$output/ci-status.json"
  exit "$code"
}
trap finish EXIT
git -C "$repo" rev-parse HEAD > "$output/harness-commit.txt"
git -C "$repo" ls-files -z minivec/tools/compensated_distance .github/workflows/minivec-score.yml .gitattributes | \
  (cd "$repo" && xargs -0 sha256sum) > "$output/input-sha256.txt"
python3 - "$source_dir" "$output" <<'PY'
import hashlib,json,sys
from pathlib import Path
src,out=map(Path,sys.argv[1:]);raw=(src/'runner-manifest.json').read_bytes()
assert hashlib.sha256(raw).hexdigest()=='404e2ac0fb136b3cf9d0580575717d795e111ad184d30e0d3c832949e4ffb175','frozen manifest changed'
manifest=json.loads(raw);members=manifest['files']
assert len(members)==16 and len({x['path'] for x in members})==16,'source inventory differs'
for item in members:
    path=Path(item['path']);assert not path.is_absolute() and '..' not in path.parts
    data=(src/path).read_bytes()
    assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'],item['path']
actual={p.relative_to(src).as_posix() for p in src.rglob('*') if p.is_file()}
assert actual=={x['path'] for x in members}|{'runner-manifest.json'},'unexpected source member'
(out/'source-manifest.json').write_bytes(raw)
(out/'source-verification.json').write_text(json.dumps({'status':'verified','members':16,'experiment_id':'compensated-distance-standalone-v1','manifest_sha256':hashlib.sha256(raw).hexdigest(),'ci_wrapper_sha256':hashlib.sha256((src.parent/'ci_prototype.sh').read_bytes()).hexdigest()},indent=2)+'\n')
PY
docker version > "$output/docker-version.txt"
docker pull --platform linux/amd64 "$image" 2>&1 | tee "$output/tooling-image-pull.log"
docker image inspect "$image" > "$output/tooling-image-inspect.json"
cpu=$(python3 -c 'import os;print(min(os.sched_getaffinity(0)))')
printf '%s\n' "$cpu" > "$output/requested-cpu.txt"
cat > "$output/container-entry.py" <<'PY'
import json,os,platform,subprocess,sys
from pathlib import Path
out=Path('/out');requested=int((out/'requested-cpu.txt').read_text())
record={'status':'checking','database_runs':0,'pgo_builds':0,'native_started':False}
def save():(out/'resource-preflight.json').write_text(json.dumps(record,indent=2)+'\n')
save()
try:
    assert platform.system()=='Linux' and platform.machine()=='x86_64','unsupported platform'
    observed={}
    for name,path in {'proc-status':'/proc/self/status','proc-cgroup':'/proc/self/cgroup','cpu-max':'/sys/fs/cgroup/cpu.max','memory-max':'/sys/fs/cgroup/memory.max','memory-swap-max':'/sys/fs/cgroup/memory.swap.max'}.items():
        value=Path(path).read_text();(out/('preflight-'+name+'.txt')).write_text(value);observed[name]=value.strip()
    record['observed']=observed;record['affinity']=sorted(os.sched_getaffinity(0));save()
    quota,period=map(int,observed['cpu-max'].split())
    assert quota==period and period>0,'actual quota must be one CPU'
    assert record['affinity']==[requested],'actual affinity differs'
    assert observed['memory-max']=='2147483648' and observed['memory-swap-max']=='0','actual memory/swap cap differs'
    version=subprocess.run(['g++','-dumpfullversion'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
    (out/'preflight-compiler-version.txt').write_text(version.stdout)
    assert version.stdout.strip()=='13.3.0','fixed compiler unavailable'
    record.update(status='verified',native_started=False);save()
except Exception as exc:
    record.update(status='failed_before_runner',error=str(exc));save();raise
# The runner owns native attempted/completed counts, including partial failures.
result=subprocess.run([sys.executable,'/prototype/run_linux.py','--output','/out/results'])
(out/'runner-exit.json').write_text(json.dumps({'exit_code':result.returncode,'database_runs':0,'pgo_builds':0})+'\n')
if result.returncode==0:
    with (out/'micro-disassembly.txt').open('w') as disassembly:
        asm=subprocess.run(['objdump','-d','-C','/out/results/micro'],stdout=disassembly,stderr=subprocess.STDOUT)
    (out/'micro-disassembly-status.json').write_text(json.dumps({'exit_code':asm.returncode,'timed':False})+'\n')
    if asm.returncode!=0:raise RuntimeError('actual micro disassembly unavailable')
post={'status':'checking','runner_exit_code':result.returncode}
try:
    post.update({name:Path('/sys/fs/cgroup/'+name).read_text().strip() for name in ('cpu.max','memory.max','memory.swap.max')})
    post['affinity']=sorted(os.sched_getaffinity(0))
    quota,period=map(int,post['cpu.max'].split())
    assert quota==period and period>0 and post['affinity']==[requested],'post-run CPU quota/affinity differs'
    assert post['memory.max']=='2147483648' and post['memory.swap.max']=='0','post-run memory/swap cap differs'
    post['status']='verified'
except Exception as exc:
    post.update(status='failed',error=str(exc))
finally:
    (out/'resource-postflight.json').write_text(json.dumps(post,indent=2)+'\n')
if result.returncode==0 and post['status']!='verified':raise RuntimeError('post-run resource verification failed')
sys.exit(result.returncode)
PY
docker create --platform linux/amd64 --name "$container" --network=none --cpuset-cpus "$cpu" --cpus=1 \
  --memory=2147483648 --memory-swap=2147483648 --pids-limit=128 \
  -e PYTHONDONTWRITEBYTECODE=1 -v "$source_dir:/prototype:ro" -v "$output:/out:rw" \
  -w /prototype "$image" bash -c 'set -euo pipefail; command -v python3; exec python3 /out/container-entry.py' \
  > "$output/container-id.txt"
created=1
docker inspect "$container" > "$output/container-start-inspect.json"
python3 - "$output" "$source_dir" <<'PY'
import json,sys
from pathlib import Path
out,src=map(Path,sys.argv[1:]);row=json.loads((out/'container-start-inspect.json').read_bytes())[0]
host=row['HostConfig'];cpu=(out/'requested-cpu.txt').read_text().strip()
assert host['NetworkMode']=='none' and host['CpusetCpus']==cpu and host['NanoCpus']==1000000000
assert host['Memory']==2147483648 and host['MemorySwap']==2147483648 and host['PidsLimit']==128
mounts={m['Destination']:m for m in row['Mounts']}
assert set(mounts)=={'/prototype','/out'}
assert mounts['/prototype']['Source']==str(src) and mounts['/prototype']['RW'] is False
assert mounts['/out']['Source']==str(out) and mounts['/out']['RW'] is True
assert row['Image']==json.loads((out/'tooling-image-inspect.json').read_bytes())[0]['Id']
PY
docker start -a "$container" 2>&1 | tee "$output/run.log"
# The attached Docker client's exit status is not proof of container success.
code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
test "$code" = 0
python3 - "$output" <<'PY'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1]);results=out/'results';summary=json.loads((results/'summary.json').read_bytes())
quota,period=map(int,(results/'cpu-max.txt').read_text().split());requested=int((out/'requested-cpu.txt').read_text())
assert quota==period and period>0
assert (results/'memory-max.txt').read_text().strip()=='2147483648'
assert (results/'memory-swap-max.txt').read_text().strip()=='0'
assert summary['affinity']=={'before':[requested],'selected':[requested]}
assert not summary.get('unavailable_resource_files')
assert summary['status']=='correctness_and_fixed_micro_completed'
assert summary['runner_manifest_sha256']=='404e2ac0fb136b3cf9d0580575717d795e111ad184d30e0d3c832949e4ffb175'
assert summary['database_runs']==summary['pgo_builds']==0
assert summary['native_compilation_attempts']==summary['native_compilations_completed']==3
assert summary['native_execution_attempts']==summary['native_executions_completed']==14
assert len(summary['pairs'])==12 and summary['plan']['counter_instrumented_timing'] is False
assert summary['plan']['whole_call'] is True and all(p['whole_call'] is True for p in summary['pairs'])
assert summary['oracle']['cases']==7842 and summary['oracle']['sha256']=='a159c84b49ad4a2c018aa208677cd67143395d362ce214fa7a34db83adcd3608'
assert summary['correctness_checks']['normal']==summary['correctness_checks']['sanitized']>0
assert json.loads((out/'micro-disassembly-status.json').read_bytes())=={'exit_code':0,'timed':False}
for item in json.loads((out/'source-manifest.json').read_bytes())['files']:
    assert summary['source_sha256'][item['path']]==item['sha256'],item['path']
for name in ('normal','sanitized','micro'):
    assert hashlib.sha256((results/name).read_bytes()).hexdigest()==summary['binary_sha256'][name]
(out/'resource-verification.json').write_text(json.dumps({'status':'verified','cpu_quota':quota,'cpu_period':period,'memory_max':2147483648,'swap_max':0,'cpu_affinity':[requested],'database_runs':0,'pgo_builds':0,'native_compilations_completed':3,'native_executions_completed':14},indent=2)+'\n')
PY
