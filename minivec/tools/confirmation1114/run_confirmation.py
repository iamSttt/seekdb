#!/usr/bin/env python3
"""One separately reported seed19087 confirmation on a new runner; no rerun loop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import traceback

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import runner_identity
import verify_sources


def require(value, message):
    if not value:
        raise ValueError(message)


def read(path):
    return json.loads(Path(path).read_bytes())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf8')
    temporary.replace(path)


def bind(role, binary, source, proof_path):
    require(re.fullmatch('[0-9a-f]{40}', source) is not None, 'full source commit required')
    raw = binary.read_bytes()
    require(raw[:6] == b'\x7fELF\x02\x01' and int.from_bytes(raw[18:20], 'little') == 62,
            role + ' must be the actual extracted x86-64 ELF')
    require(os.access(binary, os.X_OK), role + ' ELF is not executable')
    digest = hashlib.sha256(raw).hexdigest()
    proof = read(proof_path)
    require(proof.get('source_commit', proof.get('runtime_commit')) == source and
            proof.get('binary_sha256', proof.get('image_binary_sha256')) == digest,
            role + ' root proof/source/ELF mismatch')
    return {'source_commit': source, 'binary_sha256': digest, 'proof_file': str(proof_path),
            'proof_sha256': sha(proof_path), 'scope': 'Identity binding; full root correctness is a separate prerequisite.'}


def check_screen(screen, module, seed, bindings):
    require(screen['status'] == 'screen_completed' and screen['expected_pairs'] == screen['completed_pairs'] == 12,
            'screen is failed or incomplete')
    require(screen['all_EE_primary_thresholds_passed'] is True, 'screen EE thresholds not passed')
    require(set(screen['arms']) == set(screen['comparisons']) == {'1', '2'}, 'both CPU arms required')
    expected = [dict(cpu_count=cpu, scenario='standard10k', seed=seed, pair_index=i,
                     order=['baseline', 'candidate'] if i % 2 == 0 else ['candidate', 'baseline'])
                for cpu in (1, 2) for i in range(6)]
    require(screen['plan'] == expected and screen['workload']['seed'] == seed,
            'wrong seed or executed plan')
    for role in ('baseline', 'candidate'):
        identity = screen['identities'][role]
        require(identity['source_commit'] == bindings[role]['source_commit'] and
                identity['binary_sha256'] == bindings[role]['binary_sha256'],
                'screen uses a different ' + role + ' source or ELF')
    for cpu in (1, 2):
        raw = screen['arms'][str(cpu)]['data']
        require(raw['status'] == 'passed' and raw['expected_pairs'] == raw['completed_pairs'] == len(raw['pairs']) == 6,
                'missing raw confirmation pairs')
        executed = [dict(cpu_count=cpu, **{k: p[k] for k in ('scenario', 'seed', 'pair_index', 'order')})
                    for p in raw['pairs']]
        require(executed == [x for x in expected if x['cpu_count'] == cpu], 'raw executed plan differs')
        require(raw['query_mode_policy'] == 'exact' and raw['db_cpu_count'] == cpu, 'raw mode/CPU differs')
        require(len(raw['workloads']) == 1 and raw['workloads'][0]['scenario'] == 'standard10k'
                and raw['workloads'][0]['seed'] == seed, 'generator workload seed differs')
        require(raw['requested_limits'] == dict(cpu_quota_us=cpu * 100000, cpu_period_us=100000,
                                                memory_max='2147483648', swap_max='0'), 'raw resource limits differ')
        require(all(p['all_responses_identical'] is True for p in raw['pairs']), 'response/oracle gate absent')
        recomputed = module.summarize(raw, module.EE)
        require(recomputed == screen['comparisons'][str(cpu)] and recomputed['all_EE_primary_thresholds_passed'] is True,
                'original statistics/threshold result not reproducible')
        if seed == 19087:
            require(raw.get('highest_primary_confirmation') is True and raw.get('benchmark_seed') == 19087,
                    'helper did not explicitly enter seed19087 mode')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'baseline-proof', 'candidate-proof', 'initial-screen',
                 'initial-runner', 'launcher', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--candidate-source', required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    require(not output.exists(), 'refuse to overwrite confirmation output')
    output.mkdir(parents=True)
    report_path = output / 'confirmation.json'
    report = {'status': 'running', 'seed': 19087, 'confirmation_passed': False, 'official_score': None,
              'release_gate_passed': False, 'automatic_adoption': False, 'formal_submission': False,
              'merged_with_seed7319': False, 'repeat_until_pass': False, 'expected_pairs': 12,
              'scope': 'Separate new-runner EE confirmation; earlier R protection is a separate prerequisite.'}
    save(report_path, report)
    code = 20
    try:
        require(sys.platform.startswith('linux'), 'actual Linux CI runner required')
        report['prepared_sources'] = verify_sources.verify()
        module = verify_sources.module('run_seed_screen', HERE / 'seed_screen.py')
        current = runner_identity.capture()
        initial_runner = read(args.initial_runner)
        report['runner'] = current
        report['initial_runner'] = initial_runner
        report['initial_runner_evidence_sha256'] = sha(args.initial_runner)
        save(output / 'runner.json', current)
        runner_identity.require_fresh(initial_runner, current)
        bindings = {'baseline': bind('baseline', args.baseline, module.EE, args.baseline_proof),
                    'candidate': bind('candidate', args.candidate, args.candidate_source, args.candidate_proof)}
        report['bindings'] = bindings
        initial = read(args.initial_screen)
        check_screen(initial, module, 7319, bindings)
        report['initial_screen_sha256'] = sha(args.initial_screen)
        bound_plan = {'schedule': read(HERE / 'plan.json'), 'runtime_bindings': bindings,
                      'initial_screen_sha256': report['initial_screen_sha256'],
                      'initial_runner_evidence_sha256': report['initial_runner_evidence_sha256'],
                      'scope': 'Runtime sources and actual ELFs bound to the completed initial screen, independently of driver Git HEAD.'}
        save(output / 'bound-plan.json', bound_plan)
        report['bound_plan_sha256'] = sha(output / 'bound-plan.json')
        require(args.launcher.is_file() and os.access(args.launcher, os.X_OK), 'executable launcher required')
        # The launcher source is frozen; record the caller-built executable too.
        report['launcher_sha256'] = sha(args.launcher)
        command = [sys.executable, '-B', str(HERE / 'seed_screen.py'),
                   '--baseline', str(args.baseline.resolve()), '--candidate', str(args.candidate.resolve()),
                   '--candidate-source', args.candidate_source,
                   '--baseline-evidence', str(args.baseline_proof.resolve()),
                   '--candidate-evidence', str(args.candidate_proof.resolve()),
                   '--launcher', str(args.launcher.resolve()), '--output', str(output / 'screen.json')]
        report['command'] = command
        save(report_path, report)
        environment = {k: v for k, v in os.environ.items() if not k.startswith('MVDB_')}
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        with (output / 'screen.log').open('wb') as log:
            code = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=False).returncode
        report['screen_exit_code'] = code
        require(code == 0, 'confirmation measurement failed; partial evidence retained')
        observed = read(output / 'screen.json')
        check_screen(observed, module, 19087, bindings)
        require(verify_sources.verify() == report['prepared_sources'], 'sources changed during confirmation')
        report.update(status='confirmation_complete_thresholds_passed', confirmation_passed=True,
                      completed_pairs=12, screen_sha256=sha(output / 'screen.json'))
        code = 0
    except BaseException as exc:
        report.update(status='confirmation_blocked_or_failed', confirmation_passed=False,
                      error=type(exc).__name__ + ': ' + str(exc), traceback=traceback.format_exc())
        code = code if code != 0 else 20
    finally:
        save(report_path, report)
    print(json.dumps({k: report[k] for k in ('status', 'seed', 'confirmation_passed', 'release_gate_passed')}))
    if code:
        sys.exit(code)


if __name__ == '__main__':
    main()
