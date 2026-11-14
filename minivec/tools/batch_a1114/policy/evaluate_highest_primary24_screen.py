"""Numerical screen only; the caller must independently verify actual evidence.

No DB, compiler, network, or repository mutation is performed. Input groups use
(reference, cpu_count, scenario) keys and the exact preregistered pair order.
Each pair supplies pair_index, order and metrics[name]={baseline,candidate}.
The raw reader retains every other measured phase/resource value separately.
"""
import hashlib
import json
import math
from statistics import median

PREREG_SHA = 'a930984922150c82eea6c95fa56679cf7f0fe48ff0e88c72cdb5c36986a46d4d'
ERRATUM_SHA = 'cf2a1823f392f1baef512cc968f7068ed4b0529f786430770e026a18a5c63a1d'
ERRATUM_CANONICAL_SHA = '89e4e047683ab742b2f68736018a7041917205b6611245610c7be1cbcc2fc760'
DEFAULT_TIME = ('first_lifecycle_wall_seconds', 'first_1000_queries_wall_seconds',
                'first_1000_queries_cpu_usec', 'first_query_wall_seconds',
                'ingest_wall_seconds', 'ingest_cpu_usec', 'complete_lifecycle_cpu_usec')
DEFAULT_MEMORY = ('memory_peak_bytes', 'max_process_rss_bytes')
SPILL_TIME = ('first_lifecycle_wall_seconds', 'query_before_update_wall_seconds',
              'query_before_update_cpu_usec', 'first_query_wall_seconds',
              'ingest_wall_seconds', 'complete_lifecycle_cpu_usec')
WIDE_TIME = ('lifecycle_stage_wall_sum', 'seven_query_phases_wall_seconds',
             'seven_query_phases_cpu_usec', 'complete_lifecycle_cpu_usec')

def require(ok, message):
    if not ok:
        raise ValueError(message)

def encode(value):
    return (json.dumps(value,ensure_ascii=False,separators=(',',':'),allow_nan=False)+'\n').encode('utf8')

def read_preregistration(path):
    raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==PREREG_SHA,'preregistration raw SHA differs')
    return json.loads(raw)

def read_mapping_erratum(path):
    raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==ERRATUM_SHA,'field mapping erratum raw SHA differs')
    return json.loads(raw)

def guard_metric_names(scenario):
    if scenario=='standard10k':
        return DEFAULT_TIME,DEFAULT_MEMORY
    if scenario=='spill_high_q16':
        return SPILL_TIME,DEFAULT_MEMORY
    if scenario=='wide768':
        return WIDE_TIME,('memory_peak_bytes',)
    raise ValueError('unsupported scenario')

def expected_groups(plan):
    require(hashlib.sha256(encode(plan)).hexdigest()==PREREG_SHA,'preregistration value/order differs')
    groups={}
    for group in plan['groups']:
        for pair in group['pairs']:
            key=(group['reference'],group['db_cpu_count'],pair['scenario'])
            groups.setdefault(key,[]).append({'pair_index':pair['pair_index'],'order':pair['order']})
    require(sum(map(len,groups.values()))==24 and len(groups)==6,'preregistered plan changed')
    return groups

def summarize_metric(pairs, name):
    rows=[]
    for pair in pairs:
        require(name in pair['metrics'],'required metric absent: '+name)
        values=pair['metrics'][name]
        require(set(values)=={'baseline','candidate'},'metric side inventory differs')
        b,c=values['baseline'],values['candidate']
        require(type(b) in (int,float) and type(c) in (int,float) and
                math.isfinite(b) and math.isfinite(c) and b>0 and c>=0,
                'invalid/zero-denominator metric: '+name)
        ratio=c/b
        require(math.isfinite(ratio),'nonfinite paired ratio')
        rows.append({'pair_index':pair['pair_index'],'order':pair['order'],
                     'baseline':b,'candidate':c,'candidate_minus_baseline':c-b,
                     'candidate_over_baseline':ratio,'candidate_faster':c<b,'candidate_worse':c>b})
    by_order={}
    for order,label in ((['baseline','candidate'],'AB'),(['candidate','baseline'],'BA')):
        values=[r['candidate_over_baseline'] for r in rows if r['order']==order]
        require(values,'missing AB/BA order')
        by_order[label]=median(values)
    return {'pairs':rows,'median_candidate_over_baseline':median(r['candidate_over_baseline'] for r in rows),
            'AB_median':by_order['AB'],'BA_median':by_order['BA'],
            'candidate_faster_pairs':sum(r['candidate_faster'] for r in rows),
            'candidate_worse_pairs':sum(r['candidate_worse'] for r in rows)}

def stable_regression(stats, threshold, protection):
    if protection:
        require(len(stats['pairs'])==2,'protection requires exactly two pairs')
        return all(r['candidate_over_baseline']>threshold for r in stats['pairs'])
    return all(stats[k]>threshold for k in ('median_candidate_over_baseline','AB_median','BA_median'))

