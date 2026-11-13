#!/usr/bin/env python3
"""Bind the new root program to retained PGO and the archived original-EE rebuild."""
import argparse
import json
from pathlib import Path
import zipfile
from prepare_small_compute import digest, git, require, safe_names, save
from prepare_small_root import prepare_root
from prepare_highest_compare import EE, EE_TREE, EE_ARCHIVE, EE_ARCHIVE_SHA, MEASURED_TOOLS, ee_source
from small_compute import CASES, PROTECTION_CASES
from summarize_small_root import verify_measurement

ARCHIVE = 'validation/iteration5-highest-root-raw.zip'
ARCHIVE_SHA = 'cb6a5801cf2195cfcb0aafe7067143c825c8420974ead2c9e81ab6e6c2deb599'
ARCHIVE_BYTES = 3879395
HARNESS_COMMIT = '95afba3993cb21cdce32acf2502312262b8ae9ac'
EE_BINARY_SHA = '9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd'
MISSING_SOURCE = 'ee-context/.dockerignore'


def sha(path):
    return digest(path.read_bytes())


def verified_ee(repo):
    archive = repo / ARCHIVE
    require(archive.stat().st_size == ARCHIVE_BYTES and sha(archive) == ARCHIVE_SHA,
            'highest comparison artifact differs')
    source_manifest, source_raw, source_files = ee_source(repo / EE_ARCHIVE)
    with zipfile.ZipFile(archive) as bundle:
        names = safe_names(bundle)
        manifest_raw = bundle.read('hashes.json')
        entries_list = json.loads(manifest_raw)['artifacts']
        entries = {item['path']: item for item in entries_list}
        require(len(entries) == len(entries_list) and set(entries) - names == {MISSING_SOURCE} and
                names - set(entries) == {'hashes.json'}, 'unexpected missing/output manifest members')

        def read(path):
            # Only this known redundant SOURCE input is recovered in memory.
            # Executable, output and measurements must exist in the raw archive.
            value = source_files['.dockerignore'] if path == MISSING_SOURCE else bundle.read(path)
            item = entries[path]
            require(len(value) == item['bytes'] and digest(value) == item['sha256'], 'artifact member differs: ' + path)
            return value

        for path in entries:
            read(path)
        summary_raw = read('summary.json'); summary = json.loads(summary_raw)
        prepare_raw = read('prepare-proof.json'); prepared = json.loads(prepare_raw)
        binding_raw = read('binding-proof.json'); binding = json.loads(binding_raw)
        original_raw = read('programs/ee/proof.json'); original = json.loads(original_raw)
        require(summary['status'] == 'passed' and summary['exit_code'] == 0 and
                summary['completed_pairs'] == summary['expected_pairs'] == 40 and
                len(summary['checks']) == 6 and all(value is True for value in summary['checks'].values()),
                'historical EE comparison incomplete')
        require(prepared['status'] == binding['status'] == original['status'] == 'verified' and
                summary['harness_commit'] == prepared['harness_commit'] == HARNESS_COMMIT and
                binding['prepare_proof_sha256'] == original['prepare_proof_sha256'] == digest(prepare_raw) and
                binding['ee'] == original, 'historical EE binding differs')
        require(original['source_commit'] == prepared['ee_source_commit'] == EE and
                original['source_tree'] == prepared['ee_source_tree'] == EE_TREE and
                original['source_archive_sha256'] == prepared['ee_archive_sha256'] == EE_ARCHIVE_SHA and
                original['source_manifest_sha256'] == prepared['ee_source_manifest_sha256'] == digest(source_raw) and
                read('ee-source-manifest.json') == source_raw and
                original['source_sha256'] == prepared['ee_source_sha256'] == {p: digest(v) for p, v in source_files.items()},
                'historical complete EE source differs')
        require({p.removeprefix('ee-context/') for p in entries if p.startswith('ee-context/')} == set(source_files),
                'historical EE context inventory differs')
        for path, value in source_files.items():
            require(read('ee-context/' + path) == value, 'historical source context differs: ' + path)
        for path, value in prepared['git_input_sha256'].items():
            require(digest(git(repo, 'show', HARNESS_COMMIT + ':' + path)) == value,
                    'historical harness/Git input differs: ' + path)
        require(original['new_pgo_builds'] == 0 and original['new_non_pgo_builds'] == 1 and
                original['kind'] == 'one_original_root_docker_rebuild_without_pgo', 'EE build kind differs')
        program = read('programs/ee/minivec')
        require(len(program) > 64 and program[:6] == b'\x7fELF\x02\x01' and
                int.from_bytes(program[18:20], 'little') == 62 and
                digest(program) == original['binary_sha256'] == summary['binary_sha256']['ee'] == EE_BINARY_SHA and
                read('ee-binary-sha256.txt').decode().split()[0] == EE_BINARY_SHA, 'actual EE ELF differs')
        image = original['image_id']; inspected = json.loads(read('ee-image-inspect.json'))
        require(summary['ee_image_id'] == image == read('ee-image-id.txt').decode().strip() and
                len(inspected) == 1 and inspected[0]['Id'] == image and
                inspected[0]['Config']['Entrypoint'] == ['/minivec', '--data', '/data'], 'actual historical EE image differs')

        public = json.loads(git(repo, 'show', HARNESS_COMMIT + ':mysqltest/manifest.json'))['cases']
        for directory, expected in (('ee-image-handshake', {'handshake'}), ('ee-image-public', {x['id'] for x in public})):
            data = json.loads(read(directory + '/result.json'))
            require(data['passed'] is True and len(data['cases']) == len(expected) and
                    {x['id'] for x in data['cases']} == expected and all(x['status'] == 'PASS' for x in data['cases']),
                    'historical EE public checks incomplete')
        expected_json = ['typed_rows', 'row_encoding_reuse', 'transactions_and_invalid_input', 'crash_recovery',
                         'mode_switch', 'fragmented_jsonl', 'parallel_insert_errors', 'legacy_mode']
        require([x for x in read('ee-json.log').decode().splitlines() if x.startswith('PASS ')] ==
                ['PASS ' + x for x in expected_json], 'historical EE JSON checks incomplete')
        for name in ('ee-topk.log', 'ee-topk-parallel.log'):
            require(read(name).decode().strip() == 'PASS JSONL TopK against exact float32 oracle, including ties, filters and rollback',
                    'historical EE TopK checks incomplete')
        for label in ('retained', 'crc'):
            identity_raw = read('ee-vs-' + label + '/identities.json'); ids = json.loads(identity_raw)
            require(digest(identity_raw) == binding['comparison_identity_sha256'][label] and
                    ids['baseline']['binary_sha256'] == EE_BINARY_SHA and ids['baseline']['source_commit'] == EE and
                    ids['baseline']['evidence_sha256'] == digest(original_raw), 'historical direct EE identity differs')
            for count, cases in ((1, CASES), (2, PROTECTION_CASES)):
                data = json.loads(read('ee-vs-' + label + f'/measurements-{count}cpu.json'))
                verify_measurement(data, count, cases, digest(identity_raw), ids)
                measured = {}
                for path, value in data['tool_hashes'].items():
                    matches = [name for name in MEASURED_TOOLS if path.endswith('/' + name)]
                    require(len(matches) == 1 and matches[0] not in measured and
                            value == prepared['git_input_sha256'][matches[0]], 'historical measured tools differ')
                    measured[matches[0]] = value
                require(set(measured) == MEASURED_TOOLS, 'historical measured tool inventory differs')
        proof = {'status': 'verified', 'source_commit': EE, 'source_tree': EE_TREE, 'binary_sha256': EE_BINARY_SHA,
            'kind': 'imported_historical_original_root_no_pgo', 'new_root_builds': 0, 'new_pgo_builds': 0,
            'archive_sha256': ARCHIVE_SHA, 'archive_bytes': ARCHIVE_BYTES,
            'prior_run_id': 43459289479, 'prior_artifact_id': 14054549959, 'prior_harness_commit': HARNESS_COMMIT,
            'historical_image_id': image, 'source_archive_sha256': EE_ARCHIVE_SHA,
            'source_manifest_sha256': digest(source_raw), 'source_sha256': original['source_sha256'],
            'original_proof_sha256': digest(original_raw), 'prior_summary_sha256': digest(summary_raw),
            'prior_binding_sha256': digest(binding_raw), 'prior_manifest_sha256': digest(manifest_raw),
            'historical_verified_pairs': 40, 'raw_source_archive_complete': False,
            'missing_raw_members': [MISSING_SOURCE],
            'source_recovery': {'kind': 'only_known_source_file_checked_in_memory', 'path': MISSING_SOURCE,
                'sha256': digest(source_files['.dockerignore']), 'bytes': len(source_files['.dockerignore']),
                'source_archive_member': 'context/.dockerignore', 'raw_archive_modified': False},
            'official_binary_identity_known': False,
            'scope': 'Imports the prior actual EE image executable. Original Git objects, every artifact member, image/executable binding and prior checks are verified. No new EE build, no recovered official executable, no fabricated missing outputs.'}
        evidence = {'original-proof.json': original_raw, 'prior-summary.json': summary_raw,
            'prior-binding.json': binding_raw, 'prior-prepare.json': prepare_raw,
            'prior-hashes.json': manifest_raw, 'source-manifest.json': source_raw}
    return program, proof, evidence


