#!/usr/bin/env python3
"""Record a CI runner instance so a later confirmation can verify a fresh boot."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys


def capture():
    if not sys.platform.startswith('linux'):
        raise ValueError('runner evidence requires the actual Linux CI host')
    result = {key: os.environ.get(key, '') for key in
              ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB', 'RUNNER_NAME', 'RUNNER_OS')}
    result['boot_id'] = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    result['captured_utc'] = datetime.now(timezone.utc).isoformat()
    validate(result)
    return result


def validate(data):
    for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT'):
        if not re.fullmatch('[1-9][0-9]*', data.get(key, '')):
            raise ValueError('missing/invalid runner field: ' + key)
    for key in ('GITHUB_JOB', 'RUNNER_NAME'):
        if not isinstance(data.get(key), str) or not data[key]:
            raise ValueError('missing runner field: ' + key)
    if data.get('RUNNER_OS') != 'Linux':
        raise ValueError('expected Linux runner')
    if not re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', data.get('boot_id', '')):
        raise ValueError('missing/invalid kernel boot identity')


def require_fresh(initial, current):
    validate(initial)
    validate(current)
    if current['GITHUB_RUN_ID'] == initial['GITHUB_RUN_ID']:
        raise ValueError('confirmation must use a different CI run')
    if current['boot_id'] == initial['boot_id']:
        raise ValueError('confirmation must use a different runner boot')
    if current['GITHUB_RUN_ATTEMPT'] != '1':
        raise ValueError('confirmation rerun attempts are not part of this frozen plan')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['capture'])
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('refuse to overwrite runner evidence')
    data = capture()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2) + '\n', encoding='utf8')
    print(json.dumps(data, sort_keys=True))
