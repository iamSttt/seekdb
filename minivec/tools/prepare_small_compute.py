#!/usr/bin/env python3
"""Import the previously audited submitted-source PGO program, without rebuilding."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

COMMIT = 'e020fdeb67dd8b64b77f0f8c2905f137f0112bba'
ARCHIVE_SHA = '88e4a73af9d333b08a6db817a78c7cb06357fcfddc05eee1b31c2a065aebfef2'
ARCHIVE_BYTES = 49685962
BINARY_SHA = 'acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
FLAGS = ['-DMVDB_ENABLE_SPILL_FILTER', '-DMVDB_ENABLE_TYPED_JSON',
         '-DMVDB_TYPED_WIDE_VECTOR_ONLY', '-DMVDB_MULTI_POSTING_REUSE_ONLY']
COMPILER = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'


def require(ok, message):
    if not ok: raise ValueError(message)


def digest(data): return hashlib.sha256(data).hexdigest()


def safe_names(bundle):
    items = bundle.infolist()
    require(len(items) <= 10000 and sum(i.file_size for i in items) <= 1024**3, 'archive exceeds bounds')
    seen = set()
    for item in items:
        name = item.filename; path = PurePosixPath(name)
        require(name and name not in seen and not item.is_dir(), 'empty, duplicate or directory member')
        require(not path.is_absolute() and path.as_posix() == name and
                not any(part in ('.', '..') for part in path.parts) and
                not any(c in name for c in ('\\', ':', '\0')), 'unsafe archive path')
        require((item.external_attr >> 16) & 0o170000 != 0o120000, 'archive symlink forbidden')
        seen.add(name)
    return seen


def git(repo, *args):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + repo.as_posix(),
                                   '-C', str(repo), *args])


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf8', newline='\n')


def prepare(repo, archive, out, candidate_policy='serial'):
    policies = {'serial': {'MVDB_QUERY_THREADS': '1'},
                'no-fanout': {'MVDB_QUERY_FANOUT_ROWS': '9223372036854775807'}}
    require(candidate_policy in policies, 'unknown query policy')
    require(not out.exists(), 'refuse overwrite')
    require(archive.stat().st_size == ARCHIVE_BYTES and digest(archive.read_bytes()) == ARCHIVE_SHA,
            'retained artifact bytes differ')
    with zipfile.ZipFile(archive) as bundle:
        names = safe_names(bundle)
        manifest_raw = bundle.read('hashes.json')
        manifest = json.loads(manifest_raw)['artifacts']
        entries = {item['path']: item for item in manifest}
        require(len(entries) == len(manifest) and set(entries) == names - {'hashes.json'}, 'manifest inventory differs')

        def read(name):
            value = bundle.read(name); item = entries[name]
            require(len(value) == item['bytes'] and digest(value) == item['sha256'], 'member hash differs: ' + name)
            return value

        context_raw = read('context.json'); context = json.loads(context_raw)
        proof = context['profile_source_proof']['baseline']
        require(context['baseline_commit'] == proof['source_commit'] == COMMIT, 'baseline source differs')
        require(context['compiler_image'] == COMPILER and proof['feature_flags'] == FLAGS and
                proof['root_feature_flags_override'] == '', 'compiler or feature policy differs')
        require(proof['binary_sha256'] == BINARY_SHA, 'declared binary differs')
        program = read('baseline-pgo/minivec')
        require(digest(program) == BINARY_SHA, 'actual binary differs')
        paths = git(repo, 'ls-tree', '-r', '--name-only', COMMIT, '--', 'minivec/src').decode().splitlines()
        require(paths and {p.removeprefix('minivec/src/') for p in paths} == set(proof['source_sha256']),
                'complete engine source inventory differs')
        require({n.removeprefix('baseline-pgo/built-source/') for n in names if n.startswith('baseline-pgo/built-source/')} ==
                set(proof['source_sha256']), 'built engine inventory differs')
        source_hashes = {}
        for path in paths:
            short = path.removeprefix('minivec/src/')
            original = git(repo, 'show', COMMIT + ':' + path)
            require(read('baseline-pgo/built-source/' + short) == original and
                    digest(original) == proof['source_sha256'][short], 'engine Git/source mismatch: ' + path)
            source_hashes[path] = digest(original)
        for name in ('train.cpp', 'run-training.sh'):
            path = 'minivec/tools/pgo/' + name
            original = git(repo, 'show', COMMIT + ':' + path)
            require(read('baseline-pgo/training/' + name) == original and
                    digest(original) == proof['training_source_sha256'][name], 'training Git/source mismatch')
            source_hashes[path] = digest(original)
        require(all(item['status'] == 'passed' for item in proof['training_oracle']) and
                len(proof['training_oracle']) == 6, 'historical training evidence incomplete')

    # No archive member is executed or extracted by its own pathname. Only this
    # individually verified executable gets executable permission.
    out.mkdir(parents=True)
    target = out / 'submitted-source-minivec'; target.write_bytes(program); target.chmod(0o755)
    (out / 'prior-context.json').write_bytes(context_raw)
    revision = git(repo, 'rev-parse', 'HEAD').decode().strip()
    tool_paths = ('minivec/tools/small_compute.py', 'minivec/tools/prepare_small_compute.py',
        'minivec/tools/ci_small_compute.sh', 'minivec/tools/cgroup_exec.c',
        'minivec/tools/ci_small_root.sh', 'minivec/tools/prepare_small_root.py',
        'minivec/tools/summarize_small_root.py', 'minivec/tools/ci_score.sh',
        'minivec/tools/ci_insert_copy_root.sh', 'minivec/tools/prepare_insert_copy_root.py',
        'minivec/tools/summarize_insert_copy_root.py',
        'minivec/tools/ci_highest_primary.sh', 'minivec/tools/prepare_highest_primary.py',
        'minivec/tools/summarize_highest_primary.py', '.github/workflows/minivec-score.yml',
        'validation/iteration5-highest-primary24-preregistration.json',
        'validation/iteration5-norm-maxabs-primary24-preregistration.json',
        'minivec/tools/prepare_highest_compare.py',
        'validation/iteration5-ee299-root-source.zip', 'validation/iteration5-highest-root-raw.zip',
        'minivec/tools/ci_crc_checks.sh', 'minivec/tests/check_crc_persistence.py',
        'minivec/tests/crc_persistence_fixture.cpp',
        'minivec/tools/benchmark_iteration.py', 'minivec/tools/benchmark_index_delta.py',
        'minivec/tools/benchmark_write_query.py', 'minivec/tools/benchmark_projection.py',
        'minivec/tests/json_protocol.py', 'protocol_check/_transport.py', 'protocol_check/config.json')
    tools = {path: digest((repo / path).read_bytes()) for path in tool_paths}
    # New harness files may be uncommitted for a read-only local import check.
    # CI binds all of them to Git before running any database.
    harness_matches_git = all(git(repo, 'show', revision + ':' + path) == (repo / path).read_bytes()
                              for path in tool_paths) if (repo / '.git').exists() and all(
        subprocess.run(['git', '-c', 'safe.directory=' + repo.as_posix(), '-C', str(repo),
                        'cat-file', '-e', revision + ':' + path], capture_output=True).returncode == 0
        for path in tool_paths) else False
    evidence = {'status': 'verified', 'source_commit': COMMIT, 'binary_sha256': BINARY_SHA,
        'feature_flags': FLAGS, 'compiler_image': COMPILER, 'source_and_training_sha256': source_hashes,
        'retained_archive_sha256': ARCHIVE_SHA, 'retained_archive_bytes': ARCHIVE_BYTES,
        'retained_context_sha256': digest(context_raw), 'retained_manifest_sha256': digest(manifest_raw),
        'retained_run_id': 43419207297, 'retained_artifact_id': 14034362636,
        'harness_commit': revision, 'harness_file_sha256': tools, 'harness_matches_git': harness_matches_git,
        'scope': 'Reuses previously audited immutable PGO bytes. This import verifies archive, selected member hashes, complete engine/training source and Git identity; it does not repeat PGO training or its historical performance tests.',
        'official_binary_sha256': None, 'official_image_identity_proven': False,
        'candidate_policy': candidate_policy, 'new_pgo_builds': 0, 'new_database_tests': 0}
    save(out / 'provenance.json', evidence)
    item = {'binary': target.name, 'binary_sha256': BINARY_SHA, 'source_commit': COMMIT,
        'evidence_file': 'provenance.json', 'evidence_sha256': digest((out / 'provenance.json').read_bytes()),
        'evidence_binary_pointer': ['binary_sha256'], 'evidence_commit_pointer': ['source_commit']}
    save(out / 'identities.json', {'baseline': dict(item, environment={}),
                                  'candidate': dict(item, environment=policies[candidate_policy])})
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'archive', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--allow-uncommitted-harness', action='store_true',
                        help='local import check only; does not run a database')
    parser.add_argument('--candidate-policy', choices=['serial', 'no-fanout'], default='serial')
    args = parser.parse_args()
    evidence = prepare(args.repo.resolve(), args.archive.resolve(), args.output.resolve(), args.candidate_policy)
    require(args.allow_uncommitted_harness or evidence['harness_matches_git'], 'measurement harness differs from Git')
    print(json.dumps({'status': 'verified', 'source_commit': COMMIT, 'binary_sha256': BINARY_SHA,
                      'harness_matches_git': evidence['harness_matches_git'], 'new_database_tests': 0}))


if __name__ == '__main__': main()