def prepare_insert(repo, binary, image, out):
    # Current N uses only its own current root gates. Historical EE never does.
    current = prepare_root(repo, binary, image, out)
    program, proof, evidence = verified_ee(repo)
    directory = out / 'ee'; directory.mkdir()
    (directory / 'minivec').write_bytes(program); (directory / 'minivec').chmod(0o755)
    for name, value in evidence.items():
        (directory / name).write_bytes(value)
    save(directory / 'proof.json', proof)
    original_ids_raw = (out / 'identities.json').read_bytes(); ids = json.loads(original_ids_raw)
    require(ids['candidate']['binary_sha256'] == current['binary_sha256'] == sha(binary), 'current candidate differs')
    (out / 'retained-identities.json').write_bytes(original_ids_raw)
    highest = {'binary': 'ee/minivec', 'binary_sha256': EE_BINARY_SHA, 'source_commit': EE,
        'evidence_file': 'ee/proof.json', 'evidence_sha256': sha(directory / 'proof.json'),
        'evidence_binary_pointer': ['binary_sha256'], 'evidence_commit_pointer': ['source_commit'], 'environment': {}}
    save(out / 'ee-identities.json', {'baseline': highest, 'candidate': ids['candidate']})
    save(out / 'insert-binding-proof.json', {'status': 'verified',
        'candidate_source_commit': current['source_commit'], 'candidate_binary_sha256': current['binary_sha256'],
        'candidate_image_id': image, 'current_root_proof_sha256': sha(out / 'root-proof.json'),
        'ee_import_proof_sha256': sha(directory / 'proof.json'),
        'retained_identity_sha256': sha(out / 'retained-identities.json'),
        'ee_identity_sha256': sha(out / 'ee-identities.json'), 'new_ee_builds': 0, 'new_pgo_builds': 0})
    return current, proof


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'binary', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--image', required=True); args = p.parse_args()
    current, historical = prepare_insert(args.repo.resolve(), args.binary.resolve(), args.image, args.output.resolve())
    print(json.dumps({'status': 'verified', 'candidate_binary_sha256': current['binary_sha256'],
                      'ee_binary_sha256': historical['binary_sha256'], 'new_ee_builds': 0, 'new_pgo_builds': 0}))


if __name__ == '__main__': main()
