#!/usr/bin/env python3
"""Import the immutable R/N executables from N's prior actual-root artifact."""
import argparse
import json
from pathlib import Path
import zipfile
from prepare_small_compute import FLAGS, digest, git, require, safe_names, save
from small_compute import insert_copy_plan
from summarize_small_root import verify_measurement

ARCHIVE = 'validation/iteration5-insert-copy-n-raw.zip'
ARCHIVE_BYTES = 3723866
ARCHIVE_SHA = 'a0e9d3e89add2e85ac841e01083ea7a29e220f2d4b65dd7cd6b3538c69278ae3'
N_COMMIT = '7554753868330006270cf39b1d32108f0a40ccac'
N_SHA = '56061188b2f84127e563e1d061452ec6cc62c8fddbdf09c23bcf4d8b155c2784'
R_COMMIT = 'e020fdeb67dd8b64b77f0f8c2905f137f0112bba'
R_SHA = 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
IMAGE = 'sha256:0d5a6f0a2757101bbca1c03dad4b4bb46d25b14dd56661eaf4b58a2e1041984e'
TOOLS = ('minivec/tools/prepare_insert_thread_control.py', 'minivec/tools/summarize_insert_thread_control.py',
    'minivec/tools/ci_insert_thread_control.sh', 'minivec/tools/small_compute.py',
    'minivec/tools/summarize_small_root.py', 'minivec/tools/summarize_insert_copy_root.py',
    'minivec/tools/prepare_small_compute.py', 'minivec/tools/cgroup_exec.c',
    'minivec/tools/benchmark_iteration.py', 'minivec/tools/benchmark_index_delta.py',
    'minivec/tools/benchmark_write_query.py', 'minivec/tools/benchmark_projection.py',
    'minivec/tests/json_protocol.py', 'protocol_check/_transport.py', 'protocol_check/config.json',
    'minivec/src/db.cpp', 'minivec/src/exec.cpp', ARCHIVE)


