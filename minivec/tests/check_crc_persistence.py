#!/usr/bin/env python3
"""Real old/new WAL+snapshot interchange, byte compatibility and corrupt-tail checks.

Requires two fixture writers built from the SAME fixture source against old/new
storage, and two actual protocol binaries. No formal submission or score claim.
"""
import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


def require(ok, why):
    if not ok: raise AssertionError(why)
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def query(binary, directory, expected_keys=(1,2,7,15,40)):
    requests=[{'id':1,'op':'execute','sql':'SELECT id,tag,label,v FROM crc_rows ORDER BY id;'},
              {'id':2,'op':'shutdown'}]
    run=subprocess.run([str(binary),'--data',str(directory)],input=''.join(json.dumps(x)+'\n' for x in requests),
                       text=True,capture_output=True,timeout=30)
    messages=[json.loads(line) for line in run.stdout.splitlines()]
    require(run.returncode==0 and len(messages)==3 and messages[0]=={'type':'ready'},'protocol/read failure')
    require(messages[1].get('ok') is True and messages[2].get('ok') is True,'query/shutdown failure')
    expected=[[key,key%2,"quoted '[1,2]' utf8 汉字 "+'x'*(17+key),[float(key),-0.0,0.125,-3.5]]
              for key in expected_keys]
    require(messages[1]['columns']==['id','tag','label','v'] and messages[1]['rows']==expected,
            'uncommitted tail, missing row, or field content differs')
    return messages[1],run.stderr
def records(raw):
    items=[];at=0
    while at<len(raw):
        require(at+8<=len(raw),'writer produced truncated header')
        length,crc=struct.unpack_from('<II',raw,at)
        require(0<length and at+8+length<=len(raw),'writer produced bad length')
        body=raw[at+8:at+8+length]
        require(zlib.crc32(body)==crc,'writer differs from independent zlib IEEE CRC')
        items.append((at,length,body[0]));at+=8+length
    return items

