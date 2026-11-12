#!/usr/bin/env python3
"""Mechanical provenance/size check; does not grade or authenticate authorship."""
import hashlib
import json
import math
from pathlib import Path


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite_float(value):
    result = float(value)
    require(math.isfinite(result), 'non-finite JSON number')
    return result


def validate(root):
    data = (root / 'agent/conversation.json').read_bytes()
    require(data == (root / 'conversation.json').read_bytes(), 'material copies differ')
    require(len(data) <= 131072, 'material exceeds 128 KiB limit')
    record = json.loads(data.decode('utf-8'), object_pairs_hook=unique_object,
                        parse_float=finite_float,
                        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    require(record['base_commit'] == 'a90b2b750e40f020793cddefe01d5b66631afc63', 'wrong baseline')
    messages = record['messages']
    by_id = {entry['id']: entry for entry in messages}
    require(len(by_id) == len(messages), 'duplicate message IDs')
    for entry in messages:
        require(entry['role'] in ('user', 'assistant'), 'invalid role')
        require(isinstance(entry['content'], str) and entry['content'], 'empty message')
    for direction in record['adopted_directions']:
        require(by_id[direction['source_message']]['role'] == direction['origin_role'] == 'user', 'direction source must be user')
        require(direction['implementation_role'] == 'assistant', 'implementation must retain assistant attribution')
    for experiment in record['experiments']:
        require(experiment['executed_by'] == 'assistant', 'experiment executor must retain assistant attribution')
        for artifact in experiment.get('artifacts', []):
            path = (root / artifact['path']).resolve()
            require(path.is_relative_to(root.resolve()), 'artifact path escapes repository')
            require(hashlib.sha256(path.read_bytes()).hexdigest() == artifact['sha256'], 'artifact hash mismatch')
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'messages': len(messages), 'experiments': len(record['experiments']),
            'scope': 'mechanical checks only; no official score or authorship judgment'}


if __name__ == '__main__':
    print(json.dumps(validate(Path(__file__).resolve().parents[2]), ensure_ascii=False))
