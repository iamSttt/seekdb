#!/usr/bin/env python3
"""Reusable isolated-role PGO build proofs; no production default changes."""
import json
import math
from pathlib import Path
import os
import platform
import shlex
import subprocess
import sys
import time

from regression_ablation_proof import COMPILER, COMMON, require, sha, tree, save, read, recorded


def render_dockerfile(original, flags):
    needle = "flags='" + COMMON + "'"
    generate = 'g++ $flags -fprofile-generate=/pgo -fprofile-update=atomic src/*.cpp -o /minivec;'
    use = 'g++ $flags -fprofile-use=/pgo -Werror=missing-profile -Werror=coverage-mismatch src/*.cpp -o /minivec'
    effective = 'flags="$flags $FEATURE_FLAGS";'
    require(original.count(needle) == original.count(generate) == original.count(use) == original.count(effective) == original.count('strip /minivec;') == 1 and original.count('g++ $flags ') == 3, 'production build anchors changed')
    require('FROM ' + COMPILER + ' AS build' in original.splitlines(), 'unexpected compiler')
    continuation = ' ' + chr(92) + '\n'
    proof = continuation + '         cp /minivec /training/instrumented-minivec;' + continuation + '         sha256sum /minivec > /training/instrumented-binary-sha256.txt;'
    replacement = "flags='" + COMMON + ' ' + ' '.join(flags) + "'"
    effective_proof = effective + continuation + '    printf "%s\\n" "$flags" > /training/effective-flags.txt;'
    layout = 'cp /minivec /training/unstripped-minivec;' + continuation + '    nm -nSC /minivec > /training/symbols.txt;' + continuation + '    objdump -h /minivec > /training/sections.txt;' + continuation + '    strip /minivec;'
    modified = original.replace(needle, replacement).replace(generate, generate + proof).replace(effective, effective_proof).replace('strip /minivec;', layout)
    require(modified.replace(replacement, needle).replace(generate + proof, generate).replace(effective_proof, effective).replace(layout, 'strip /minivec;') == original, 'unreviewed build mutation')
    return modified


def initialize(out, scratch, baseline, revision, role_flags, plan):
    require('baseline' in role_flags and len(baseline) == len(revision) == 40, 'baseline/full revision missing')
    roles = {}
    for role, flags in role_flags.items():
        snapshot = scratch / ('baseline' if role == 'baseline' else 'candidate')
        path = out / 'build-input' / ('Dockerfile.' + role)
        path.write_text(render_dockerfile((snapshot / 'Dockerfile').read_text(), flags))
        roles[role] = {'source_commit': baseline if role == 'baseline' else revision, 'snapshot': snapshot.name,
            'feature_flags': flags, 'root_feature_flags_override': '', 'engine_source': tree(snapshot / 'minivec/src'), 'third_party': tree(snapshot / 'minivec/third_party'),
            'training_source': {n: sha(snapshot / 'minivec/tools/pgo' / n) for n in ('train.cpp', 'run-training.sh')},
            'production_dockerfile_sha256': sha(snapshot / 'Dockerfile'), 'dockerignore_sha256': sha(snapshot / '.dockerignore'), 'build_dockerfile_sha256': sha(path)}
    reference = next(role for role in role_flags if role != 'baseline')
    save(out / 'build-input.json', {'baseline_commit': baseline, 'candidate_commit': revision, 'harness_commit': revision,
        'compiler_image': COMPILER, 'roles': roles, 'snapshot_files': {r: tree(scratch / r) for r in ('baseline', 'candidate')},
        'source_differences': {part: [n for n in sorted(set(roles['baseline'][part]) | set(roles[reference][part])) if roles['baseline'][part].get(n) != roles[reference][part].get(n)] for part in ('engine_source', 'third_party', 'training_source')},
        'plan': plan, 'expected_pairs': len(plan), 'automatic_adoption': False, 'official_score': None})


