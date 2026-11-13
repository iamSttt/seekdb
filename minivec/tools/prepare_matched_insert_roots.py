#!/usr/bin/env python3
"""Prepare two Git-exact original Docker contexts and bind newly built roots."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
from prepare_small_compute import FLAGS, digest, git, require, save

SOURCES = {'r2': '33a52a7dd4426af059ec2334b7c93a07c38c93c9',
           'n2': '7554753868330006270cf39b1d32108f0a40ccac'}
OLD = {'r2': 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54',
       'n2': '56061188b2f84127e563e1d061452ec6cc62c8fddbdf09c23bcf4d8b155c2784'}
DOCKER = '8014b651f2974844a411fdc1b88b9b6e46a6010eb31be14f28d909e77eeb8b5b'
CONTEXT_PATHS = ('Dockerfile', '.dockerignore', 'minivec/src', 'minivec/third_party',
                 'minivec/tools/pgo/train.cpp', 'minivec/tools/pgo/run-training.sh')
TOOL_PATHS = ('minivec/tools/prepare_matched_insert_roots.py', 'minivec/tools/summarize_matched_insert_roots.py',
    'minivec/tools/ci_matched_insert_roots.sh', 'minivec/tools/small_compute.py',
    'minivec/tools/summarize_small_root.py', 'minivec/tools/summarize_insert_copy_root.py',
    'minivec/tools/prepare_small_compute.py', 'minivec/tools/cgroup_exec.c',
    'minivec/tools/benchmark_iteration.py', 'minivec/tools/benchmark_index_delta.py',
    'minivec/tools/benchmark_write_query.py', 'minivec/tools/benchmark_projection.py',
    'minivec/tests/json_protocol.py', 'minivec/tests/topk_bounds.py', 'minivec/tests/image_interactive.py',
    'minivec/tests/test_typed_response.py', 'STDIO_PROTOCOL.md', 'protocol_check', 'mysqltest')
JSON_CHECKS = ['typed_rows', 'row_encoding_reuse', 'transactions_and_invalid_input', 'crash_recovery',
               'mode_switch', 'fragmented_jsonl', 'parallel_insert_errors', 'legacy_mode']
TOPK_PASS = 'PASS JSONL TopK against exact float32 oracle, including ties, filters and rollback'


def sha(path): return digest(path.read_bytes())


def source_files(repo, commit):
    names = git(repo, 'ls-tree', '-r', '--name-only', commit, '--', *CONTEXT_PATHS).decode().splitlines()
    require(names and len(names) == len(set(names)), 'source inventory absent/duplicate')
    files = {}
    for name in names:
        path = PurePosixPath(name)
        require(not path.is_absolute() and path.as_posix() == name and
                not any(part in ('.', '..') for part in path.parts) and '\\' not in name, 'unsafe source path')
        mode = git(repo, 'ls-tree', commit, '--', name).decode().split()[0]
        require(mode in ('100644', '100755'), 'non-regular source')
        files[name] = git(repo, 'show', commit + ':' + name)
    require(digest(files['Dockerfile']) == DOCKER, 'original root Dockerfile changed')
    return files


def prepare(repo, out, allow_uncommitted=False):
    require(not (out / 'prepare-proof.json').exists(), 'refuse overwrite')
    contexts = {label: source_files(repo, commit) for label, commit in SOURCES.items()}
    require(set(contexts['r2']) == set(contexts['n2']), 'context inventories differ')
    changed = [p for p in contexts['r2'] if contexts['r2'][p] != contexts['n2'][p]]
    require(changed == ['minivec/src/exec.cpp'], 'contexts must differ only in exec.cpp')
    for label, files in contexts.items():
        directory = out / (label + '-context'); require(not directory.exists(), 'context already exists')
        directory.mkdir(parents=True)
        for name, value in files.items():
            target = directory / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(value)
    revision = git(repo, 'rev-parse', 'HEAD').decode().strip()
    paths = set(git(repo, 'ls-files', '-z', *TOOL_PATHS).decode().rstrip('\0').split('\0'))
    # New standalone tools need explicit inventory even before local commit.
    paths.update(p for p in TOOL_PATHS if (repo / p).is_file())
    hashes = {p: sha(repo / p) for p in sorted(paths)}
    matches = True
    for path, value in hashes.items():
        try: matches = matches and digest(git(repo, 'show', revision + ':' + path)) == value
        except Exception: matches = False
    proof = {'status': 'verified', 'harness_commit': revision, 'harness_matches_git': matches,
        'harness_file_sha256': hashes, 'source_commits': SOURCES,
        'source_sha256': {label: {p: digest(v) for p, v in files.items()} for label, files in contexts.items()},
        'context_differences': changed, 'dockerfile_sha256': DOCKER, 'feature_flags': FLAGS,
        'build_args': {}, 'pgo_training_policy': 'unchanged original root default ENABLE_PGO=1',
        'new_pgo_builds_planned': 2, 'source_sanitizers_rerun': False,
        'scope': 'Two separate exact Git contexts. Original Docker/PGO/flags and all other inputs identical; only exec.cpp differs. No builds or performance yet.'}
    save(out / 'prepare-proof.json', proof)
    require(matches or allow_uncommitted, 'current matched harness differs from Git')
    return proof


def verify_inputs(repo, out, proof):
    require(proof['status'] == 'verified' and proof['harness_matches_git'] is True and
            proof['source_commits'] == SOURCES and proof['context_differences'] == ['minivec/src/exec.cpp'] and
            proof['dockerfile_sha256'] == DOCKER and proof['build_args'] == {} and proof['feature_flags'] == FLAGS,
            'matched preparation policy differs')
    require(proof['harness_commit'] == git(repo, 'rev-parse', 'HEAD').decode().strip(), 'harness commit changed')
    for path, value in proof['harness_file_sha256'].items():
        require(sha(repo / path) == value == digest(git(repo, 'show', proof['harness_commit'] + ':' + path)),
                'harness differs: ' + path)
    for label, commit in SOURCES.items():
        original = source_files(repo, commit); directory = out / (label + '-context')
        require({p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()} == set(original),
                'context inventory changed')
        require(proof['source_sha256'][label] == {p: digest(v) for p, v in original.items()}, 'source proof differs')
        for name, value in original.items(): require((directory / name).read_bytes() == value, 'context changed: ' + name)


def training_evidence(log_path, source_hashes):
    text = log_path.read_text(encoding='utf8'); manifest = None; oracles = []
    for line in text.splitlines():
        brace = line.find('{')
        if brace < 0: continue
        try: item = json.loads(line[brace:])
        except ValueError: continue
        if item.get('kind') == 'build_only_general_PGO_training':
            require(manifest is None, 'multiple training corpora in one build'); manifest = item
        if item.get('actual_file', '').startswith('responses-'):
            oracles.append({'record': item, 'json_record_sha256': digest(line[brace:].encode())})
    require(manifest is not None and manifest['seed'] == 2026111201 and manifest['phase_requests'] == [950, 40, 13],
            'original training manifest missing')
    expected = {(f'responses-{threads}-{phase}.jsonl', f'phase-{phase}.expected.jsonl', count)
                for threads in (1, 2) for phase, count in ((0, 951), (1, 41), (2, 14))}
    actual = {(v['record']['actual_file'], v['record']['expected_file'], v['record']['responses']) for v in oracles}
    require(len(oracles) == 6 and actual == expected and all(v['record']['status'] == 'passed' for v in oracles),
            'six training oracle records incomplete')
    profiles = re.findall(r'([0-9a-f]{64})\s+/pgo/(minivec-[A-Za-z_]+\.gcda)', text)
    profile_map = {name: value for value, name in profiles}
    expected_profiles = {'minivec-' + Path(p).stem + '.gcda' for p in source_hashes if p.startswith('minivec/src/') and p.endswith('.cpp')}
    require(len(profiles) == len(profile_map) == len(expected_profiles) and set(profile_map) == expected_profiles,
            'profile hash inventory incomplete')
    return {'status': 'passed', 'build_log_sha256': sha(log_path), 'training_manifest': manifest,
            'training_manifest_sha256': digest(json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()),
            'oracle_records': oracles, 'profile_sha256_from_build_log': profile_map,
            'profile_contents_archived': False, 'scope': 'Original Docker log records, not recovered gcda contents or labelled function counts.'}


def gates(repo, directory):
    public = json.loads((repo / 'mysqltest/manifest.json').read_bytes())['cases']
    require(len(public) == 20, 'public case inventory changed')
    for name, expected in (('image-handshake', {'handshake'}), ('image-public', {p['id'] for p in public})):
        data = json.loads((directory / name / 'result.json').read_bytes())
        require(data['passed'] is True and len(data['cases']) == len(expected) and
                {x['id'] for x in data['cases']} == expected and all(x['status'] == 'PASS' for x in data['cases']),
                'actual image public/handshake incomplete')
    events = json.loads((directory / 'image-interactive/events.json').read_bytes())
    require(len(events) == 20 and sum(v.get('fragmented_utf8_request') is True for v in events) == 1 and
            sum(v.get('exit_before_stdin_close') is True for v in events) == 2 and
            sum(v.get('response', {}).get('ok') is False for v in events) == 3 and
            all(v.get('stdin_open') is True for v in events if 'response' in v) and
            (directory / 'image-interactive.log').read_text().strip().startswith('PASS: official Session,'),
            'actual image interactive evidence incomplete')
    require([v for v in (directory / 'json.log').read_text().splitlines() if v.startswith('PASS ')] ==
            ['PASS ' + v for v in JSON_CHECKS], 'actual ELF JSON eight-case inventory differs')
    utf8 = json.loads((directory / 'utf8.json').read_bytes())
    expected = {(raw, dim, path) for raw in ('ff', 'c080', 'eda080', 'e4b8', 'f4908080', '80') for dim in (None, 127, 128)
                for path in ('ORDER BY id LIMIT 10', 'unbounded', 'ORDER BY id LIMIT 4096')}
    require(utf8['status'] == 'passed' and len(utf8['checks']) == 54 and
            {(v['malformed_hex'], v['projected_vector_dimension'], v['path']) for v in utf8['checks']} == expected and
            all(v['status'] == 'passed' for v in utf8['checks']), 'actual ELF UTF8 54-case inventory differs')
    for name in ('topk.log', 'topk-parallel.log'):
        require((directory / name).read_text().strip() == TOPK_PASS, 'actual ELF TopK gate incomplete')
    return {'actual_image_handshake': True, 'actual_image_public_20': True, 'actual_image_interactive': True,
            'actual_elf_json_8': True, 'actual_elf_utf8_54': True, 'actual_elf_topk_serial': True,
            'actual_elf_topk_parallel': True}


def bind(repo, out):
    proof = json.loads((out / 'prepare-proof.json').read_bytes()); verify_inputs(repo, out, proof)
    records = {}
    for label in ('r2', 'n2'):
        directory = out / label; program = directory / 'minivec'; data = program.read_bytes()
        image = (directory / 'image-id.txt').read_text().strip(); inspect = json.loads((directory / 'image-inspect.json').read_bytes())
        require(re.fullmatch('sha256:[0-9a-f]{64}', image) and len(inspect) == 1 and inspect[0]['Id'] == image and
                inspect[0]['Config']['Entrypoint'] == ['/minivec', '--data', '/data'], 'actual image differs')
        require(data[:6] == b'\x7fELF\x02\x01' and int.from_bytes(data[18:20], 'little') == 62 and
                (directory / 'binary-sha256.txt').read_text().split()[0] == digest(data), 'actual extracted ELF differs')
        train = training_evidence(directory / 'docker-build.log', proof['source_sha256'][label])
        save(directory / 'training-evidence.json', train)
        checks = gates(repo, directory); checks.update(original_root_docker_pgo=True, six_training_oracles=True)
        record = {'status': 'verified', 'label': label.upper(), 'source_commit': SOURCES[label],
            'image_id': image, 'binary_sha256': digest(data), 'feature_flags': FLAGS, 'new_pgo_builds': 1,
            'source_sha256': proof['source_sha256'][label], 'checks': checks,
            'training_evidence_sha256': sha(directory / 'training-evidence.json'),
            'prepare_proof_sha256': sha(out / 'prepare-proof.json'), 'harness_commit': proof['harness_commit'],
            'matches_imported_prior_binary': digest(data) == OLD[label], 'prior_binary_sha256_for_comparison_only': OLD[label],
            'source_sanitizers_rerun': False, 'official_binary_identity_known': False,
            'scope': 'New actual root build and listed image/ELF gates; not 18 new source/root gates. Prior source sanitizer results are historical only.'}
        save(directory / 'proof.json', record); records[label] = record
    require(records['r2']['training_evidence_sha256'] and records['n2']['training_evidence_sha256'], 'training proofs absent')
    left, right = [json.loads((out / label / 'training-evidence.json').read_bytes()) for label in ('r2', 'n2')]
    require(left['training_manifest'] == right['training_manifest'], 'training workload policies differ')
    ids = {}
    for role, label in (('baseline', 'r2'), ('candidate', 'n2')):
        ids[role] = {'binary': label + '/minivec', 'binary_sha256': records[label]['binary_sha256'],
            'source_commit': SOURCES[label], 'evidence_file': label + '/proof.json',
            'evidence_sha256': sha(out / label / 'proof.json'), 'evidence_binary_pointer': ['binary_sha256'],
            'evidence_commit_pointer': ['source_commit'], 'environment': {}}
    save(out / 'identities.json', ids)
    save(out / 'binding-proof.json', {'status': 'verified', 'prepare_proof_sha256': sha(out / 'prepare-proof.json'),
        'identity_sha256': sha(out / 'identities.json'), 'programs': records, 'new_pgo_builds': 2,
        'source_sanitizers_rerun': False, 'automatic_adoption': False})
    return records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--stage', choices=('prepare', 'bind'), required=True)
    p.add_argument('--allow-uncommitted-harness', action='store_true')
    args = p.parse_args()
    result = prepare(args.repo.resolve(), args.output.resolve(), args.allow_uncommitted_harness) if args.stage == 'prepare' else bind(args.repo.resolve(), args.output.resolve())
    print(json.dumps({'status': 'verified', 'stage': args.stage, 'result_keys': sorted(result)}))


if __name__ == '__main__': main()
