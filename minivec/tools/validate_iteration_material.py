#!/usr/bin/env python3
"""Read-only checks for this repository's attributed material, not an official grader.

The organizer permits arbitrary business fields. The additional message, quote,
table and artifact checks below enforce our documented local evidence schema.
No code path generates, updates or overwrites material.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re


DIMENSIONS = ('问题理解与自主拆解', '技术理解与迁移', '独立验证与质疑',
              '实验与反馈迭代', '取舍与反思')


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


def resolve_pointer(record, pointer):
    """Resolve RFC 6901 without accepting invalid escapes or array indices."""
    require(isinstance(pointer, str), 'pointer must be text')
    if pointer == '':
        return record
    require(pointer.startswith('/'), 'pointer must start with /')
    current = record
    for token in pointer[1:].split('/'):
        require(re.search(r'~(?![01])', token) is None, 'invalid pointer escape')
        token = token.replace('~1', '/').replace('~0', '~')
        if isinstance(current, list):
            require(re.fullmatch(r'0|[1-9][0-9]*', token) is not None, 'invalid array pointer index')
            index = int(token)
            require(index < len(current), 'array pointer out of range')
            current = current[index]
        else:
            require(isinstance(current, dict) and token in current, 'unresolved pointer')
            current = current[token]
    return current


def validate_quote(record, by_id, reference, pointer_key='pointer', message_key='message_id', required_role='user'):
    quote = reference['quote']
    require(isinstance(quote, str) and 0 < len(quote) <= 240, 'quote must contain 1..240 characters')
    pointer = reference[pointer_key]
    text = resolve_pointer(record, pointer)
    require(isinstance(text, str) and quote in text, 'quote is not an exact substring')
    if message_key in reference:
        message = by_id[reference[message_key]]
        require(pointer.endswith('/content'), 'message quote must point to content')
        require(resolve_pointer(record, pointer[:-len('/content')]) is message,
                'quote pointer does not belong to referenced message')
        if required_role is not None:
            require(message['role'] == required_role, 'student/source quote must retain user role')


def check_artifact(root, artifact, path_key='path'):
    name = artifact[path_key]
    digest = artifact['sha256']
    require(isinstance(name, str) and name, 'empty artifact path')
    require(isinstance(digest, str) and re.fullmatch('[0-9a-f]{64}', digest), 'invalid artifact hash')
    require(not Path(name).is_absolute(), 'artifact path must be repository relative')
    path = (root / name).resolve()
    require(path.is_relative_to(root), 'artifact path escapes repository')
    require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'artifact hash mismatch')


def check_structure(record, value):
    """Validate declared pointers and lossless columns/rows shape recursively."""
    if isinstance(value, dict):
        require('score_hint' not in value, 'legacy score_hint is not permitted by the local schema')
        if 'quote' in value:
            pointer_key = 'source_pointer' if 'source_pointer' in value else 'pointer'
            require(pointer_key in value, 'quote requires a pointer')
            quote = value['quote']
            target = resolve_pointer(record, value[pointer_key])
            require(isinstance(quote, str) and 0 < len(quote) <= 240,
                    'quote must contain 1..240 characters')
            require(isinstance(target, str) and quote in target, 'quote is not an exact substring')
        for key, child in value.items():
            if key == 'pointer' or key.endswith('_pointer'):
                resolve_pointer(record, child)
            check_structure(record, child)
        if 'columns' in value and isinstance(value.get('rows'), dict):
            columns = value['columns']
            require(isinstance(columns, list) and all(isinstance(k, str) for k in columns),
                    'table columns must be text')
            require(len(columns) == len(set(columns)), 'duplicate table columns')
            require(all(isinstance(row, list) and len(row) == len(columns)
                        for row in value['rows'].values()), 'table row/column mismatch')
    elif isinstance(value, list):
        for child in value:
            check_structure(record, child)


def validate(root):
    root = Path(root).resolve()
    data = (root / 'agent/conversation.json').read_bytes()
    require(data == (root / 'conversation.json').read_bytes(), 'material copies differ')
    require(len(data) <= 131072, 'material exceeds 128 KiB limit')
    record = json.loads(data.decode('utf-8'), object_pairs_hook=unique_object,
                        parse_float=finite_float,
                        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    require(isinstance(record, dict), 'material must be a JSON object')
    require(record['base_commit'] == 'a90b2b750e40f020793cddefe01d5b66631afc63', 'wrong baseline')
    messages = record['messages']
    require(isinstance(messages, list) and messages, 'messages must be a nonempty array')
    by_id = {entry['id']: entry for entry in messages}
    require(len(by_id) == len(messages), 'duplicate message IDs')
    for entry in messages:
        require(isinstance(entry['id'], str) and entry['id'], 'invalid message ID')
        require(entry['role'] in ('user', 'assistant'), 'invalid role')
        require(isinstance(entry['content'], str) and entry['content'], 'empty message')
        if 'sha256' in entry:
            require(hashlib.sha256(entry['content'].encode('utf-8')).hexdigest() == entry['sha256'],
                    'message content hash mismatch')
    user_hashes = record['provenance']['user_content_sha256']
    require(user_hashes == {entry['id']: hashlib.sha256(entry['content'].encode('utf-8')).hexdigest()
                           for entry in messages if entry['role'] == 'user'},
            'user preservation manifest mismatch')
    for direction in record['adopted_directions'] + record.get('direction_status', []):
        require(by_id[direction['source_message']]['role'] == direction['origin_role'] == 'user', 'direction source must be user')
        require(direction['implementation_role'] == 'assistant', 'implementation must retain assistant attribution')
        validate_quote(record, by_id, direction, 'source_pointer', 'source_message')
        if 'prediction_message' in direction:
            require(by_id[direction['prediction_message']]['role'] == 'user', 'prediction must retain user role')
    experiments = record['experiments']
    require(len({entry['id'] for entry in experiments}) == len(experiments), 'duplicate experiment IDs')
    for experiment in experiments:
        require(experiment['executed_by'] == 'assistant', 'experiment executor must retain assistant attribution')
        for artifact in experiment.get('artifacts', []):
            check_artifact(root, artifact)
        schema_reference = experiment.get('observed_result', {}).get('schema_reference')
        if schema_reference:
            check_artifact(root, schema_reference, 'file')
    units = record.get('evidence_units', [])
    require(len({unit['id'] for unit in units}) == len(units), 'duplicate evidence unit')
    for unit in units:
        require(unit['dimension'] in DIMENSIONS, 'unknown evidence dimension')
        require('score' not in unit, 'evidence index must not self-score')
        require('message_id' in unit['student_evidence'], 'student evidence must identify a user message')
        validate_quote(record, by_id, unit['student_evidence'])
        if unit.get('result_evidence') is not None:
            validate_quote(record, by_id, unit['result_evidence'], required_role=None)
    check_structure(record, record)
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'messages': len(messages), 'experiments': len(record['experiments']),
            'remaining_bytes': 131072 - len(data), 'read_only': True,
            'scope': 'mechanical checks only; no official score or authorship judgment'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2],
                        help='repository root to read; no files are written')
    print(json.dumps(validate(parser.parse_args().root), ensure_ascii=False))
