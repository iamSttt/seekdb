#!/usr/bin/env python3
"""Read-only identity checks for the unmeasured PGO-only candidate."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
manifest = json.loads((ROOT / 'validation/pgo-only-source-20261114.json').read_bytes())
git = ['git', '-c', 'safe.directory=' + ROOT.as_posix(), '-C', str(ROOT)]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def baseline(path):
    return subprocess.run(git + ['show', manifest['baseline_commit'] + ':' + path],
                          check=True, capture_output=True).stdout


require(manifest['runtime_source_count'] == 16, 'unexpected runtime inventory')
expected_paths = set(manifest['runtime_and_third_party'])
actual_paths = {p.relative_to(ROOT).as_posix()
                for directory in ('minivec/src', 'minivec/third_party')
                for p in (ROOT / directory).rglob('*') if p.is_file()}
require(actual_paths == expected_paths, 'runtime/third-party files added or removed')
for path, identity in manifest['runtime_and_third_party'].items():
    raw = (ROOT / path).read_bytes()
    require(raw == baseline(path), 'runtime is not byte-identical to 760f: ' + path)
    require(digest(raw) == identity['sha256'] and len(raw) == identity['bytes'],
            'runtime manifest mismatch: ' + path)
for group in ('training_inputs', 'build_inputs'):
    for path, identity in manifest[group].items():
        raw = (ROOT / path).read_bytes()
        require(digest(raw) == identity['sha256'] and len(raw) == identity['bytes'],
                'frozen ' + group + ' changed: ' + path)
docker = (ROOT / 'Dockerfile').read_text(encoding='utf8')
require('FEATURE_FLAGS' not in docker and '-DMVDB_' not in docker,
        'R feature macros are not part of this experiment')
flags = '-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc'
require(flags in docker and flags in baseline('Dockerfile').decode(),
        'non-PGO compiler flags changed')
require('-fprofile-correction' not in docker and '-flto' not in docker,
        'unplanned build factor')
require('-Werror=missing-profile -Werror=coverage-mismatch' in docker,
        'strict feedback checks missing')
release = docker.split('FROM scratch AS release\n', 1)[1]
require('training' not in release and '/pgo' not in release,
        'training/profile files may not enter release image')
expected_ignore = baseline('.dockerignore').rstrip(b'\r\n') + (
    b'\n!minivec/tools/\n!minivec/tools/pgo/\n'
    b'!minivec/tools/pgo/train.cpp\n!minivec/tools/pgo/run-training.sh\n')
require((ROOT / '.dockerignore').read_bytes() == expected_ignore,
        'Docker context allowlist changed beyond two training inputs')
adapter_path = ROOT / 'validation/pgo-only-adapter-20261114.json'
require(adapter_path.is_file(), 'execution adapter must be frozen before native runs')
adapter = json.loads(adapter_path.read_bytes())
for path, expected in adapter['sha256'].items():
    require(digest((ROOT / path).read_bytes()) == expected, 'frozen adapter/policy changed: ' + path)
print(json.dumps({'status': 'static_identity_passed', 'runtime_files': 16,
                  'third_party_files': len(expected_paths) - 16,
                  'native_builds': 0, 'performance_pairs': 0,
                  'official_score': None, 'automatic_adoption': False}))