def verify_builds(out, scratch, role_flags):
    inputs = read(out / 'build-input.json'); evidence = {}
    require(set(inputs['roles']) == set(role_flags), 'role inventory differs')
    for role, source in inputs['roles'].items():
        target = out / (role + '-pgo'); training = target / 'training'
        require(source['feature_flags'] == role_flags[role] and source['root_feature_flags_override'] == '', 'unexpected role flags')
        require(tree(target / 'built-source') == source['engine_source'] and tree(target / 'built-third-party') == source['third_party'], 'built engine/thirdparty differs')
        require({n: sha(training / n) for n in source['training_source']} == source['training_source'], 'training source differs')
        require(sha(out / 'build-input' / ('Dockerfile.' + role)) == source['build_dockerfile_sha256'], 'build Dockerfile changed')
        effective = shlex.split((training / 'effective-flags.txt').read_text())
        require(effective == shlex.split(COMMON) + role_flags[role], 'effective flags leaked/changed')
        profiles = tree(training / 'profile-data')
        require(profiles == recorded(training / 'profile-sha256.txt', '/pgo/') and len(profiles) == sum(n.endswith('.cpp') for n in source['engine_source']) and all(n.endswith('.gcda') for n in profiles), 'own-source/profile proof differs')
        instrumented = sha(training / 'instrumented-minivec')
        require(recorded(training / 'instrumented-binary-sha256.txt', '/') == {'minivec': instrumented}, 'instrumented binary differs')
        corpus = tree(training / 'corpus'); require(corpus == recorded(training / 'corpus-sha256.txt', '/training/corpus/'), 'corpus differs')
        oracle = [json.loads(line) for line in (training / 'verification.jsonl').read_text().splitlines()]
        responses = {p.name: sha(p) for p in training.glob('responses-*.jsonl')}
        require(len(oracle) == len(responses) == 6 and all(v['status'] == 'passed' and type(v['responses']) is int and v['responses'] > 0 for v in oracle) and {v['actual_file'] for v in oracle} == set(responses), 'training oracle incomplete')
        binary = sha(target / 'minivec')
        require(recorded(target / 'binary-sha256.txt', str(target) + '/') == {'minivec': binary}, 'runtime binary extraction differs')
        require((training / 'symbols.txt').stat().st_size and (training / 'sections.txt').stat().st_size, 'layout diagnostic missing')
        evidence[role] = {'source_commit': source['source_commit'], 'feature_flags': role_flags[role], 'root_feature_flags_override': '',
            'effective_compiler_flags': effective, 'effective_flags_sha256': sha(training / 'effective-flags.txt'),
            'source_sha256': source['engine_source'], 'training_source_sha256': source['training_source'], 'build_dockerfile_sha256': source['build_dockerfile_sha256'],
            'binary_sha256': binary, 'instrumented_binary_sha256': instrumented, 'unstripped_binary_sha256': sha(training / 'unstripped-minivec'),
            'layout_sha256': {n: sha(training / n) for n in ('symbols.txt', 'sections.txt')}, 'profile_sha256': profiles,
            'training_corpus_sha256': corpus, 'training_response_sha256': responses, 'training_oracle': oracle, 'image_id': read(target / 'image.json')[0]['Id']}
    require(all(p['training_corpus_sha256'] == evidence['baseline']['training_corpus_sha256'] for p in evidence.values()), 'roles trained on different corpora')
    candidates = [p for r, p in evidence.items() if r != 'baseline']
    require(all(p['source_sha256'] == candidates[0]['source_sha256'] for p in candidates), 'candidate source variants differ')
    for name, files in inputs['snapshot_files'].items(): require(tree(scratch / name) == files, 'frozen source/harness snapshot changed')
    save(out / 'context.json', {'baseline_commit': inputs['baseline_commit'], 'candidate_commit': inputs['candidate_commit'], 'harness_commit': inputs['harness_commit'],
        'compiler_image': COMPILER, 'profile_source_proof': evidence, 'source_differences': inputs['source_differences'],
        'profile_generate_flags': ['-fprofile-generate=/pgo', '-fprofile-update=atomic'],
        'profile_use_flags': ['-fprofile-use=/pgo', '-Werror=missing-profile', '-Werror=coverage-mismatch'],
        'training_corpora_equal': True, 'candidate_sources_identical': True, 'platform': platform.platform(), 'cpu_info': (out / 'lscpu.txt').read_text(),
        'expected_pairs': inputs['expected_pairs'], 'limitations': ['Source baseline is delivered e020 with four original flags, independently rebuilt here. Binary equality to earlier deliveries must be checked, not assumed either way.',
            'Each role trains its own original PGO; identical-binary A/A checks process/host variation, not independent PGO rebuild variation.',
            'Source/profile/layout/binary proofs bind this run. No automatic adoption or formal submission.']})


