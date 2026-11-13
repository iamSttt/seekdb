#!/usr/bin/env python3
"""Bind one actual root candidate to the immutable EE and retained references."""
import argparse
import json
from pathlib import Path
import re
from prepare_small_compute import require, save
from prepare_small_root import prepare_root
from prepare_insert_copy_root import verified_ee, EE, EE_BINARY_SHA, sha
from small_compute import HIGHEST_PRIMARY_FAMILY
EXPERIMENT_ID = 'norm-maxabs-fusion-highest-primary24'
PREREGISTRATION = 'validation/iteration5-norm-maxabs-primary24-preregistration.json'

ROOT_GATES = {
    'root_docker_build', 'ieee_crc_byte_oracle_normal_and_sanitized',
    'insert_owned_copy_normal_and_sanitized', 'image_official_handshake_and_public_cases',
    'source_official_handshake_and_public_cases',
    'live_stdin_errors_utf8_fragment_shutdown_restart_commit_sigkill', 'make_test',
    'native_spill_cache_budget_writer_and_54_utf8_cases', 'actual_image_54_utf8_atomicity_cases',
    'actual_image_jsonl_cursor_frames', 'selected_spill_and_utf8_asan_ubsan',
    'parser_ownership_serial_parallel_and_quantized_bound_asan_ubsan',
    'image_binary_json_and_exact_topk_serial_parallel', 'eight_wal_failure_cases_asan_ubsan',
    'query_pool_both_modes_asan_ubsan', 'cross_table_spill_asan_ubsan',
    'full_binary_json_ubsan', 'index_budget_and_resident_update_asan_ubsan',
    'compound_reuse_diagnostic_and_sanitizer',
    'cached_quantizer_code_equivalence_normal_and_sanitized',
    'global_max_hint_equivalence_normal_and_sanitized'}


def verify_root_gates(reports):
    gates = json.loads((reports / 'root-gates-summary.json').read_bytes())
    require(gates.get('passed') is True and set(gates['checks']) == ROOT_GATES and
            all(x is True for x in gates['checks'].values()), 'root gate inventory failed/incomplete')
    path = reports / 'cached-quantizer-equivalence.json'
    details = json.loads(path.read_bytes())
    require(details.get('schema_version') == 1 and
            gates.get('cached_quantizer_equivalence') == details, 'quantizer JSON/root binding differs')
    hashes = {path.name: sha(path)}
    counts = []
    for mode in ('normal', 'sanitized', 'block-scale'):
        count = details.get(mode + '_checks')
        require(type(count) is int and count > 0 and details.get(mode + '_pass') is True,
                'quantizer result count/pass differs')
        log = reports / ('cached-quantizer-' + mode + '.log')
        text = log.read_text()
        observed = re.findall(r'^cached_quantizer_checks=(\d+)$', text, re.M)
        require(observed == [str(count)] and text.splitlines().count('cached_quantizer=PASS') == 1,
                'quantizer log/JSON differs')
        hashes[log.name] = sha(log); counts.append(count)
    require(counts[0] == counts[1], 'normal/sanitized quantizer count differs')
    historical = {'checks_per_mode': counts[0], 'file_sha256': hashes,
                  'scope': 'rejected cached encoder isolated in test-only header'}
    path = reports / 'global-max-hint-equivalence.json'
    details = json.loads(path.read_bytes())
    require(details.get('schema_version') == 1 and gates.get('global_max_hint_equivalence') == details,
            'norm/max JSON/root binding differs')
    hashes = {path.name: sha(path)}
    counts = []
    for mode in ('normal', 'sanitized', 'block-scale'):
        count = details.get(mode + '_checks')
        require(type(count) is int and count > 0 and details.get(mode + '_pass') is True,
                'norm/max result count/pass differs')
        log = reports / ('global-max-hint-' + mode + '.log')
        text = log.read_text()
        require(re.findall(r'^global_max_hint_checks=(\d+)$', text, re.M) == [str(count)] and
                text.splitlines().count('global_max_hint=PASS') == 1, 'norm/max log/JSON differs')
        hashes[log.name] = sha(log); counts.append(count)
    require(counts[0] == counts[1], 'normal/sanitized norm/max counts differ')
    return {'historical_cached_quantizer': historical,
            'global_max_hint': {'checks_per_mode': counts[0], 'file_sha256': hashes}}


def prepare_highest(repo, binary, image, out):
    source_gates = verify_root_gates(repo / 'protocol-results')
    prereg_path = repo / PREREGISTRATION
    declared = json.loads(prereg_path.read_bytes())
    require(declared.get('experiment_id') == EXPERIMENT_ID and
            declared.get('measurement_family') == HIGHEST_PRIMARY_FAMILY and
            declared.get('planned_pairs', {}).get('total') == 24,
            'norm/max preregistration identity or plan differs')
    preregistration = {'path': PREREGISTRATION, 'bytes': prereg_path.stat().st_size, 'sha256': sha(prereg_path)}
    current = prepare_root(repo, binary, image, out)
    program, proof, evidence = verified_ee(repo)
    directory = out / 'ee'; directory.mkdir()
    (directory / 'minivec').write_bytes(program); (directory / 'minivec').chmod(0o755)
    for name, value in evidence.items(): (directory / name).write_bytes(value)
    save(directory / 'proof.json', proof)
    original = (out / 'identities.json').read_bytes(); ids = json.loads(original)
    require(ids['candidate']['binary_sha256'] == current['binary_sha256'] == sha(binary),
            'actual candidate differs')
    (out / 'retained-identities.json').write_bytes(original)
    highest = {'binary': 'ee/minivec', 'binary_sha256': EE_BINARY_SHA, 'source_commit': EE,
        'evidence_file': 'ee/proof.json', 'evidence_sha256': sha(directory / 'proof.json'),
        'evidence_binary_pointer': ['binary_sha256'], 'evidence_commit_pointer': ['source_commit'],
        'environment': {}}
    save(out / 'ee-identities.json', {'baseline': highest, 'candidate': ids['candidate']})
    save(out / 'highest-primary-binding-proof.json', {'status': 'verified',
        'measurement_family': HIGHEST_PRIMARY_FAMILY, 'primary_reference': 'ee',
        'experiment_id': EXPERIMENT_ID, 'preregistration': preregistration,
        'candidate_source_commit': current['source_commit'], 'candidate_binary_sha256': current['binary_sha256'],
        'candidate_image_id': image, 'current_root_proof_sha256': sha(out / 'root-proof.json'),
        'ee_import_proof_sha256': sha(directory / 'proof.json'),
        'retained_identity_sha256': sha(out / 'retained-identities.json'),
        'ee_identity_sha256': sha(out / 'ee-identities.json'), 'source_equivalence_gates': source_gates,
        'new_ee_builds': 0, 'new_pgo_builds': 0,
        'build_count_scope': 'This import step only; the candidate root PGO build precedes import.'})
    return current, proof


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'binary', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--image', required=True); args = parser.parse_args()
    current, historical = prepare_highest(args.repo.resolve(), args.binary.resolve(), args.image, args.output.resolve())
    print(json.dumps({'status': 'verified', 'measurement_family': HIGHEST_PRIMARY_FAMILY,
        'candidate_binary_sha256': current['binary_sha256'], 'ee_binary_sha256': historical['binary_sha256'],
        'new_reference_builds': 0}))


if __name__ == '__main__': main()
