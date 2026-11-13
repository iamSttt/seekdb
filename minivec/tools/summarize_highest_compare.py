#!/usr/bin/env python3
"""Fail-closed independent 40-pair collection; no current-root or official claims."""
import argparse
import json
from pathlib import Path
from prepare_highest_compare import (EE, EE_TREE, EE_ARCHIVE, EE_ARCHIVE_SHA, CRC_ARCHIVE_SHA,
    RETAINED, CRC, RETAINED_SHA, CRC_SHA, MEASURED_TOOLS,
    sha, save, require, verify_git_inputs, verify_context, verify_gates, ee_source, digest)
from small_compute import CASES, PROTECTION_CASES, identities as resolve_identities
from summarize_small_root import verify_measurement


def collect(repo, out, exit_code):
    result = {'status': 'failed_or_incomplete', 'kind': 'independent_highest_source_comparison',
        'exit_code': exit_code, 'expected_pairs': 40, 'completed_pairs': 0,
        'new_pgo_builds': 0, 'new_non_pgo_builds_planned': 1, 'imported_pgo_programs': 2,
        'formal_submission': False, 'official_score': None, 'official_binary_identity_known': False,
        'automatic_adoption': False, 'comparisons': {}, 'checks': {},
        'limitations': ['EE is newly rebuilt from its original root Dockerfile, not recovered official executable bytes.',
            'Retained and CRC programs are imported unchanged from the prior verified artifact; no new PGO training.',
            'One runner, one seed, two AB/BA pairs per scenario. Explicit 1/2 logical CPU, exact, 2 GiB, swap disabled are local diagnostic settings.',
            'Historical prior passes are provenance only. Current success requires all 40 new pairs and EE image/JSON/TopK gates.',
            'Passing proves measured correctness/identity/resource checks, not a performance improvement or a submission recommendation.']}
    try:
        require(exit_code == 0, 'CI command failed before complete collection')
        proof = json.loads((out / 'prepare-proof.json').read_bytes())
        binding = json.loads((out / 'binding-proof.json').read_bytes())
        require(proof['status'] == binding['status'] == 'verified' and
                binding['prepare_proof_sha256'] == sha(out / 'prepare-proof.json'), 'preparation/binding changed')
        verify_git_inputs(repo, proof); verify_context(out, proof); verify_gates(repo, out)
        _, raw, files = ee_source(repo / EE_ARCHIVE)
        require(proof['ee_source_manifest_sha256'] == digest(raw) and
                proof['ee_source_sha256'] == {p: digest(v) for p, v in files.items()} and
                proof['ee_archive_sha256'] == EE_ARCHIVE_SHA and proof['crc_archive_sha256'] == CRC_ARCHIVE_SHA,
                'prepared archives/source differ from pinned evidence')
        ee = binding['ee']; require(ee['source_commit'] == EE and ee['new_pgo_builds'] == 0 and ee['new_non_pgo_builds'] == 1,
                                   'EE rebuild identity differs')
        require(ee['source_tree'] == EE_TREE and ee['source_archive_sha256'] == EE_ARCHIVE_SHA and
                ee['source_manifest_sha256'] == proof['ee_source_manifest_sha256'] and
                ee['source_sha256'] == proof['ee_source_sha256'] and
                ee['prepare_proof_sha256'] == sha(out / 'prepare-proof.json'), 'EE source proof changed')
        require(json.loads((out / 'programs/ee/proof.json').read_bytes()) == ee and
                sha(out / 'programs/ee/minivec') == ee['binary_sha256'], 'EE executable/proof changed')
        require((out / 'ee-image-id.txt').read_text().strip() == ee['image_id'] and
                json.loads((out / 'ee-image-inspect.json').read_bytes())[0]['Id'] == ee['image_id'], 'EE image changed')
        require((out / 'ee-binary-sha256.txt').read_text().split()[0] == ee['binary_sha256'], 'EE extracted hash changed')
        for label, expected, commit in (('retained', RETAINED_SHA, RETAINED), ('crc', CRC_SHA, CRC)):
            imported = json.loads((out / 'programs' / label / 'proof.json').read_bytes())
            require(imported == proof['imported'][label] and imported['binary_sha256'] == expected and
                    imported['source_commit'] == commit and imported['new_pgo_builds'] == 0 and
                    sha(out / 'programs' / label / 'minivec') == expected and
                    sha(out / 'programs' / label / 'original-proof.json') == imported['original_proof_sha256'],
                    'imported program/proof changed')
        result['checks'].update(complete_git_bound_inputs=True, original_ee_git_objects_and_context=True,
            ee_actual_image_public_json_topk=True, prior_program_bytes_and_proofs=True)
        result.update(harness_commit=proof['harness_commit'], ee_image_id=ee['image_id'],
                      binary_sha256={'ee': ee['binary_sha256'], 'retained': RETAINED_SHA, 'crc': CRC_SHA})
        hosts = []; cpus_seen = {}; workload_sets = {}
        for label in ('retained', 'crc'):
            directory = out / ('ee-vs-' + label); path = directory / 'identities.json'
            require(sha(path) == binding['comparison_identity_sha256'][label], 'comparison identity changed')
            ids = resolve_identities(path, True)
            require(ids['baseline']['source_commit'] == EE and ids['baseline']['binary_sha256'] == ee['binary_sha256'] and
                    ids['candidate']['binary_sha256'] == result['binary_sha256'][label], 'comparison roles swapped')
            compared = {'completed_pairs': 0, 'measurements': {}}
            for count, cases in ((1, CASES), (2, PROTECTION_CASES)):
                measurement = directory / f'measurements-{count}cpu.json'
                data = json.loads(measurement.read_bytes())
                verify_measurement(data, count, cases, sha(path), ids)
                require(data['launcher_sha256'] == sha(out / 'cgroup_exec'), 'launcher changed')
                measured = {}
                for name, value in data['tool_hashes'].items():
                    target = Path(name); relative = target.relative_to(repo).as_posix()
                    require(relative not in measured and value == proof['git_input_sha256'][relative] == sha(target),
                            'measured tool differs from Git proof')
                    measured[relative] = value
                require(set(measured) == MEASURED_TOOLS, 'measurement tool inventory differs')
                require(Path(data['taskset']['path']).is_file() and sha(Path(data['taskset']['path'])) == data['taskset']['sha256'],
                        'taskset changed')
                hosts.append(data['host'])
                if count in cpus_seen:
                    require(cpus_seen[count] == data['db_cpus'] and workload_sets[count] == data['workloads'],
                            'comparisons used different affinity/workload')
                cpus_seen[count] = data['db_cpus']; workload_sets[count] = data['workloads']
                compared['completed_pairs'] += len(data['pairs']); result['completed_pairs'] += len(data['pairs'])
                compared['measurements'][str(count) + 'cpu'] = {'sha256': sha(measurement), 'pairs': len(data['pairs'])}
            require(compared['completed_pairs'] == 20, 'incomplete direct comparison')
            result['comparisons']['ee_vs_' + label] = compared
        require(len(hosts) == 4 and all(host == hosts[0] for host in hosts) and result['completed_pairs'] == 40,
                'runner metadata differs or total incomplete')
        result['checks'].update(all_40_pairs_correct_identity_exact_resource=True, same_runner_affinity_and_workloads=True)
        result['status'] = 'passed'
    except Exception as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
    save(out / 'summary.json', result)
    # This is the only authoritative summary for this independent entry point.
    files = [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(out.rglob('*')) if p.is_file() and p != out / 'hashes.json']
    save(out / 'hashes.json', {'artifacts': files})
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--exit-code', type=int, required=True); args = p.parse_args()
    result = collect(args.repo.resolve(), args.output.resolve(), args.exit_code)
    print(json.dumps(result))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__': main()
