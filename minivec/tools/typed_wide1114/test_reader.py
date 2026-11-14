#!/usr/bin/env python3
"""Synthetic and historical reader checks only; does not execute a database."""
import argparse
import copy
import json
from pathlib import Path
import run_ablation as driver

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--historical',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    driver.require(not args.output.exists(),'refuse overwrite')
    raw=json.loads(args.historical.read_bytes())
    recomputed=driver.numerical_guards(raw['arms'])
    driver.require(recomputed['guard_metrics']==raw['guard_metrics'],'original31 summaries changed')
    original=driver.observed_metrics
    arms={str(cpu):{'data':{'status':'passed','plan':driver.plan(cpu),'pairs':[]}} for cpu in (1,2)}
    for cpu in (1,2):
        for item in driver.plan(cpu):
            arms[str(cpu)]['data']['pairs'].append(dict(item,baseline={'role':'baseline'},candidate={'role':'candidate'}))
    def observed(result,scenario):
        times,mem=driver.rules.guard_metric_names(scenario)
        values={name:1.0 for name in times+mem}
        if result['role']=='candidate' and scenario=='wide768':
            values.update(driver.PLAN['thresholds']['wide_per_pair_required_max'])
        values.update(result.get('override',{}))
        for name in result.get('remove',[]):values.pop(name)
        return values
    driver.observed_metrics=observed
    try:
        driver.require(driver.numerical_guards(arms)['numerical_ablation_guards_passed'],'inclusive thresholds rejected')
        blocked=[]
        for metric,threshold in driver.PLAN['thresholds']['wide_per_pair_required_max'].items():
            for pair_index in (0,1):
                trial=copy.deepcopy(arms)
                wide=[p for p in trial['2']['data']['pairs'] if p['scenario']=='wide768']
                wide[pair_index]['candidate']['override']={metric:threshold+0.000001}
                driver.require(not driver.numerical_guards(trial)['numerical_ablation_guards_passed'],'wide adverse pair accepted')
                blocked.append(metric+':pair'+str(pair_index))
        for metric,ratio in [('first_1000_queries_wall_seconds',1.020001),('memory_peak_bytes',1.050001)]:
            trial=copy.deepcopy(arms)
            for pair in trial['1']['data']['pairs']:
                if pair['scenario']=='standard10k':pair['candidate']['override']={metric:ratio}
            driver.require(not driver.numerical_guards(trial)['numerical_ablation_guards_passed'],'default stable regression accepted')
            blocked.append('default:'+metric)
        rejected=[]
        for label in ('missing_cpu','missing_pair','wrong_plan','missing_metric','nonfinite'):
            trial=copy.deepcopy(arms)
            if label=='missing_cpu':trial.pop('2')
            elif label=='missing_pair':trial['1']['data']['pairs'].pop()
            elif label=='wrong_plan':trial['1']['data']['plan']=[]
            elif label=='missing_metric':trial['1']['data']['pairs'][0]['candidate']['remove']=['memory_peak_bytes']
            else:trial['1']['data']['pairs'][0]['candidate']['override']={'first_query_wall_seconds':float('nan')}
            try:driver.numerical_guards(trial)
            except (ValueError,KeyError):rejected.append(label)
            else:raise AssertionError('invalid evidence accepted: '+label)
    finally:driver.observed_metrics=original
    result={'status':'reader_checks_passed_not_measurement','historical_sha256':driver.sha(args.historical),
            'original_metric_summaries_equal':31,'inclusive_wide_thresholds_accepted':True,
            'regressions_blocked':blocked,'invalid_evidence_rejected':rejected,
            'new_database_runs':0,'release_gate_passed':False}
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
    print(json.dumps(result))

if __name__=='__main__':main()
