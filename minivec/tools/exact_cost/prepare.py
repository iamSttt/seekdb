#!/usr/bin/env python3
"""Make a new, independent R shadow build; never edit a repository input."""
import argparse, difflib, hashlib, json, os, pathlib, subprocess, zipfile

RAW_SHA='88e4a73af9d333b08a6db817a78c7cb06357fcfddc05eee1b31c2a065aebfef2'
RAW_BYTES=49685962
R_COMMIT='e020fdeb67dd8b64b77f0f8c2905f137f0112bba'
R_BINARY='acddca115ab183bfc6419e27c9e3383cfd1a9046e3897e5ea516cd7bc0f29f54'
CANONICAL_R='33a52a7dd4426af059ec2334b7c93a07c38c93c9'
HERE=pathlib.Path(__file__).resolve().parent
def sha(b):return hashlib.sha256(b).hexdigest()
def need(ok,message):
    if not ok:raise ValueError(message)
def replace(s,a,b):
    need(s.count(a)==1,'patch anchor must occur once: '+a[:100])
    return s.replace(a,b)

def patch(name,s):
    s=s.replace('\r\n','\n')
    if name in ('main.cpp','distance.h','quantized_bound.h','sql.cpp','db.cpp'):
        s='#include "cost_diagnostic.h"\n'+s
    if name=='main.cpp':
        a='static bool json_request(Database& db, const std::string& line, int* exit_code) {'
        s=replace(s,a,a+'\n  mvdb_cost::Request diagnostic_request;')
    if name=='sql.cpp':
        a='               bool retain_raw) {'
        s=replace(s,a,a+'\n  mvdb_cost::Call diagnostic_parse(mvdb_cost::Parse,sql.size());')
    if name=='db.cpp':
        a='inline uint32_t crc32_upd(uint32_t crc, const void* p, size_t n) {'
        s=replace(s,a,a+'\n  mvdb_cost::Call diagnostic_crc(mvdb_cost::Crc,n);')
    if name=='distance.h':
        a='inline double distance_exact(const float* a,const float* b,size_t n) {'
        s=replace(s,a,a+'\n  mvdb_cost::Call diagnostic_exact(mvdb_cost::Integer,n);')
        a='    if(ex==255 || ey==255) {'
        s=replace(s,a,a+'\n      mvdb_cost::event(mvdb_cost::ExactNonfinite);')
        a='inline double distance_scalar(const float* a,const float* b,size_t n) {'
        s=replace(s,a,a+'\n  mvdb_cost::Call diagnostic_scalar(mvdb_cost::Scalar,n);')
        a='  if(n<=uint64_t(1)<<32) {'
        s=replace(s,a,a+'\n    mvdb_cost::Call diagnostic_long_double(mvdb_cost::LongDouble,n);')
        s=replace(s,'      if(sum-lower>error && upper-sum>error)return result;',
          '      if(sum-lower>error && upper-sum>error){mvdb_cost::event(mvdb_cost::Certified);return result;}')
        s=replace(s,'  return distance_exact(a,b,n);','  mvdb_cost::event(mvdb_cost::ScalarFallback);\n  return distance_exact(a,b,n);')
        a='inline bool distance_definitely_greater_double(const float* a,const float* b,size_t n,\n                                        double threshold) {'
        s=replace(s,a,a+'\n  mvdb_cost::Call diagnostic_double(mvdb_cost::DoubleBound,n);')
        s=replace(s,'  return lower>threshold;','  const bool rejected=lower>threshold;\n  if(rejected)mvdb_cost::event(mvdb_cost::DoubleReject);\n  return rejected;')
        a='  bool definitely_greater(const float* a,const float* b,double threshold) const {'
        s=replace(s,a,a+'\n    mvdb_cost::Call diagnostic_float(mvdb_cost::FloatBound,n_);')
        s=replace(s,'    if(!enabled_ || !std::isfinite(threshold) || threshold<0)return false;',
          '    if(!enabled_ || !std::isfinite(threshold) || threshold<0){mvdb_cost::event(mvdb_cost::FloatInvalid);return false;}')
        s=replace(s,'      const double sum=kernel_(a,b,n_);',
          '      mvdb_cost::event(mvdb_cost::FloatKernel);\n      const double sum=kernel_(a,b,n_);')
        s=replace(s,'        return sum>cutoff_;',
          '        const bool rejected=sum>cutoff_;\n        if(rejected)mvdb_cost::event(mvdb_cost::FloatReject);\n        return rejected;')
    if name=='quantized_bound.h':
        a='    if(!enabled_ || !row || !std::isfinite(threshold) || threshold<0)return false;'
        s=replace(s,a,'    mvdb_cost::Call diagnostic_quant(mvdb_cost::QuantBound,n_);\n'+
          '    if(!enabled_ || !row || !std::isfinite(threshold) || threshold<0){mvdb_cost::event(mvdb_cost::QuantInvalid);return false;}')
        s=replace(s,'    if(!usable_cap_)return false;',
          '    if(!usable_cap_){mvdb_cost::event(mvdb_cost::QuantCapUnusable);return false;}')
        s=replace(s,'    return static_cast<double>(kernel_(row,query_data_,n_))>cap_;',
          '    mvdb_cost::event(mvdb_cost::QuantKernel);\n    const bool rejected=static_cast<double>(kernel_(row,query_data_,n_))>cap_;\n'+
          '    if(rejected)mvdb_cost::event(mvdb_cost::QuantReject);\n    return rejected;')
    return s

