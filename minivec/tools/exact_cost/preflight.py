"""Small pure-Python preparation/reader checks; no compiler or DB execution."""
import ast, copy, importlib.util, json, pathlib, sys
HERE=pathlib.Path(__file__).resolve().parent
def module(name):
    spec=importlib.util.spec_from_file_location(name,HERE/(name+'.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def main():
    run=module('run');prepare=module('prepare');checks=[]
    def ok(name,condition):
        if not condition:raise AssertionError(name)
        checks.append(name)
    def reject(name,fn):
        try:fn()
        except (ValueError,KeyError):checks.append(name);return
        raise AssertionError(name+' accepted invalid input')
    for name in ('run','prepare','preflight'):ast.parse((HERE/(name+'.py')).read_text());checks.append(name+'_syntax')
    shadow=pathlib.Path(sys.argv[1]);proof=json.loads((shadow/'source-proof.json').read_text())
    ok('archive_identity',proof['archive_sha256']==prepare.RAW_SHA and proof['retained_binary_sha256']==prepare.R_BINARY)
    ok('all20_original_sources',len(proof['original_source_sha256'])==20)
    ok('canonical19_match_one_disclosed',set(proof['canonical_R_comparison']['differences'])=={'exec.cpp'})
    changed={k for k in proof['original_source_sha256'] if proof['original_source_sha256'][k]!=proof['instrumented_source_sha256'][k]}
    ok('only_five_instrumented_sources',changed=={'main.cpp','distance.h','quantized_bound.h','sql.cpp','db.cpp'})
    ok('no_pgo_flags',not any('profile' in x for x in proof['compiler_flags']) and proof['pgo'] is False)
    ok('scalar_unsampled','k==Scalar?0u:' in (HERE/'cost_diagnostic.h').read_text())
    record={'sequence':1,'thread_cpu_ns':20000,'metrics':{k:dict(calls=0,units=0,samples=0,sample_cpu_ns=0,sample_stride=v) for k,v in run.METRICS.items()},
      'events':{k:0 for k in run.EVENTS}}
    record['metrics']['scalar_distance']['calls']=2;record['metrics']['long_double_certificate']['calls']=2
    record['metrics']['integer_exact']['calls']=1;record['events']['scalar_to_integer']=1;record['events']['long_double_certified']=1
    requests=[{'sequence':1,'phase':'unfiltered_query'}]
    result=run.summarize_diagnostic([record],requests)
    ok('balanced_fixture',result['unfiltered_query']['metrics']['scalar_distance']['calls']==2)
    def changed_record(mutator):
        r=copy.deepcopy(record);mutator(r);return lambda:run.summarize_diagnostic([r],requests)
    reject('missing_record',lambda:run.summarize_diagnostic([],requests))
    reject('wrong_sequence',changed_record(lambda r:r.update(sequence=2)))
    reject('missing_metric',changed_record(lambda r:r['metrics'].pop('integer_exact')))
    reject('extra_event',changed_record(lambda r:r['events'].update(invented=0)))
    reject('wrong_stride',changed_record(lambda r:r['metrics']['long_double_certificate'].update(sample_stride=1)))
    reject('negative_event',changed_record(lambda r:r['events'].update(integer_nonfinite=-1)))
    reject('unbalanced_scalar',changed_record(lambda r:r['events'].update(scalar_to_integer=2)))
    reject('unbalanced_quant',changed_record(lambda r:r['events'].update(int8_kernel_calls=1)))
    reject('sampled_scalar',changed_record(lambda r:r['metrics']['scalar_distance'].update(samples=1)))
    reject('nonzero_unsampled_cpu',changed_record(lambda r:r['metrics']['integer_exact'].update(sample_cpu_ns=1)))
    reject('ambiguous_anchor',lambda:prepare.replace('aa','a','b'))
    result={'status':'passed','checks':checks,'count':len(checks),'scope':'pure Python/source preparation only; no Linux DB, C++ compilation or native timing executed'}
    (HERE/'preflight.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
