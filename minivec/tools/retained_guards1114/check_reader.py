"""Read archived measurements only; never launch a benchmark or database."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import retained_guards as g

W=Path('C:/Users/Lenovo/Documents/Codex/2026-11-12/iamsttt-seekdb-https-github-com-iamsttt/work')
RAW=W/'highest-primary-quantizer-raw/highest-primary-root'
arms={str(cpu):{'data':g.read(RAW/f'retained-{cpu}cpu.json')} for cpu in (1,2)}
out=g.numerical_guards(arms)
sys.path.insert(0,str(W))
import analyze_highest_primary_root as prior
checks=[]
for cpu in (1,2):
    old=prior.screen_input(arms[str(cpu)]['data'],'retained',cpu)
    for key,pairs in old.items():
        label=':'.join(map(str,key));times,memories=g.rules.guard_metric_names(key[2])
        for metric in times+memories:
            expected=g.rules.summarize_metric(pairs,metric)
            assert out['guard_metrics'][label][metric]==expected,(label,metric)
            checks.append(label+'/'+metric)

def rejected(label,edit):
    changed=deepcopy(arms);edit(changed)
    try:g.numerical_guards(changed)
    except (ValueError,KeyError,IndexError):return label
    raise AssertionError('invalid evidence accepted: '+label)
negatives=[]
negatives.append(rejected('missing_cpu_arm',lambda x:x.pop('2')))
negatives.append(rejected('missing_pair',lambda x:x['1']['data']['pairs'].pop()))
negatives.append(rejected('missing_default_memory',lambda x:x['1']['data']['pairs'][0]['baseline']['cgroup']['resource_delta'].pop('memory_peak_bytes')))
negatives.append(rejected('zero_first_query_denominator',lambda x:x['1']['data']['pairs'][0]['baseline']['query']['query_before_update']['per_request_seconds'].__setitem__(0,0.0)))
negatives.append(rejected('spill_initial_query_count',lambda x:x['1']['data']['pairs'][4]['baseline']['query']['query_before_mutations']['per_request_seconds'].pop()))
negatives.append(rejected('wide_phase_missing',lambda x:x['2']['data']['pairs'][4]['baseline']['phases'].pop('wide_all')))
report={'status':'reader_checks_passed_no_execution','original_reader_metric_summaries_equal':len(checks),
        'checked_metrics':checks,'invalid_evidence_rejected':negatives,
        'historical_raw':[{'file':str(RAW/f'retained-{cpu}cpu.json'),'sha256':g.sha(RAW/f'retained-{cpu}cpu.json')} for cpu in (1,2)],
        'new_database_pairs':0,'new_pgo_builds':0,'release_gate_passed':False,
        'scope':'Postprocess historical data to check field/rule equivalence only; not results for any new candidate.'}
(g.HERE/'reader-checks.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:report[k] for k in ('status','original_reader_metric_summaries_equal','invalid_evidence_rejected','new_database_pairs')}))
