#!/usr/bin/env python3
"""Verify archived inputs, import two historical executables, bind one new EE image."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import zipfile
from prepare_small_compute import digest, git, require, safe_names, save, FLAGS, COMPILER

EE = 'ee299adb296a2922d01982aa6c5707d37e767b41'
EE_TREE = '18e18607350d36f5e7b701512d68afdd99df0628'
RETAINED = 'e020fdeb67dd8b64b77f0f8c2905f137f0112bba'
CRC = '780be2c8954ce5706f44abba5681d7326e41a27e'
EE_ARCHIVE = 'validation/iteration5-ee299-root-source.zip'
CRC_ARCHIVE = 'validation/iteration5-crc-root-raw.zip'
EE_ARCHIVE_SHA = 'ffe5ea77bd0c102b0f3658fa6c5c46a4834c77acb2a2a9365b96b3aea5ca52e4'
CRC_ARCHIVE_SHA = '70a0383fc86708ab680315fd9b98d5573d8dedc22f0bf70ea055e6abd7ab11b8'
RETAINED_SHA = 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
CRC_SHA = '8bf702f8b87df81f886df9cff12d72ec27e5a600e367fad0b97de19526607f20'
TOOLS = ('minivec/tools/ci_highest_compare.sh', 'minivec/tools/prepare_highest_compare.py',
         'minivec/tools/summarize_highest_compare.py', 'minivec/tools/prepare_small_compute.py',
         'minivec/tools/summarize_small_root.py', 'minivec/tools/small_compute.py',
         'minivec/tools/benchmark_iteration.py', 'minivec/tools/benchmark_index_delta.py',
         'minivec/tools/benchmark_write_query.py', 'minivec/tools/benchmark_projection.py',
         'minivec/tools/cgroup_exec.c', 'minivec/tests/json_protocol.py',
         'minivec/tests/topk_bounds.py')
MEASURED_TOOLS = {'minivec/tools/small_compute.py', 'minivec/tools/benchmark_iteration.py',
                  'minivec/tools/benchmark_index_delta.py', 'minivec/tools/benchmark_write_query.py',
                  'minivec/tools/benchmark_projection.py', 'minivec/tests/json_protocol.py',
                  'protocol_check/_transport.py', 'protocol_check/config.json'}


def sha(path):
    return digest(path.read_bytes())


def oid(kind, raw):
    return hashlib.sha1(kind.encode() + b' ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()


def tree_entries(raw):
    result = {}; offset = 0
    while offset < len(raw):
        end = raw.index(b'\0', offset)
        mode, name = raw[offset:end].split(b' ', 1)
        require(end + 21 <= len(raw), 'truncated Git tree')
        name = name.decode('utf8')
        require(name not in result and name not in ('.', '..') and '/' not in name, 'invalid Git tree entry')
        result[name] = (mode.decode(), raw[end + 1:end + 21].hex())
        offset = end + 21
    return result


def ee_source(archive):
    require(archive.stat().st_size == 232726 and sha(archive) == EE_ARCHIVE_SHA, 'EE source archive differs')
    with zipfile.ZipFile(archive) as bundle:
        names = safe_names(bundle)
        raw = bundle.read('manifest.json'); manifest = json.loads(raw)
        require(manifest['source_commit'] == EE and manifest['full_tree'] == EE_TREE, 'EE identity differs')
        commit = base64.b64decode(manifest['git_commit_raw_base64'], validate=True)
        require(oid('commit', commit) == EE and commit.splitlines()[0] == ('tree ' + EE_TREE).encode(),
                'EE commit/tree object differs')
        trees = {}
        for key, encoded in manifest['git_tree_objects_base64'].items():
            value = base64.b64decode(encoded, validate=True)
            require(oid('tree', value) == key, 'Git tree hash differs')
            trees[key] = tree_entries(value)
        root = trees[EE_TREE]
        minivec = trees[root['minivec'][1]]
        expected = {'.dockerignore', 'Dockerfile', 'minivec/Makefile'}
        for directory in ('src', 'third_party'):
            entries = trees[minivec[directory][1]]
            require(all(mode == '100644' for mode, _ in entries.values()), 'unexpected nested/nonregular source')
            expected.update('minivec/' + directory + '/' + name for name in entries)
        require(set(manifest['files']) == expected and names == {'manifest.json'} | {'context/' + p for p in expected},
                'complete EE source inventory differs')
        files = {}
        for path, entry in manifest['files'].items():
            parts = path.split('/'); at = root
            for component in parts[:-1]:
                mode, key = at[component]
                require(mode == '40000', 'non-tree source ancestor')
                at = trees[key]
            mode, blob = at[parts[-1]]
            value = bundle.read('context/' + path)
            require(mode == entry['mode'] == '100644' and blob == entry['git_blob'] == oid('blob', value) and
                    len(value) == entry['bytes'] and digest(value) == entry['sha256'], 'EE source differs: ' + path)
            files[path] = value
    return manifest, raw, files


def checked_git_inputs(repo):
    revision = git(repo, 'rev-parse', 'HEAD').decode().strip()
    package = git(repo, 'ls-files', '-z', 'protocol_check', 'mysqltest').decode().split('\0')
    paths = set(TOOLS) | set(filter(None, package)) | {EE_ARCHIVE, CRC_ARCHIVE}
    values = {path: sha(repo / path) for path in sorted(paths)}
    for path, value in values.items():
        require(digest(git(repo, 'show', revision + ':' + path)) == value, 'input differs from committed Git: ' + path)
    return revision, values


def verify_git_inputs(repo, proof):
    require(git(repo, 'rev-parse', 'HEAD').decode().strip() == proof['harness_commit'], 'checkout moved')
    package = git(repo, 'ls-files', '-z', 'protocol_check', 'mysqltest').decode().split('\0')
    require(set(proof['git_input_sha256']) == set(TOOLS) | set(filter(None, package)) | {EE_ARCHIVE, CRC_ARCHIVE},
            'Git input inventory changed')
    for path, value in proof['git_input_sha256'].items():
        require(sha(repo / path) == value and digest(git(repo, 'show', proof['harness_commit'] + ':' + path)) == value,
                'Git/input changed: ' + path)


def import_crc(repo, out):
    archive = repo / CRC_ARCHIVE
    require(archive.stat().st_size == 3020045 and sha(archive) == CRC_ARCHIVE_SHA, 'CRC artifact differs')
    with zipfile.ZipFile(archive) as bundle:
        names = safe_names(bundle)
        prefix = 'small-compute-root/'
        listing = json.loads(bundle.read(prefix + 'hashes.json'))['artifacts']
        indexed = {entry['path']: entry for entry in listing}
        require(len(indexed) == len(listing) and {prefix + p for p in indexed} ==
                {p for p in names if p.startswith(prefix) and p != prefix + 'hashes.json'}, 'prior manifest inventory differs')
        for path, entry in indexed.items():
            value = bundle.read(prefix + path)
            require(len(value) == entry['bytes'] and digest(value) == entry['sha256'], 'prior member differs: ' + path)
        prior = json.loads(bundle.read(prefix + 'summary.json'))
        root = json.loads(bundle.read('verification-summary.json'))
        require(prior['status'] == 'passed' and prior['exit_code'] == 0 and prior['completed_pairs'] == prior['expected_pairs'] == 20,
                'prior CRC measurement incomplete')
        require(root['passed'] is True and len(root['checks']) >= 19 and all(v is True for v in root['checks'].values()),
                'prior CRC root checks failed')
        require(root['checks']['actual_crc_old_new_18_checks'] and prior['crc_compatibility']['checks'] == 18,
                'prior actual-reader compatibility incomplete')
        ids = json.loads(bundle.read(prefix + 'programs/identities.json'))
        imported = {}
        for label, role, commit, expected in (('retained', 'baseline', RETAINED, RETAINED_SHA),
                                               ('crc', 'candidate', CRC, CRC_SHA)):
            item = ids[role]
            binary = bundle.read(prefix + 'programs/' + item['binary'])
            evidence_raw = bundle.read(prefix + 'programs/' + item['evidence_file'])
            evidence = json.loads(evidence_raw)
            require(item['environment'] == {} and item['binary_sha256'] == evidence['binary_sha256'] == digest(binary) == expected and
                    item['source_commit'] == evidence['source_commit'] == commit and
                    item['evidence_sha256'] == digest(evidence_raw) and evidence['status'] == 'verified', 'prior identity differs')
            require(evidence['feature_flags'] == FLAGS, 'prior selected features differ')
            if label == 'crc':
                require(root['runtime_commit'] == prior['candidate_source_commit'] == commit and
                        root['image_binary_sha256'] == prior['candidate_binary_sha256'] == expected and
                        root['image_id'] == prior['candidate_image_id'] == evidence['image_id'], 'CRC actual image differs')
                require(digest(bundle.read(prefix + 'programs/root-gates-summary.json')) == evidence['root_gate_summary_sha256'] and
                        digest(bundle.read(prefix + 'programs/root-input-sha256.txt')) == evidence['root_input_manifest_sha256'],
                        'CRC root gate/input proof differs')
                source = evidence['root_input_sha256']
            else:
                require(evidence['compiler_image'] == COMPILER and evidence['harness_matches_git'] is True,
                        'retained historical preparation differs')
                source = evidence['source_and_training_sha256']
            for path, value in source.items():
                require(digest(git(repo, 'show', commit + ':' + path)) == value, 'historical source/Git differs: ' + path)
            target = out / 'programs' / label; target.mkdir(parents=True)
            (target / 'minivec').write_bytes(binary); (target / 'minivec').chmod(0o755)
            (target / 'original-proof.json').write_bytes(evidence_raw)
            proof = {'status': 'verified', 'source_commit': commit, 'binary_sha256': expected,
                'kind': 'imported_historical_pgo_bytes', 'new_pgo_builds': 0,
                'archive_sha256': CRC_ARCHIVE_SHA, 'prior_run_id': 43454350694, 'prior_artifact_id': 14052519019,
                'original_proof_sha256': digest(evidence_raw), 'historical_source_sha256': source,
                'historical_image_id': evidence.get('image_id'), 'official_binary_identity_known': False}
            save(target / 'proof.json', proof); imported[label] = proof
        for path in ('verification-summary.json', prefix + 'summary.json', prefix + 'hashes.json',
                     prefix + 'programs/identities.json', prefix + 'programs/root-gates-summary.json',
                     prefix + 'programs/root-input-sha256.txt', 'crc-checks/compat/summary.json'):
            target = out / 'prior-crc' / path; target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(bundle.read(path))
    return imported


def prepare(repo, out):
    require(not (out / 'prepare-proof.json').exists() and not (out / 'programs').exists() and not (out / 'ee-context').exists(),
            'refuse preparation overwrite')
    revision, inputs = checked_git_inputs(repo)
    manifest, raw, files = ee_source(repo / EE_ARCHIVE)
    for path, value in files.items():
        target = out / 'ee-context' / path; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(value)
    (out / 'ee-source-manifest.json').write_bytes(raw)
    imported = import_crc(repo, out)
    proof = {'status': 'verified', 'harness_commit': revision, 'git_input_sha256': inputs,
        'ee_source_commit': EE, 'ee_source_tree': EE_TREE, 'ee_archive_sha256': EE_ARCHIVE_SHA,
        'ee_source_manifest_sha256': digest(raw), 'ee_source_sha256': {p: digest(v) for p, v in files.items()},
        'crc_archive_sha256': CRC_ARCHIVE_SHA, 'imported': imported,
        'new_pgo_builds': 0, 'new_non_pgo_builds_planned': 1, 'official_binary_identity_known': False}
    save(out / 'prepare-proof.json', proof)
    return proof


def verify_context(out, proof):
    require(sha(out / 'ee-source-manifest.json') == proof['ee_source_manifest_sha256'], 'EE manifest changed')
    actual = {p.relative_to(out / 'ee-context').as_posix(): sha(p) for p in (out / 'ee-context').rglob('*') if p.is_file()}
    require(actual == proof['ee_source_sha256'], 'EE build context changed')


def verify_gates(repo, out):
    expected = {x['id'] for x in json.loads((repo / 'mysqltest/manifest.json').read_bytes())['cases']}
    for name, cases in (('ee-image-handshake', {'handshake'}), ('ee-image-public', expected)):
        data = json.loads((out / name / 'result.json').read_bytes())
        require(data['passed'] is True and len(data['cases']) == len(cases) and
                {x['id'] for x in data['cases']} == cases and all(x['status'] == 'PASS' for x in data['cases']),
                'EE public gate incomplete: ' + name)
    tests = ['typed_rows', 'row_encoding_reuse', 'transactions_and_invalid_input', 'crash_recovery',
             'mode_switch', 'fragmented_jsonl', 'parallel_insert_errors', 'legacy_mode']
    lines = (out / 'ee-json.log').read_text().splitlines()
    require([line for line in lines if line.startswith('PASS ')] == ['PASS ' + x for x in tests], 'EE JSON gate incomplete')
    for name in ('ee-topk.log', 'ee-topk-parallel.log'):
        require((out / name).read_text().strip() == 'PASS JSONL TopK against exact float32 oracle, including ties, filters and rollback',
                'EE TopK gate incomplete')


def bind(repo, out, image):
    proof = json.loads((out / 'prepare-proof.json').read_bytes())
    verify_git_inputs(repo, proof); verify_context(out, proof); verify_gates(repo, out)
    require(re.fullmatch(r'sha256:[0-9a-f]{64}', image) is not None and
            (out / 'ee-image-id.txt').read_text().strip() == image, 'full actual EE image required')
    inspected = json.loads((out / 'ee-image-inspect.json').read_bytes())
    require(len(inspected) == 1 and inspected[0]['Id'] == image and
            inspected[0]['Config']['Entrypoint'] == ['/minivec', '--data', '/data'], 'EE actual image inspect differs')
    binary = out / 'programs/ee/minivec'
    require(binary.is_file() and sha(binary) == (out / 'ee-binary-sha256.txt').read_text().split()[0], 'EE extraction differs')
    ee = {'status': 'verified', 'source_commit': EE, 'source_tree': EE_TREE, 'binary_sha256': sha(binary),
        'image_id': image, 'source_archive_sha256': EE_ARCHIVE_SHA,
        'source_manifest_sha256': proof['ee_source_manifest_sha256'], 'source_sha256': proof['ee_source_sha256'],
        'kind': 'one_original_root_docker_rebuild_without_pgo', 'new_pgo_builds': 0, 'new_non_pgo_builds': 1,
        'prepare_proof_sha256': sha(out / 'prepare-proof.json'), 'official_binary_identity_known': False}
    require(not (binary.parent / 'proof.json').exists(), 'refuse binding overwrite')
    save(binary.parent / 'proof.json', ee)
    def identity(label):
        directory = out / 'programs' / label; item = json.loads((directory / 'proof.json').read_bytes())
        require(sha(directory / 'minivec') == item['binary_sha256'], 'program changed before identity binding')
        return {'binary': '../programs/' + label + '/minivec', 'binary_sha256': item['binary_sha256'],
            'source_commit': item['source_commit'], 'evidence_file': '../programs/' + label + '/proof.json',
            'evidence_sha256': sha(directory / 'proof.json'), 'evidence_binary_pointer': ['binary_sha256'],
            'evidence_commit_pointer': ['source_commit'], 'environment': {}}
    for label in ('retained', 'crc'):
        directory = out / ('ee-vs-' + label); directory.mkdir()
        save(directory / 'identities.json', {'baseline': identity('ee'), 'candidate': identity(label)})
    save(out / 'binding-proof.json', {'status': 'verified', 'ee': ee, 'prepare_proof_sha256': sha(out / 'prepare-proof.json'),
        'comparison_identity_sha256': {label: sha(out / ('ee-vs-' + label) / 'identities.json') for label in ('retained', 'crc')}})
    return ee


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', choices=('prepare', 'bind'), required=True)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--image'); args = p.parse_args()
    result = prepare(args.repo.resolve(), args.output.resolve()) if args.stage == 'prepare' else bind(args.repo.resolve(), args.output.resolve(), args.image)
    print(json.dumps({k: v for k, v in result.items() if k in ('status', 'source_commit', 'harness_commit', 'binary_sha256', 'image_id')}))


if __name__ == '__main__': main()