def prepare(repo,output):
    need(not output.exists(),'output must be a new directory')
    raw=repo/'validation/iteration4-bulk-loop-raw.zip';b=raw.read_bytes()
    need(len(b)==RAW_BYTES and sha(b)==RAW_SHA,'fixed R archive bytes mismatch')
    import io
    with zipfile.ZipFile(io.BytesIO(b)) as z:
        names=z.namelist();need(len(names)==len(set(names)),'duplicate archive names')
        for name in names:
            p=pathlib.PurePosixPath(name)
            need(not p.is_absolute() and '..' not in p.parts and '\\' not in name,'unsafe archive member')
        inventory=json.loads(z.read('hashes.json'))['artifacts']
        indexed={x['path']:x for x in inventory};need(len(indexed)==len(inventory),'duplicate manifest keys')
        def read(name):
            b=z.read(name);m=indexed[name]
            need(len(b)==m['bytes'] and sha(b)==m['sha256'],'archive manifest mismatch '+name);return b
        context=json.loads(read('context.json'));proof=context['profile_source_proof']['baseline']
        need(proof['source_commit']==R_COMMIT and proof['binary_sha256']==R_BINARY,'wrong retained identity')
        flags=proof['effective_compiler_flags']
        need(proof['feature_flags']==['-DMVDB_ENABLE_SPILL_FILTER','-DMVDB_ENABLE_TYPED_JSON','-DMVDB_TYPED_WIDE_VECTOR_ONLY','-DMVDB_MULTI_POSTING_REUSE_ONLY'],'unexpected R flags')
        output.mkdir(parents=True);(output/'src').mkdir();(output/'third_party').mkdir()
        patches=[];original={};modified={}
        for name,digest in sorted(proof['source_sha256'].items()):
            need(pathlib.PurePosixPath(name).name==name,'unexpected source name')
            b=read('baseline-pgo/built-source/'+name);need(sha(b)==digest,'source proof mismatch '+name)
            original[name]=sha(b)
            old=b.decode();new=patch(name,old)
            # Only the five explicitly instrumented files change; untouched bytes stay exact.
            out=new.encode() if name in ('main.cpp','distance.h','quantized_bound.h','sql.cpp','db.cpp') else b
            (output/'src'/name).write_bytes(out);modified[name]=sha(out)
            if out!=b:patches.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='R/'+name,tofile='diagnostic/'+name))
        third=read('baseline-pgo/built-third-party/json.hpp');(output/'third_party/json.hpp').write_bytes(third)
        program=read('baseline-pgo/minivec');need(sha(program)==R_BINARY,'R ELF mismatch')
        (output/'retained-minivec').write_bytes(program);os.chmod(output/'retained-minivec',0o755)
    header=(HERE/'cost_diagnostic.h').read_bytes();(output/'src/cost_diagnostic.h').write_bytes(header)
    (output/'timer_overhead.cpp').write_bytes((HERE/'timer_overhead.cpp').read_bytes())
    (output/'instrumentation.patch').write_text(''.join(patches),encoding='utf-8')
    canonical={}
    for name in original:
        b=subprocess.check_output(['git','-c','safe.directory='+repo.as_posix(),'-C',str(repo),
          'show',CANONICAL_R+':minivec/src/'+name])
        canonical[name]=sha(b)
    differences={n:{'archived_sha256':original[n],'canonical_sha256':canonical[n]} for n in original if original[n]!=canonical[n]}
    need(differences=={'exec.cpp':{'archived_sha256':'f33d42c27035cb732a8ce08c75ae91510970c8feecb7ba1a3a0181c367aca2cf',
      'canonical_sha256':'366eb7a8f0bb22ae113aa9e841443f8913b4e876cc6599183bd6450f914137e5'}},'unexpected archive/canonical R source difference')
    canonical_diff=subprocess.check_output(['git','-c','safe.directory='+repo.as_posix(),'-C',str(repo),
      'diff',R_COMMIT,CANONICAL_R,'--','minivec/src/exec.cpp'])
    (output/'archive-vs-canonical-exec.diff').write_bytes(canonical_diff)
    manifest={'scope':'diagnostic-only; historical R source, no PGO rebuild or release candidate',
      'archive_sha256':RAW_SHA,'archive_bytes':RAW_BYTES,'runtime_source_commit':R_COMMIT,
      'retained_binary_sha256':R_BINARY,'original_source_sha256':original,'instrumented_source_sha256':modified,
      'canonical_R_comparison':{'commit':CANONICAL_R,'all_20_shared_sources_sha256':canonical,'differences':differences,
        'diff_sha256':sha(canonical_diff),'scope':'19/20 byte-identical. exec.cpp adds disabled bulk-vector branch and compiled-out JSON counters in canonical R. Diagnostic uses the archived actual R source, not a byte-identical canonical checkout claim.'},
      'diagnostic_header_sha256':sha(header),'third_party_json_sha256':sha(third),
      'compiler_flags':flags+['-DMVDB_COST_DIAGNOSTIC'],
      'pgo':False,'timing_scope':'instrumented, inclusive/nested CPU samples; no official performance claim',
      'sampling':{'distance_stride':256,'scalar_counter_only':True,'crc_stride':16,'parse_stride':1,'timer_overhead_subtracted':False},
      'required_environment':{'MVDB_QUERY_THREADS':'1','MVDB_PARSE_THREADS':'1'},
      'tools':{p.name:sha(p.read_bytes()) for p in HERE.glob('*') if p.is_file() and p.suffix in ('.py','.h','.cpp','.sh')}}
    (output/'source-proof.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--repo',type=pathlib.Path,required=True);a.add_argument('--output',type=pathlib.Path,required=True)
    args=a.parse_args();m=prepare(args.repo.resolve(),args.output.resolve());print(json.dumps({'status':'prepared','source_files':len(m['original_source_sha256']),'runtime_source_commit':R_COMMIT}))