def import_programs(repo, out, allow_uncommitted_harness=False):
    require(not out.exists(), 'refuse overwrite')
    archive = repo / ARCHIVE
    require(archive.stat().st_size == ARCHIVE_BYTES and digest(archive.read_bytes()) == ARCHIVE_SHA,
            'N actual-root archive differs')
    with zipfile.ZipFile(archive) as bundle:
        names = safe_names(bundle)
        def read(name):
            require(name in names, 'missing original member: ' + name)
            return bundle.read(name)
        prefix = 'insert-copy-root/'
        manifest_raw = read(prefix + 'hashes.json')
        listed = json.loads(manifest_raw)['artifacts']; manifest = {v['path']: v for v in listed}
        require(len(manifest) == len(listed) and set(manifest) ==
                {n[len(prefix):] for n in names if n.startswith(prefix)} - {'hashes.json'}, 'N nested inventory differs')
        for path, item in manifest.items():
            value = read(prefix + path)
            require(len(value) == item['bytes'] and digest(value) == item['sha256'], 'N member changed: ' + path)
        summary = json.loads(read(prefix + 'summary.json'))
        require(summary['status'] == 'passed' and summary['exit_code'] == 0 and
                summary['completed_pairs'] == summary['expected_pairs'] == 24 and
                summary['automatic_adoption'] is False and summary['active_bytewise_crc_restored'] is True,
                'prior N screen incomplete or misattributed')
        root_raw = read(prefix + 'programs/root-proof.json'); root = json.loads(root_raw)
        gates_raw = read('root-gates-summary.json'); gates = json.loads(gates_raw)
        require(root['status'] == 'verified' and root['source_commit'] == gates['runtime_commit'] == N_COMMIT and
                root['binary_sha256'] == gates['image_binary_sha256'] == N_SHA and
                root['image_id'] == gates['image_id'] == IMAGE and root['feature_flags'] == gates['selected_feature_flags'] == FLAGS,
                'N root identity differs')
        require(gates['passed'] is True and len(gates['checks']) == 18 and all(v is True for v in gates['checks'].values()) and
                gates['insert_owned_copy_checks'] == {'normal': 952, 'sanitized': 952} and
                root['root_gate_summary_sha256'] == digest(gates_raw) and
                gates_raw == read(prefix + 'programs/root-gates-summary.json'), 'N root gates differ')
        image_info = json.loads(read('image-inspect.json'))
        require(len(image_info) == 1 and image_info[0]['Id'] == IMAGE and
                read('image-id.txt').decode().strip() == IMAGE and read('image-binary-sha256.txt').decode().split()[0] == N_SHA,
                'N actual image/executable metadata differs')
        input_raw = read('input-sha256.txt')
        require(input_raw == read(prefix + 'programs/root-input-sha256.txt') and
                digest(input_raw) == root['root_input_manifest_sha256'], 'N input manifest differs')
        recorded = {}
        for line in input_raw.decode().splitlines():
            value, path = line.split(maxsplit=1); path = path.lstrip('*')
            require(path not in recorded and digest(git(repo, 'show', N_COMMIT + ':' + path)) == value,
                    'N Git input differs: ' + path)
            recorded[path] = value
        expected = git(repo, 'ls-tree', '-r', '--name-only', N_COMMIT, '--', 'Dockerfile', '.dockerignore',
            'minivec/src', 'minivec/third_party', 'minivec/Makefile', 'minivec/tests', 'minivec/tools/ci_score.sh',
            'minivec/tools/ci_final_performance.sh', 'minivec/tools/pgo/train.cpp',
            'minivec/tools/pgo/run-training.sh', 'STDIO_PROTOCOL.md', 'protocol_check', 'mysqltest').decode().splitlines()
        require(recorded == root['root_input_sha256'] and set(recorded) == set(expected), 'N complete Git inventory differs')
        for path, value in root['harness_file_sha256'].items():
            require(digest(git(repo, 'show', N_COMMIT + ':' + path)) == value, 'prior harness differs: ' + path)
        standard_workload = None
        for reference in ('retained', 'ee'):
            raw = read(prefix + 'programs/' + reference + '-identities.json'); ids = json.loads(raw)
            for count in (1, 2):
                data = json.loads(read(prefix + f'{reference}-{count}cpu.json'))
                cases = tuple(dict.fromkeys(p['scenario'] for p in insert_copy_plan(reference, count)))
                verify_measurement(data, count, cases, digest(raw), ids, insert_reference=reference)
                standard = next(w for w in data['workloads'] if w['scenario'] == 'standard10k')
                require(standard_workload is None or standard == standard_workload, 'prior default workloads differ')
                standard_workload = standard
        identities_raw = read(prefix + 'programs/retained-identities.json'); ids = json.loads(identities_raw)
        originals = {}
        for role, expected_sha, expected_commit in (('baseline', R_SHA, R_COMMIT), ('candidate', N_SHA, N_COMMIT)):
            item = ids[role]
            require(item['binary_sha256'] == expected_sha and item['source_commit'] == expected_commit and
                    item['environment'] == {}, 'prior retained/N identity differs')
            program = read(prefix + 'programs/' + item['binary'])
            evidence = read(prefix + 'programs/' + item['evidence_file'])
            require(program[:6] == b'\x7fELF\x02\x01' and int.from_bytes(program[18:20], 'little') == 62 and
                    digest(program) == expected_sha and digest(evidence) == item['evidence_sha256'], 'actual R/N bytes differ')
            originals[role] = (program, evidence)
        retained = json.loads(originals['baseline'][1])
        require(retained['status'] == 'verified' and retained['feature_flags'] == FLAGS and
                retained['source_commit'] == R_COMMIT and retained['binary_sha256'] == R_SHA, 'retained proof differs')
        for path, value in retained['source_and_training_sha256'].items():
            require(digest(git(repo, 'show', R_COMMIT + ':' + path)) == value, 'retained Git input differs: ' + path)
        out.mkdir(parents=True)
        for role, (program, evidence) in originals.items():
            target = out / (role + '-minivec'); target.write_bytes(program); target.chmod(0o755)
            (out / (role + '-proof.json')).write_bytes(evidence)
            ids[role].update(binary=target.name, evidence_file=role + '-proof.json')
        save(out / 'identities.json', ids)
        for name, value in {'prior-root-gates.json': gates_raw, 'prior-root-inputs.txt': input_raw,
                            'prior-n-summary.json': read(prefix + 'summary.json'), 'prior-manifest.json': manifest_raw}.items():
            (out / name).write_bytes(value)
    revision = git(repo, 'rev-parse', 'HEAD').decode().strip()
    hashes = {path: digest((repo / path).read_bytes()) for path in TOOLS}
    require(hashes['minivec/src/db.cpp'] == '9ca1d88ce5f8fe168c5a20800cbb2d69a09f34f6496c737e8a7be9b3fddb818e' and
            hashes['minivec/src/exec.cpp'] == '366eb7a8f0bb22ae113aa9e841443f8913b4e876cc6599183bd6450f914137e5',
            'checkout retained runtime restore differs')
    matches = True
    for path, value in hashes.items():
        try: matches = matches and digest(git(repo, 'show', revision + ':' + path)) == value
        except Exception: matches = False
    proof = {'status': 'verified', 'archive_sha256': ARCHIVE_SHA, 'archive_bytes': ARCHIVE_BYTES,
        'source_commit': N_COMMIT, 'binary_sha256': N_SHA, 'retained_binary_sha256': R_SHA, 'prior_image_id': IMAGE,
        'prior_root_gates': 18, 'prior_verified_pairs': 24, 'prior_run_id': 43464055936, 'prior_artifact_id': 14057922936,
        'identity_sha256': digest((out / 'identities.json').read_bytes()), 'harness_commit': revision,
        'harness_file_sha256': hashes, 'harness_matches_git': matches,
        'new_root_builds': 0, 'new_pgo_builds': 0, 'new_database_runs': 0,
        'prior_candidate_adoption': 'not_adopted_default', 'runtime_policy_changed': False,
        'standard_workload': standard_workload,
        'checkout_runtime': 'retained db/exec restored; not built or executed by this diagnostic',
        'scope': 'Imports exact prior actual R/N ELF bytes and proofs; checks immutable archive, prior root gates/Git inputs and 24 completed prior pairs. No rebuilding, formal score or new performance claim.'}
    save(out / 'import-proof.json', proof)
    require(matches or allow_uncommitted_harness, 'current diagnostic harness differs from Git')
    return proof


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--allow-uncommitted-harness', action='store_true', help='read-only local import preflight, never Linux measurement')
    args = p.parse_args()
    proof = import_programs(args.repo.resolve(), args.output.resolve(), args.allow_uncommitted_harness)
    print(json.dumps({k: proof[k] for k in ('status', 'binary_sha256', 'retained_binary_sha256', 'harness_matches_git', 'new_root_builds')}))


if __name__ == '__main__': main()