def gates(out, scratch, roles):
    context = read(out / 'context.json'); candidate = scratch / 'candidate'
    checks = {'status': 'running', 'checks': []}; save(out / 'correctness.json', checks)
    for role in roles:
        binary = out / (role + '-pgo') / 'minivec'
        tests = [('json_protocol', ['json_protocol.py', '--binary', str(binary), '--timeout', '15'], {}),
            ('utf8_atomicity', ['test_typed_response.py', '--binary', str(binary), '--output', str(out / (role + '-utf8.json'))], {}),
            ('topk_serial', ['topk_bounds.py', '--binary', str(binary)], {'MVDB_QUERY_THREADS': '1'}),
            ('topk_parallel', ['topk_bounds.py', '--binary', str(binary)], {'MVDB_QUERY_THREADS': '2'})]
        for label, args, environment in tests:
            (out / 'stage.txt').write_text('gate/' + role + '/' + label); started = time.monotonic(); error = None
            try:
                run = subprocess.run([sys.executable, str(candidate / 'minivec/tests' / args[0]), *args[1:]], capture_output=True, timeout=240, env={**os.environ, **environment})
                code, stdout, stderr = run.returncode, run.stdout, run.stderr
            except subprocess.TimeoutExpired as exc: code, stdout, stderr, error = None, exc.stdout or b'', exc.stderr or b'', 'timeout'
            for stream, content in (('stdout', stdout), ('stderr', stderr)): (out / (role + '-pgo') / (label + '.' + stream + '.log')).write_bytes(content)
            checks['checks'].append({'role': role, 'test': label, 'returncode': code, 'failure': error, 'environment': environment,
                'elapsed_seconds': time.monotonic() - started, 'binary_sha256': context['profile_source_proof'][role]['binary_sha256']})
            if code != 0: checks['status'] = 'failed'
            save(out / 'correctness.json', checks); require(code == 0, role + '/' + label + ' failed; no performance observation')
    checks['status'] = 'passed'; save(out / 'correctness.json', checks)


