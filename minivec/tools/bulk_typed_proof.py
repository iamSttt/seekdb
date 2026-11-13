#!/usr/bin/env python3
"""Fail-closed proof collection for the explicit 84-pair bulk typed experiment."""
import argparse
from pathlib import Path
import benchmark_bulk_typed as bench
from ablation_build import initialize, verify_builds, gates, verify_measurements
from regression_ablation_proof import BASELINE, BASE_FLAGS, require, sha, save

FLAGS = {'baseline': BASE_FLAGS, 'bulk': BASE_FLAGS + ['-DMVDB_TYPED_BULK_VECTOR_ONLY']}
UNIT_CASES = {('control', 'normal', 'bulk_typed_response'), ('bulk', 'normal', 'bulk_typed_response'), ('bulk', 'sanitized', 'bulk_typed_response')}

def verify_results(out):
    return verify_measurements(out, bench, FLAGS, BASELINE, UNIT_CASES)

def collect(out, code):
    error = None; summary = {'status': 'incomplete_or_failed', 'verified_total_pairs': 0}
    if code == 0:
        try: summary = verify_results(out)
        except Exception as exc: error = f'{type(exc).__name__}: {exc}'
    summary.update(exit_code=code, stage=(out / 'stage.txt').read_text().strip(), collection_verification_error=error)
    save(out / 'summary.json', summary)
    save(out / 'run-status.json', {'exit_code': code, 'proof_status': summary['status'], 'collection_verification_error': error})
    save(out / 'hashes.json', {'artifacts': [{'path': p.relative_to(out).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'hashes.json'], 'note': 'Manifest excludes itself.'})
    for path in sorted(out.glob('*.json')):
        print('MINIVEC_BULK_TYPED_BEGIN ' + path.name); print(path.read_text()); print('MINIVEC_BULK_TYPED_END')
    require(code != 0 or error is None, 'zero process exit with failed evidence collection: ' + str(error))


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('command', choices=('init', 'verify', 'gates', 'collect'))
    p.add_argument('out', type=Path); p.add_argument('scratch_or_exit'); p.add_argument('revision', nargs='?'); args = p.parse_args()
    if args.command == 'collect': collect(args.out.resolve(), int(args.scratch_or_exit)); return
    out, scratch = args.out.resolve(), Path(args.scratch_or_exit).resolve()
    if args.command == 'init': initialize(out, scratch, BASELINE, args.revision, FLAGS, bench.make_plan())
    elif args.command == 'verify': verify_builds(out, scratch, FLAGS)
    else: gates(out, scratch, FLAGS)


if __name__ == '__main__': main()
