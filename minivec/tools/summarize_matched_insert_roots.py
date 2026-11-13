#!/usr/bin/env python3
"""Bind eight default-policy pairs to two newly built, matched actual roots."""
import argparse
import json
from pathlib import Path
from prepare_matched_insert_roots import SOURCES, OLD, verify_inputs, gates, training_evidence, sha, save, require
from small_compute import matched_insert_plan, counters
from summarize_small_root import verify_measurement
from summarize_insert_copy_root import paired_standard, TOOLS as MEASURED_TOOLS


def collect(repo, out, exit_code):
    result = {'status': 'failed_or_incomplete', 'exit_code': exit_code, 'expected_pairs': 8, 'completed_pairs': 0,
        'new_pgo_builds_planned': 2, 'new_pgo_builds_verified': 0, 'source_sanitizers_rerun': False,
        'formal_submission': False, 'automatic_adoption': False, 'official_score': None, 'comparisons': {}}
    try:
        proof = json.loads((out / 'prepare-proof.json').read_bytes()); verify_inputs(repo, out, proof)
        binding = json.loads((out / 'binding-proof.json').read_bytes())
        ids_path = out / 'identities.json'; ids = json.loads(ids_path.read_bytes())
        require(binding['status'] == 'verified' and binding['prepare_proof_sha256'] == sha(out / 'prepare-proof.json') and
                binding['identity_sha256'] == sha(ids_path) and binding['new_pgo_builds'] == 2 and
                binding['source_sanitizers_rerun'] is False and binding['automatic_adoption'] is False, 'binding differs')
        result['harness_commit'] = proof['harness_commit']; result['programs'] = {}
        training = []
        for role, label in (('baseline', 'r2'), ('candidate', 'n2')):
            directory = out / label; record = json.loads((directory / 'proof.json').read_bytes())
            require(record == binding['programs'][label] and record['status'] == 'verified' and record['label'] == label.upper() and
                    record['source_commit'] == SOURCES[label] and record['new_pgo_builds'] == 1 and
                    record['source_sanitizers_rerun'] is False and record['source_sha256'] == proof['source_sha256'][label],
                    'matched source/program proof differs')
            require(record['prepare_proof_sha256'] == sha(out / 'prepare-proof.json') and
                    record['training_evidence_sha256'] == sha(directory / 'training-evidence.json'), 'program input proof changed')
            item = ids[role]
            require(item['binary_sha256'] == record['binary_sha256'] == sha(directory / 'minivec') and
                    item['source_commit'] == SOURCES[label] and item['environment'] == {} and
                    sha(out / item['binary']) == record['binary_sha256'] and
                    sha(out / item['evidence_file']) == item['evidence_sha256'], 'actual measured ELF identity differs')
            image = json.loads((directory / 'image-inspect.json').read_bytes())
            require(len(image) == 1 and image[0]['Id'] == record['image_id'] == (directory / 'image-id.txt').read_text().strip() and
                    (directory / 'binary-sha256.txt').read_text().split()[0] == record['binary_sha256'], 'actual root metadata differs')
            checks = gates(repo, directory); checks.update(original_root_docker_pgo=True, six_training_oracles=True)
            require(checks == record['checks'] and len(checks) == 9, 'listed actual-image gates differ')
            current_training = training_evidence(directory / 'docker-build.log', proof['source_sha256'][label])
            require(current_training == json.loads((directory / 'training-evidence.json').read_bytes()), 'training evidence changed')
            training.append(current_training['training_manifest'])
            result['programs'][label] = {'label': label.upper(), 'source_commit': SOURCES[label],
                'binary_sha256': record['binary_sha256'], 'image_id': record['image_id'], 'actual_gate_categories': len(checks),
                'same_as_prior_imported_bytes': record['binary_sha256'] == OLD[label], 'prior_binary_sha256': OLD[label]}
            result['new_pgo_builds_verified'] += 1
        require(training[0] == training[1], 'two training manifests differ')
        host = None; workloads = None; cpus = {}
        for count in (1, 2):
            data = json.loads((out / f'measurements-{count}cpu.json').read_bytes())
            verify_measurement(data, count, ('standard10k',), sha(ids_path), ids, matched_roots=True)
            require(data['plan'] == matched_insert_plan(count) and data['launcher_sha256'] == sha(out / 'cgroup_exec'),
                    'matched plan/launcher differs')
            current_host = (data['host'], data['taskset'], data['launcher_sha256'])
            require(host is None or host == current_host, 'host/tools differ between CPU plans'); host = current_host
            require(workloads is None or workloads == data['workloads'], 'workload differs between CPU plans'); workloads = data['workloads']
            require(len(workloads) == 1 and workloads[0]['scenario'] == 'standard10k' and
                    workloads[0]['seed'] == 7319 and workloads[0]['rows'] == 10000 and workloads[0]['dimension'] == 128,
                    'default workload identity differs')
            observed_tools = {}
            for path, digest in data['tool_hashes'].items():
                name = Path(path).relative_to(repo).as_posix()
                require(name not in observed_tools and digest == proof['harness_file_sha256'][name] == sha(Path(path)),
                        'measurement tools differ from Git')
                observed_tools[name] = digest
            require(set(observed_tools) == MEASURED_TOOLS, 'measured tool inventory differs')
            cpus[count] = data['db_cpus']
            result['comparisons'][str(count) + 'cpu'] = paired_standard(data)
            # User/system counters are retained separately, not inferred from
            # 100 Hz task ticks or from wall minus CPU.
            cpu_details = []
            for pair in data['pairs']:
                row = {k: pair[k] for k in ('pair_index', 'order')}
                for role in ('baseline', 'candidate'):
                    phases = {v['phase']: v for v in pair[role]['adapter']['phase_cgroups']}
                    a, b = (counters(phases[name]['cpu.stat']) for name in ('before_ingest', 'after_ingest'))
                    row[role] = {key: b[key] - a[key] for key in ('usage_usec', 'user_usec', 'system_usec', 'throttled_usec')}
                    require(all(value >= 0 for value in row[role].values()), 'negative ingest CPU counter')
                cpu_details.append(row)
            result['comparisons'][str(count) + 'cpu']['ingest_cpu_components'] = cpu_details
            result['completed_pairs'] += len(data['pairs'])
        require(cpus[1] == cpus[2][:1] and result['completed_pairs'] == 8 and exit_code == 0, 'matched screen incomplete')
        result.update(status='passed', decision='manual_review_required_no_automatic_adoption',
            any_joint_default_regression=any(c['joint_wall_cpu_regression_over_2pct_majority'] for c in result['comparisons'].values()))
    except Exception as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
    result['limitations'] = [
        'Two new actual root programs R2/N2, not relabelled old acdd/5606 and not official binaries. No EE or new algorithm/flags/training policy.',
        'Listed fresh gates are image handshake/public20/interactive, ELF JSON8/UTF8-54/TopK serial+parallel and original six training oracles. Source ASAN/UBSan results are historical and were not rerun; this is not 18 fresh root/source gates.',
        'Only standard10k, one seed, four pairs per CPU. Default exact/2 GiB/swap0 and every COMMIT/negative sample retained. Not a formal-score guarantee or automatic adoption.',
        'Same host and build path remove those historical differences, but source-induced PGO feedback and machine-code layout remain part of the candidate effect. gcda hashes alone are not function counts.',
        'First lifecycle wall is summed request timing through first shutdown; labelled CPU stops before shutdown. Ingest CPU spans BEGIN/INSERT/COMMIT; restart verification is not pure query CPU.']
    save(out / 'summary.json', result)
    save(out / 'hashes.json', {'artifacts': [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
        for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'hashes.json']})
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--exit-code', type=int, required=True); args = p.parse_args()
    result = collect(args.repo.resolve(), args.output.resolve(), args.exit_code)
    print(json.dumps({k: result.get(k) for k in ('status', 'completed_pairs', 'new_pgo_builds_verified', 'error')}))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__': main()