def committed_insert(raw, items, key):
    """Locate by independently decoded content and verified transaction framing.

    No filename, record ordinal or hard-coded file offset chooses the mutation.
    The fixture deliberately writes row 40 in its own committed transaction.
    """
    matches=[]
    for ordinal,(at,length,kind) in enumerate(items):
        if kind!=1: continue
        body=raw[at+8:at+8+length];pos=0
        def take(n):
            nonlocal pos
            require(n>=0 and pos+n<=len(body),'truncated INSERT fixture field')
            value=body[pos:pos+n];pos+=n;return value
        def u32(): return struct.unpack('<I',take(4))[0]
        require(take(1)==b'\x01','INSERT kind differs')
        flags=take(1)[0];lsn=struct.unpack('<Q',take(8))[0]
        table=take(u32()).decode('utf8')
        count=u32();values=[]
        require(count<=4096,'unexpected fixture column count')
        for _ in range(count):
            tag=take(1)[0]
            if tag==1: values.append(struct.unpack('<q',take(8))[0])
            elif tag==2: values.append(take(u32()).decode('utf8'))
            elif tag==3:
                dimension=u32();require(dimension<=1048576,'unexpected fixture dimension')
                values.append(list(struct.unpack('<'+'f'*dimension,take(dimension*4))))
            else: raise AssertionError('unknown fixture value tag')
        require(pos==len(body),'unparsed INSERT fixture bytes')
        if table!='crc_rows' or not values or values[0]!=key: continue
        require(flags==1 and ordinal>0 and ordinal+1<len(items),'target INSERT lacks transaction framing')
        previous=items[ordinal-1];following=items[ordinal+1]
        require(previous[2]==16 and following[2]==17,'target INSERT is not an isolated committed transaction')
        begin=raw[previous[0]+8:previous[0]+8+previous[1]]
        commit=raw[following[0]+8:following[0]+8+following[1]]
        require(len(begin)==len(commit)==14 and begin[1]==commit[1]==1 and
                begin[10:]==commit[10:]==b'\x00'*4,'unexpected BEGIN/COMMIT fixture payload')
        require(struct.unpack_from('<Q',begin,2)[0]+1==lsn and
                struct.unpack_from('<Q',commit,2)[0]==lsn+1,'target transaction LSN sequence differs')
        require(values==[key,key%2,"quoted '[1,2]' utf8 汉字 "+'x'*(17+key),
                         [float(key),-0.0,0.125,-3.5]],'target INSERT payload differs')
        matches.append((at,length,kind))
    require(len(matches)==1,'expected one independently located committed INSERT')
    return matches[0]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','baseline-writer','candidate-writer','output'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();require(not a.output.exists(),'refuse overwrite')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    fixture_root=a.output.with_suffix('')
    require(not fixture_root.exists() and fixture_root!=a.output,'fresh .json output and fixture directory required')
    fixture_root.mkdir()
    result={'status':'running','checks':[],'formal_score':None,'performance_measurements':0,
            'fixture_directory':str(fixture_root.resolve()),
            'binaries':{k:{'path':str(getattr(a,k).resolve()),'sha256':sha(getattr(a,k))}
                        for k in ('baseline','candidate','baseline_writer','candidate_writer')}}
    try:
        # Preserve original, cross-read and damaged fixtures even on a failure.
        with nullcontext(fixture_root) as temporary:
            root=Path(temporary)
            for mode in ('wal','snapshot'):
                originals={}
                for producer in ('baseline','candidate'):
                    directory=root/(mode+'-'+producer)
                    writer=getattr(a,producer+'_writer').resolve()
                    subprocess.run([str(writer),mode,str(directory)],check=True,capture_output=True,timeout=30)
                    originals[producer]=directory
                    records((directory/'data.log').read_bytes())
                    if mode=='snapshot':
                        raw=(directory/'snapshot.bin').read_bytes()
                        require(zlib.crc32(raw[:-4])==struct.unpack('<I',raw[-4:])[0],'snapshot differs from zlib IEEE CRC')
                files=('data.log',) if mode=='wal' else ('data.log','snapshot.bin','snapshot.lsn')
                for name in files:
                    require((originals['baseline']/name).read_bytes()==(originals['candidate']/name).read_bytes(),
                            'old/new durable bytes differ: '+mode+'/'+name)
                    result['checks'].append({'kind':'identical_durable_bytes','mode':mode,'file':name,
                                              'sha256':sha(originals['baseline']/name)})
                expected=None
                for producer in ('baseline','candidate'):
                    for consumer in ('baseline','candidate'):
                        dest=root/(mode+'-'+producer+'-read-'+consumer)
                        shutil.copytree(originals[producer],dest)
                        response,_=query(getattr(a,consumer).resolve(),dest)
                        if expected is None:expected=response
                        require(response==expected,'cross-version response differs')
                        result['checks'].append({'kind':'old_new_interchange','mode':mode,'producer':producer,'consumer':consumer})
                # A bad CRC in an acknowledged transaction must exclude that
                # transaction even though its COMMIT marker remains intact.
                # Corrupting only an uncommitted tail would not detect a reader
                # which skipped CRC checks but still discarded pending rows.
                raw=(originals['baseline']/'data.log').read_bytes()
                items=records(raw);last=items[-1];require(last[2]==1,'last record expected INSERT')
                target=committed_insert(raw,items,40)
                corrupt=bytearray(raw);corrupt[target[0]+8+target[1]-1]^=1
                for label,damaged,expected_keys in (
                        ('bad_committed_crc',bytes(corrupt),(1,2,7,15)),
                        ('torn_tail',raw[:-3],(1,2,7,15,40))):
                    responses=[]
                    for consumer in ('baseline','candidate'):
                        dest=root/(mode+'-'+label+'-'+consumer);shutil.copytree(originals['baseline'],dest)
                        (dest/'data.log').write_bytes(damaged)
                        response,_=query(getattr(a,consumer).resolve(),dest,expected_keys);responses.append(response)
                    require(responses[0]==responses[1],'damaged WAL recovery differs across versions')
                    if label=='torn_tail': require(responses[0]==expected,'torn-tail recovery changed')
                    result['checks'].append({'kind':label,'mode':mode,'expected_visible_ids':list(expected_keys)})
                if mode=='snapshot':
                    for consumer in ('baseline','candidate'):
                        dest=root/('bad-snapshot-'+consumer);shutil.copytree(originals['baseline'],dest)
                        bad=bytearray((dest/'snapshot.bin').read_bytes());bad[-5]^=1;(dest/'snapshot.bin').write_bytes(bad)
                        run=subprocess.run([str(getattr(a,consumer).resolve()),'--data',str(dest)],
                                           input='',text=True,capture_output=True,timeout=30)
                        require(run.returncode!=0 and 'CRC mismatch' in run.stderr and not run.stdout,'corrupt snapshot accepted')
                        result['checks'].append({'kind':'bad_snapshot_crc_rejected','consumer':consumer})
        for name,identity in result['binaries'].items():
            require(sha(getattr(a,name))==identity['sha256'],'reader or fixture writer changed during checks')
        result['status']='passed'
    except BaseException as exc:
        result.update(status='failed',error=type(exc).__name__+': '+str(exc));raise
    finally:
        a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
    print(json.dumps({'status':result['status'],'checks':len(result['checks']),'performance_measurements':0}))
if __name__=='__main__':main()
