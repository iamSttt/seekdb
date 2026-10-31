"""Summarize paired development SQL results; never establish an official score."""
import argparse, json, math
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('results',type=Path)
a=p.parse_args()
summary={'scope':'Development SQL pairs only. Hidden250K is unavailable; official score and gates remain unverified.','datasets':{}}
for dataset in ('normal128','normal256','sift250k'):
    f=json.loads((a.results/f'sql_{dataset}_frozen.json').read_text())
    c=json.loads((a.results/f'sql_{dataset}_candidate96.json').read_text())
    fm=json.loads((a.results/f'sql_{dataset}_frozen_memory.json').read_text())
    cm=json.loads((a.results/f'sql_{dataset}_candidate96_memory.json').read_text())
    minimum={10:0.95,100:0.95} if dataset=='sift250k' else {10:0.90,100:0.85}
    r={'build_ratio':f['build_first_s']/c['build_first_s'],
       'query_ratio':math.sqrt(c['qps10']/f['qps10']*c['qps100']/f['qps100']),
       'sampled_vram_ratio':cm['vram_peak_sampled']/fm['vram_peak_sampled'],
       'recall_delta':{str(k):c[f'recall{k}']-f[f'recall{k}'] for k in (10,100)},
       'recall':{str(k):c[f'recall{k}'] for k in (10,100)}}
    r['observed_checks_pass']=(f['integrity']==c['integrity']=='pass'
       and all(c[f'recall{k}']>=minimum[k] and r['recall_delta'][str(k)]>=-0.005 for k in (10,100))
       and r['query_ratio']>=0.9 and r['sampled_vram_ratio']<=1.25
       and cm['vram_peak_sampled']<=45*10**9)
    summary['datasets'][dataset]=r
s=summary['datasets']['sift250k'];n=summary['datasets']['normal256']
summary['development_weighted_ratios']={'build':s['build_ratio']**0.4*n['build_ratio']**0.6,
    'query':s['query_ratio']**0.4*n['query_ratio']**0.6,
    'warning':'Uses normal256 instead of Hidden250K. This is NOT an official result or a validated score prediction.'}
print(json.dumps(summary,indent=2))
