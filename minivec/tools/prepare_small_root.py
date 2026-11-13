#!/usr/bin/env python3
"""Bind one already-built root-image executable to the retained program."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
from prepare_small_compute import prepare, digest, git, FLAGS, save, require


def prepare_root(repo, binary, image, out, *, expected_flags=None, required_inputs=()):
    require(not out.exists(), 'refuse overwrite')
    require(re.fullmatch('sha256:[0-9a-f]{64}', image) is not None, 'full image ID required')
    reports = repo / 'protocol-results'
    raw = (reports / 'root-gates-summary.json').read_bytes()
    root = json.loads(raw)
    revision = git(repo, 'rev-parse', 'HEAD').decode().strip()
    program = binary.read_bytes(); program_sha = digest(program)
    require(root.get('passed') is True and len(root['checks']) >= 16 and
            all(value is True for value in root['checks'].values()), 'root gates not all passed')
    require(root['runtime_commit'] == revision, 'root gate commit differs from checkout')
    selected = FLAGS if expected_flags is None else list(expected_flags)
    require(selected in (FLAGS, FLAGS + ['-DMVDB_ENABLE_BATCH8_QUANTIZED']), 'unsupported explicit root feature policy')
    require(root['selected_feature_flags'] == selected, 'root features differ from explicit candidate policy')
    require(root['image_id'] == image == (reports / 'image-id.txt').read_text().strip(), 'root image mismatch')
    require(root['image_binary_sha256'] == program_sha ==
            (reports / 'image-binary-sha256.txt').read_text().split()[0], 'actual root binary mismatch')

    input_raw = (reports / 'input-sha256.txt').read_bytes()
    inputs = {}
    for line in input_raw.decode().splitlines():
        expected, name = line.split(maxsplit=1); name = name.lstrip('*')
        require(re.fullmatch('[0-9a-f]{64}', expected) is not None, 'invalid input hash')
        path = PurePosixPath(name)
        require(name not in inputs and not path.is_absolute() and path.as_posix() == name and
                not any(p in ('.', '..') for p in path.parts) and '\\' not in name, 'unsafe/duplicate input path')
        require(digest((repo / name).read_bytes()) == expected and
                digest(git(repo, 'show', revision + ':' + name)) == expected, 'root input differs from Git: ' + name)
        inputs[name] = expected
    expected_paths = git(repo, 'ls-files', '-z', 'Dockerfile', '.dockerignore',
        'validation/iteration5-batch8-clear-gain-policy.json', 'minivec/src',
        'minivec/third_party', 'minivec/Makefile', 'minivec/tests', 'minivec/tools/ci_score.sh',
        'minivec/tools/batch8_native_isa_probe.cpp', 'minivec/tools/prepare_highest_primary.py',
        'minivec/tools/summarize_highest_primary.py', 'minivec/tools/ci_highest_primary.sh',
        'minivec/tools/ci_final_performance.sh', 'minivec/tools/pgo/train.cpp',
        'minivec/tools/pgo/run-training.sh', 'STDIO_PROTOCOL.md', 'protocol_check', 'mysqltest').decode().split('\0')
    require(set(inputs) == set(filter(None, expected_paths)), 'root input inventory incomplete')
    require(set(required_inputs) <= set(inputs), 'new candidate header/tests/tools missing from Git-bound input inventory')

    out.mkdir(parents=True)
    retained = prepare(repo, repo / 'validation/iteration4-bulk-loop-raw.zip', out / 'retained')
    require(retained['harness_matches_git'], 'measurement harness differs from Git')
    target = out / 'actual-image-minivec'; target.write_bytes(program); target.chmod(0o755)
    (out / 'root-gates-summary.json').write_bytes(raw)
    (out / 'root-input-sha256.txt').write_bytes(input_raw)
    proof = {'status': 'verified', 'source_commit': revision, 'binary_sha256': program_sha,
        'image_id': image, 'feature_flags': root['selected_feature_flags'],
        'root_gate_summary_sha256': digest(raw), 'root_input_manifest_sha256': digest(input_raw),
        'root_input_sha256': inputs, 'harness_file_sha256': retained['harness_file_sha256'],
        'harness_matches_git': True, 'official_binary_identity_known': False,
        'scope': 'One current root Docker build, all prior gates passed, actual extracted executable and complete Git inputs verified; performance has not run yet.'}
    save(out / 'root-proof.json', proof)
    baseline = json.loads((out / 'retained/identities.json').read_bytes())['baseline']
    baseline.update(binary='retained/' + baseline['binary'],
                    evidence_file='retained/' + baseline['evidence_file'])
    candidate = {'binary': target.name, 'binary_sha256': program_sha, 'source_commit': revision,
        'evidence_file': 'root-proof.json', 'evidence_sha256': digest((out / 'root-proof.json').read_bytes()),
        'evidence_binary_pointer': ['binary_sha256'], 'evidence_commit_pointer': ['source_commit'], 'environment': {}}
    save(out / 'identities.json', {'baseline': baseline, 'candidate': candidate})
    return proof


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'binary', 'output'): p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--image', required=True)
    args = p.parse_args()
    proof = prepare_root(args.repo.resolve(), args.binary.resolve(), args.image, args.output.resolve())
    print(json.dumps({key: proof[key] for key in ('status', 'source_commit', 'image_id', 'binary_sha256')}))


if __name__ == '__main__': main()