def evaluate_screen(plan, groups, *, verified_gate_checks_passed, mapping_erratum):
    """Return a screen decision; this function does not prove gate authenticity.

    False gate checks produce an explicit inconclusive result. Invalid or missing
    numerical evidence raises ValueError; a raw-reader wrapper must preserve that
    failure and keep R, rather than retrying or dropping a pair.
    """
    require(type(verified_gate_checks_passed) is bool,'gate condition must be explicit bool')
    require(hashlib.sha256(encode(mapping_erratum)).hexdigest()==ERRATUM_CANONICAL_SHA,
            'field mapping erratum differs')
    expected=expected_groups(plan)
    require(isinstance(groups,dict) and all(type(key) is tuple and len(key)==3 and
            type(key[0]) is str and type(key[1]) is int and type(key[2]) is str for key in groups),
            'group keys must be (reference string, integer CPU count, scenario string)')
    require(set(groups)==set(expected),'group inventory differs from frozen 24-pair plan')
    summaries={}; negatives=[]
    for key, wanted in expected.items():
        pairs=groups[key]
        require(isinstance(pairs,list) and len(pairs)==len(wanted),'pair count differs')
        for pair, entry in zip(pairs,wanted):
            require(type(pair.get('pair_index')) is int and
                    pair['pair_index']==entry['pair_index'] and pair.get('order')==entry['order'],
                    'pair identity/order differs')
            require(isinstance(pair.get('metrics'),dict),'metrics missing')
            require(all(type(name) is str for name in pair['metrics']),'metric names must be strings')
            require(not any('first_query_cpu' in name for name in pair['metrics']),'first-query CPU was not measured')
            if key[2]=='spill_high_q16':
                require('ingest_cpu_usec' not in pair['metrics'],'spill has no pure ingest CPU observation')
            if key[2]=='wide768':
                require('max_process_rss_bytes' not in pair['metrics'],'wide has no RSS observation')
        times,memories=guard_metric_names(key[2])
        stats={name:summarize_metric(pairs,name) for name in times+memories}
        label=':'.join(map(str,key));summaries[label]=stats
        for metric,record in stats.items():
            for row in record['pairs']:
                if row['candidate_worse']:
                    negatives.append({'group':label,'metric':metric,**row})
    ee={}
    for cpu in (1,2):
        stats=summaries['ee:'+str(cpu)+':standard10k']
        first=stats['first_lifecycle_wall_seconds'];query=stats['first_1000_queries_wall_seconds']
        conditions={
            'first_cycle_median_at_most_0_98':first['median_candidate_over_baseline']<=0.98,
            'at_least_five_of_six_first_cycles_faster':first['candidate_faster_pairs']>=5,
            'AB_first_cycle_median_below_one':first['AB_median']<1,
            'BA_first_cycle_median_below_one':first['BA_median']<1,
            'query_wall_median_at_most_one':query['median_candidate_over_baseline']<=1,
        }
        ee[str(cpu)]={'conditions':conditions,'passed':all(conditions.values())}
    blocks=[]
    for key in expected:
        if key[0]!='retained':
            continue
        times,memories=guard_metric_names(key[2]);label=':'.join(map(str,key))
        for metric in times+memories:
            threshold=1.05 if metric in memories else 1.02
            if stable_regression(summaries[label][metric],threshold,key[2]!='standard10k'):
                blocks.append({'group':label,'metric':metric,'threshold':threshold,
                               'rule':'both individual pair ratios strictly above' if key[2]!='standard10k' else 'overall, AB and BA medians all strictly above'})
    eligible=verified_gate_checks_passed and all(x['passed'] for x in ee.values()) and not blocks
    if not verified_gate_checks_passed:
        status='inconclusive_gates_not_verified_keep_R'
    elif blocks:
        status='screen_rejected_R_regression_keep_R'
    elif not all(x['passed'] for x in ee.values()):
        status='screen_EE_primary_criteria_not_met_keep_R'
    else:
        status='screen_passed_eligible_for_one_independent_confirmation_only'
    return {'status':status,'preregistration_sha256':PREREG_SHA,'field_mapping_erratum_sha256':ERRATUM_SHA,
            'erratum_timing':'after CI launch, before viewing results; original plan unchanged','numerical_pairs':24,
            'gate_verification_claim_supplied_by_caller':verified_gate_checks_passed,
            'EE_primary':ee,'R_guard_triggers':blocks,'eligible_for_one_confirmation':eligible,
            'automatic_adoption':False,'selected_runtime_change_authorized':False,
            'formal_submission_authorized':False,'formal_score_prediction':None,'statistical_significance_claim':False,
            'guard_metrics':summaries,'all_guard_adverse_pairs':negatives,
            'not_measured':['first-request CPU','spill pure ingest CPU','wide process RSS'],
            'scope':'Numerical screen only, not independent identity/oracle/resource proof. Complete phase-level and other negative evidence remains in the independently verified raw-reader analysis; no candidate is automatically adopted.'}
