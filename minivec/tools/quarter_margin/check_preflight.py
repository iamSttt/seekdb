"""Structural work-only checks; deliberately does not compile or run native code."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
root=Path(__file__).resolve().parent
checks=[]
def need(value,name):
    assert value,name
    checks.append(name)
for name in ('make_fixtures.py','run_linux.py','check_preflight.py'):
    ast.parse((root/name).read_text());checks.append('ast:'+name)
old=(root/'reference_distance.h').read_text();new=(root/'candidate_distance.h').read_text()
need(hashlib.sha256((root/'reference_distance.h').read_bytes()).hexdigest()=='7094c0892de1b56377a65675a119a942cd07959f22e128673738c6c953fc1fe9','canonical_git_reference')
start='inline double distance_scalar(';stop='// Conservative rejection'
a=old[old.index(start):old.index(stop,old.index(start))]
b=new[new.index(start):new.index(stop,new.index(start))]
addition='''      // Keep the successful original path unchanged. In the verified x87
      // environment, both rounded products are covered; inexact status may change.
      if(distance_detail::tighter_long_double_environment()) {
        const long double tighter_error=error*(0.25L+0x1p-30L);
        if(sum-lower>tighter_error && upper-sum>tighter_error)return result;
      }
'''
need(b.count(addition)==1 and b.replace(addition,'')==a,'original_scalar_verbatim_after_remove_only_rescue')
need(b.index('if(sum-lower>error && upper-sum>error)return result;')<b.index('tighter_long_double_environment()'),'guard_only_after_original_failed_certificate')
need('0x0f3fu)==0x033fu' in new and '0xffc0u)==0x1f80u' in new,'strict_runtime_control_masks')
need(all(x in new for x in ('LDBL_MANT_DIG == 64','LDBL_MAX_EXP == 16384','LDBL_MIN_EXP == -16381','FLT_RADIX == 2','__SIZEOF_SIZE_T__ == 8')),'format_guards')
need('diagnostic_' not in old+new,'no_production_counter')
spec=importlib.util.spec_from_file_location('tighter_runner',root/'run_linux.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
for label,text in (('old',old),('new',new)):
    shadow=runner.shadow_header(text)
    need(shadow.count('++distance_detail::diagnostic_calls;')==1 and shadow.count('++distance_detail::diagnostic_integer;')==1,'shadow_calls_fallback:'+label)
    need(shadow.count('++distance_detail::diagnostic_old_cert;')==1,'shadow_oldcert:'+label)
    need(shadow.count('++distance_detail::diagnostic_tight_rescue;')==(label=='new'),'shadow_rescue:'+label)
report={'status':'passed','checks':checks,'checks_count':len(checks),'native_compilations':0,'native_executions':0,'database_runs':0,
        'scope':'source/AST/isolated shadow-insertion checks only'}
(root/'preflight.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'status':'passed','checks_count':len(checks),'native_executions':0}))