def verify_measurements(out, bench, role_flags, baseline, unit_cases):
    context, data, checks = [read(out / n) for n in ('context.json', 'measurements.json', 'correctness.json')]
    require(data['status'] == checks['status'] == 'passed' and 'error' not in data and 'failed_pair' not in data, 'failed/incomplete reports')
    require(data['build_context_sha256'] == sha(out / 'context.json') and data['baseline_commit'] == baseline == context['baseline_commit'] and data['candidate_commit'] == context['candidate_commit'], 'context/commit binding differs')
    require(data['plan'] == bench.make_plan(), 'measurement plan changed')
    expected = bench.validate_plan(data['pairs'])
    require(data['binary_unchanged'] is True and len(data['pairs']) == len(bench.make_plan()), 'incomplete pairs or binary changed')
    require(data['summary'] == bench.summarize(data['pairs']), 'summary differs from raw pairs')
    inputs = read(out / 'build-input.json')
    source_files = inputs['snapshot_files']['candidate']
    require(data['driver_sha256'] == source_files['minivec/tools/' + Path(bench.__file__).name] and
            data['harness_sha256'] == {n: source_files['minivec/tools/' + n] for n in ('benchmark_iteration.py', 'benchmark_report_matrix.py', 'benchmark_projection.py', 'benchmark_filter_repeat.py', 'benchmark_regression_ablation.py')}, 'actual benchmark driver/harness bytes differ from source snapshot')
    require(len(checks['checks']) == 4 * len(role_flags) and {(v['role'], v['test']) for v in checks['checks']} == {(r, t) for r in role_flags for t in ('json_protocol', 'utf8_atomicity', 'topk_serial', 'topk_parallel')}, 'missing binary gate')
    want_utf8 = {(raw, dim, path) for raw in ('ff', 'c080', 'eda080', 'e4b8', 'f4908080', '80') for dim in (None, 127, 128) for path in ('ORDER BY id LIMIT 10', 'unbounded', 'ORDER BY id LIMIT 4096')}
    for role, proof in context['profile_source_proof'].items():
        require(sha(out / (role + '-pgo') / 'minivec') == proof['binary_sha256'] == data['binaries'][role]['sha256'], 'measured binary mismatch')
        utf8 = read(out / (role + '-utf8.json'))
        require(utf8['status'] == 'passed' and len(utf8['checks']) == 54 and {(c['malformed_hex'], c['projected_vector_dimension'], c['path']) for c in utf8['checks']} == want_utf8 and all(c['status'] == 'passed' for c in utf8['checks']), 'UTF8 boundary cases incomplete')
    for item in checks['checks']: require(item['returncode'] == 0 and item['binary_sha256'] == context['profile_source_proof'][item['role']]['binary_sha256'], 'gate binary mismatch/failure')
    corpus = {(w['scenario'], w['seed']): w for w in data['workloads']}
    require(len(corpus) == len(data['workloads']) == len(bench.CASES) * len(bench.SEEDS), 'workload identities differ')
    require(set(corpus) == {(name, seed) for name in bench.CASES for seed in bench.SEEDS}, 'unexpected workload identity')
    require(data['environment_before_overrides'] == {} and len(set(data['host']['affinity'])) == 2, 'unexpected measurement environment/affinity')
    for (name, seed), item in corpus.items():
        if name in bench.PROJECTED:
            path = out / 'measurements.inputs' / f'{name}-{seed}.jsonl'
            require(Path(item['input_path']) == path and path.is_file() and path.stat().st_size == item['input_bytes'] and sha(path) == item['input_sha256'], 'projection input corpus hash/size/path mismatch')
            require((item['rows'], item['dimension'], item['string_codepoints']) == bench.projection.CASES[name], 'projection workload shape differs')
        else:
            rows, dim = (bench.base.FULL_CASES[name] if name in bench.ITERATION else bench.matrix.CASES[name])[:2]
            require((item['rows'], item['dimension'], item['queries']) == (rows, dim, 1000 if name in bench.ITERATION else 64), 'lifecycle workload shape differs')
    def positive(value):
        return type(value) in (int, float) and math.isfinite(value) and value > 0
    def close(a, b): return math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12)
    def hex64(value): return type(value) is str and len(value) == 64 and all(c in '0123456789abcdef' for c in value)
    for pair, planned in zip(data['pairs'], data['plan']):
        require(type(pair['seed']) is int and type(pair['pair_index']) is int, 'pair identity must contain integers')
        require(all(pair[k] == planned[k] for k in planned), 'schedule order changed')
        role = 'baseline' if pair['contrast'] == 'aa' else pair['contrast']
        require(pair['binary_sha256'] == {'baseline': data['binaries']['baseline']['sha256'], 'candidate': data['binaries'][role]['sha256']}, 'pair binary/AA mismatch')
        require(pair['binary_paths'] == {'baseline': str(out / 'baseline-pgo/minivec'), 'candidate': str(out / (role + '-pgo') / 'minivec')}, 'pair path/AA mismatch')
        require(pair['query_environment'] == {'MVDB_QUERY_THREADS': str(pair['query_threads'])}, 'pair policy override mismatch')
        w = corpus[(pair['scenario'], pair['seed'])]
        require(pair['workload_sha256'] == w.get('workload_sha256', w.get('input_sha256')), 'pair input changed')
        projection = pair['scenario'] in bench.PROJECTED
        require(pair.get('all_responses_identical_and_independently_verified' if projection else 'all_responses_identical') is True, 'response/oracle assertion missing')
        for side in ('baseline', 'candidate'):
            run = pair[side]; require('policy_evidence' in run and 'policy_validation_error' not in run, 'thread policy failed')
            if projection:
                labels = {'narrow_top10', 'wide_top10', 'narrow_large4096', 'wide_large4096', 'narrow_all', 'wide_all', 'wide_distance10'}
                require(run['status'] == 'passed' and set(run['phases']) == labels and all(p['independent_oracle'] and len(p['requests']) == 3 for p in run['phases'].values()), 'projection oracle/repeats incomplete')
                require(set(run['lifecycle_seconds']) == {'startup', 'create_and_insert', 'shutdown'} and all(positive(v) for v in run['lifecycle_seconds'].values()), 'missing projection lifecycle time')
                require(run['policy_evidence']['requested_query_threads'] == 1 and run['policy_evidence']['no_query_fanout_diagnostics'] is True and
                        len(run['policy_evidence']['task_snapshots']) == 14 and run['stderr']['bytes'] <= 4096 and 'fanout ' not in run['stderr']['tail'], 'projection thread policy proof differs')
                for phase in run['phases'].values():
                    require(all(positive(s['seconds']) and type(s['response_bytes']) is int and s['response_bytes'] > 0 for s in phase['requests']) and
                            close(phase['wall_seconds'], math.fsum(s['seconds'] for s in phase['requests'])) and phase['response_bytes'] == sum(s['response_bytes'] for s in phase['requests']), 'projection sample totals differ')
            else:
                queries = 1000 if pair['scenario'] in bench.ITERATION else 64
                require(set(run['query']) == {'query_before_update', 'query_after_restart'} and all(q['count'] == len(q['per_request_seconds']) == queries for q in run['query'].values()), 'query count/phase differs')
                require(run['correctness']['sampled_topk_oracle_queries'] == (sorted({0, 1, queries - 1}) if pair['scenario'] in bench.ITERATION else list(range(queries))), 'TopK oracle coverage differs')
                require(run['correctness']['all_restarted_ids_checked'] == w['rows'] and run['correctness']['restarted_vector_ids_checked'] == [0, 1, w['rows'] // 2, w['rows'] - 1], 'restart independent oracle coverage differs')
                require(run['policy_evidence'] == bench.repeat.observed_policy(run, pair['query_threads']), 'observed thread policy differs')
                for label, query in run['query'].items():
                    values = query['per_request_seconds']
                    require(all(positive(v) for v in values) and query['modes'] == ['exact' if i % 2 == 0 else 'ann' for i in range(queries)] and
                            query['first_request_seconds'] == values[0] and close(query['seconds'], math.fsum(values)) and close(run['phase_seconds'][label], math.fsum(values)), 'query samples/phase sum differ')
                phases = run['phase_seconds']
                require(set(phases) == {'startup', 'create_table', 'ingest', 'index_build', 'query_before_update', 'shutdown', 'restart', 'restart_verification_wall', 'query_after_restart', 'final_shutdown', 'measured_lifecycle'} and
                        all(positive(v) for v in phases.values()) and close(phases['measured_lifecycle'], math.fsum(v for k, v in phases.items() if k not in ('measured_lifecycle', 'restart_verification_wall'))), 'lifecycle phase set/sum differs')
        if projection:
            for label in pair['baseline']['phases']:
                for b, c in zip(pair['baseline']['phases'][label]['requests'], pair['candidate']['phases'][label]['requests']):
                    require(hex64(b['response_sha256']) and all(b[k] == c[k] for k in ('response_sha256', 'response_bytes', 'request_bytes')), 'projection raw response differs')
        else: require(hex64(pair['baseline']['response_sha256']) and pair['baseline']['response_sha256'] == pair['candidate']['response_sha256'], 'lifecycle raw responses differ')
    units = [line.split('\t') for line in (out / 'unit/passed.tsv').read_text().splitlines()]
    require(len(units) == len(unit_cases) and {tuple(v) for v in units} == set(unit_cases), 'source diagnostic/sanitizer gates incomplete')
    for role, mode, test in unit_cases:
        label = '-'.join((role, mode, test))
        flags = (out / 'unit' / (label + '-flags.txt')).read_text().splitlines()
        expected_flags = ['-std=c++17', '-pthread', '-Isrc', *role_flags['baseline'], '-DMVDB_JSON_RESPONSE_DIAGNOSTICS']
        if role != 'control': expected_flags += role_flags[role][len(role_flags['baseline']):]
        expected_flags += ['-O2'] if mode == 'normal' else ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        require(flags == expected_flags, 'unit flags differ')
        log = (out / 'unit' / (label + '.log')).read_text()
        require('PASS bulk typed admission boundaries' in log and ('bulk=' + str(int(role != 'control'))) in log, 'actual bulk/DOM admission proof missing')
    return {'status': 'passed', 'verified_total_pairs': len(expected), 'per_candidate_pairs': sum(p['contrast'] != 'aa' for p in data['pairs']), 'identical_baseline_aa_pairs': 18,
        'baseline_commit': baseline, 'candidate_commit': context['candidate_commit'], 'binary_unchanged': True,
        'source_profile_flags_verified': True, 'actual_binary_correctness_gates': 4 * len(role_flags), 'actual_utf8_cases': 54 * len(role_flags),
        'source_diagnostic_sanitizer_gates': len(unit_cases), 'measurement_sha256': sha(out / 'measurements.json'),
        'interpretation': context['limitations'], 'automatic_adoption': False, 'official_score': None}

