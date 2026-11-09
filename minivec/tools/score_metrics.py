#!/usr/bin/env python3
"""Compute E from official T(seconds), B(bytes), M(bytes), never from RSS proxies.

Input: {"baseline_rounds": [{"write":{"T":...,"B":...,"M":...},
"query":{...}}, ...3], "candidate_rounds": [...3]}.
No rank or contest score can be inferred without the actual leaderboard.
"""
import json
import math
import statistics
import sys
from decimal import Decimal, ROUND_HALF_UP


def score(data):
    base, candidate = data['baseline_rounds'], data['candidate_rounds']
    if len(base)!=3 or len(candidate)!=3: raise ValueError('require exactly three complete rounds')
    for r in base+candidate:
        for part in ['write','query']:
            for key in ['T','B','M']:
                v=r[part][key]
                if not isinstance(v,(int,float)) or not math.isfinite(v) or v<0:
                    raise ValueError('missing, invalid or negative metric')
        if r.get('valid',True) is not True: raise ValueError('an invalid round disqualifies performance')
    baseline={p:{k:statistics.median(r[p][k] for r in base) for k in ['T','B','M']} for p in ['write','query']}
    values=[]
    for round in candidate:
        components={}
        for p in ['write','query']:
            total=0.
            for k,weight,eps in [('T',.6,.001),('B',.2,1048576),('M',.2,1048576)]:
                ratio=(baseline[p][k]+eps)/(round[p][k]+eps)
                total+=weight*ratio/(1+ratio)
            components[p]=total
        values.append((components['write']+2*components['query'])/3)
    return {'baseline_medians':baseline,'round_E':values,
            'median_E_six_decimals':str(Decimal(str(statistics.median(values))).quantize(Decimal('.000001'),rounding=ROUND_HALF_UP)),
            'contest_score':None,'reason':'E ranks competitors; leaderboard is required for points'}


if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('usage: score_metrics.py official-metrics.json')
    print(json.dumps(score(json.load(open(sys.argv[1],encoding='utf-8'))),indent=2))
