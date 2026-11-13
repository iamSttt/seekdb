#!/usr/bin/env python3
"""Bind one actual root candidate to the immutable EE and retained references."""
import argparse
import json
from pathlib import Path
import re
import sys
from prepare_small_compute import require, save, FLAGS, git
from prepare_small_root import prepare_root
from prepare_insert_copy_root import verified_ee, EE, EE_BINARY_SHA, sha
from small_compute import HIGHEST_PRIMARY_FAMILY

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
    'batch8_bound_normal_and_sanitized', 'actual_image_batch8_dispatch_and_oracle',
    'tighter_certificate_normal_and_sanitized', 'actual_image_tighter_certificate_oracle'}


BATCH8_FLAGS = FLAGS + ['-DMVDB_ENABLE_BATCH8_QUANTIZED']
BATCH8_POLICY_PATH = 'validation/iteration5-batch8-clear-gain-policy.json'
BATCH8_POLICY_SHA256 = 'adda2853849fbc6ac2697047d890665b3484e3028f2f1dfbff114cae8254ecde'
BATCH8_POLICY_BYTES = 2661


def verify_batch8_policy(repo):
    path=repo/BATCH8_POLICY_PATH
    raw=path.read_bytes()
    require(len(raw)==BATCH8_POLICY_BYTES and sha(path)==BATCH8_POLICY_SHA256,
            'frozen clear-gain policy bytes changed')
    revision=git(repo,'rev-parse','HEAD').decode().strip()
    require(git(repo,'show',revision+':'+BATCH8_POLICY_PATH)==raw,
            'clear-gain policy is not the committed exact file')
    policy=json.loads(raw)
    require(policy.get('official_submission_by_assistant') is False and
            policy['highest_formal_reference_commit']==EE,
            'clear-gain policy reference or submission scope differs')
    return {'path':BATCH8_POLICY_PATH,'sha256':BATCH8_POLICY_SHA256,'bytes':BATCH8_POLICY_BYTES}


BATCH8_REQUIRED_INPUTS = {
    BATCH8_POLICY_PATH,
    'minivec/tools/verify_tighter_root.py', 'minivec/tests/test_tighter_certificate.cpp',
    'minivec/tests/test_tighter_certificate_db.py', 'minivec/tools/tighter_bound/make_fixtures.py',
    'minivec/tools/tighter_bound/reference_distance.h',
    'minivec/src/quantized_batch8.h', 'minivec/tests/test_batch8_bound.cpp',
    'minivec/tests/test_batch8_db.py', 'minivec/tools/batch8_native_isa_probe.cpp',
    'minivec/tools/prepare_highest_primary.py', 'minivec/tools/summarize_highest_primary.py',
    'minivec/tools/ci_highest_primary.sh'}


def verify_batch8_evidence(reports, binary_sha):
    sys.path.insert(0,str(reports.parent/'minivec/tests'))
    from test_batch8_db import EXPECTED_CASES
    path=reports/'image-binary-batch8.json'
    report=json.loads(path.read_bytes())
    checks=report.get('checks',[])
    require(report.get('status')=='passed' and report.get('binary_sha256')==binary_sha and
            report.get('trace_schema')=='MVDB_BATCH8_TRACE/v1' and
            len(checks)==len(EXPECTED_CASES)==21 and
            {c.get('name') for c in checks}==set(EXPECTED_CASES) and
            all(c.get('status')=='passed' for c in checks), 'actual-image batch8 oracle/dispatch evidence incomplete')
    hashes={path.name:sha(path)};counts=[]
    for mode in ('normal','sanitized'):
        log=reports/('batch8-bound-'+mode+'.log');text=log.read_text()
        n=re.findall(r'^batch8_bound_checks=(\d+)$',text,re.M)
        require(len(n)==1 and int(n[0])>0 and text.splitlines().count('batch8_bound=PASS')==1,
                'batch8 bound gate incomplete')
        counts.append(int(n[0]));hashes[log.name]=sha(log)
    require(counts[0]==counts[1],'batch8 normal/sanitized counts differ')
    environment=json.loads((reports/'batch8-environment.json').read_bytes())
    require(environment.get('status')=='supported_native_avx2' and environment.get('native_row_isa')=='avx2' and
            environment.get('avx2') is True and environment.get('avx512') is False and
            environment.get('database_runs')==environment.get('pgo_builds')==environment.get('performance_pairs')==0,
            'batch8 environment is not the preregistered native AVX2 target')
    hashes['batch8-environment.json']=sha(reports/'batch8-environment.json')
    return {'schema_version':1,'checks_per_mode':counts[0],'actual_image_checks':21,
            'binary_sha256':binary_sha,'native_row_isa':'avx2','batch_isa':'avx2','file_sha256':hashes}


def verify_root_gates(reports):
    gates = json.loads((reports / 'root-gates-summary.json').read_bytes())
    require(gates.get('passed') is True and set(gates['checks']) == ROOT_GATES and
            all(x is True for x in gates['checks'].values()), 'root gate inventory failed/incomplete')
    from verify_tighter_root import verify_tighter_evidence
    tighter=verify_tighter_evidence(reports,gates['image_binary_sha256'])
    require(gates.get('tighter_certificate_evidence')==tighter,'tighter certificate root binding differs')
    batch8=verify_batch8_evidence(reports,gates['image_binary_sha256'])
    require(gates.get('batch8_evidence')==batch8,'batch8 gate/root summary binding differs')
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
    return {'checks_per_mode': counts[0], 'file_sha256': hashes, 'batch8':batch8, 'tighter_certificate':tighter}


def prepare_highest(repo, binary, image, out):
    policy=verify_batch8_policy(repo)
    quantizer = verify_root_gates(repo / 'protocol-results')
    current = prepare_root(repo, binary, image, out, expected_flags=BATCH8_FLAGS, required_inputs=BATCH8_REQUIRED_INPUTS)
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
        'experiment_id':'batch8-tighter-certificate-highest-primary24',
        'acceptance_policy':policy,
        'measurement_family': HIGHEST_PRIMARY_FAMILY, 'primary_reference': 'ee',
        'candidate_source_commit': current['source_commit'], 'candidate_binary_sha256': current['binary_sha256'],
        'candidate_image_id': image, 'current_root_proof_sha256': sha(out / 'root-proof.json'),
        'ee_import_proof_sha256': sha(directory / 'proof.json'),
        'retained_identity_sha256': sha(out / 'retained-identities.json'),
        'ee_identity_sha256': sha(out / 'ee-identities.json'), 'quantizer_gate': quantizer,
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
