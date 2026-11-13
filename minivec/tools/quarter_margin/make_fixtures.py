"""Independent integer lattice + Fraction single rounding; no native execution."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import random
import struct

def integer(bits):
    exponent=(bits>>23)&255
    assert exponent!=255
    value=(((bits&0x7fffff)|(0x800000 if exponent else 0)) << (exponent-1 if exponent else 0))
    return -value if bits>>31 else value

def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
    output=bytearray(b'MVTLB1\0\0'+b'\0'*4);counts={};count=0
    def add(a,b,kind=0):
        nonlocal count
        assert len(a)==len(b)
        total=sum((integer(x)-integer(y))**2 for x,y in zip(a,b))
        expected=struct.unpack('<Q',struct.pack('<d',float(Fraction(total,1<<298))))[0]
        output.extend(struct.pack('<IQI',len(a),expected,kind))
        if a:output.extend(struct.pack('<'+'I'*(2*len(a)),*(a+b)))
        counts[str(kind)]=counts.get(str(kind),0)+1;count+=1
    for n in (0,1,2,3,4,5,7,8,15,16,17,31,32,33,127,128,129,511,512,513,4095,4096,4097,65535,65536,65537,262144):
        add([0]*n,[0x80000000]*n)
        add([bits(.75)]*n,[bits(.25)]*n)
    for exponent in range(255):
        for mantissa in (0,1,0x3fffff,0x400000,0x7ffffe,0x7fffff):
            for sign in (0,0x80000000):
                x=(exponent<<23)|mantissa|sign
                add([x]*17,[x^0x80000000]*17)
                add([x]*16,[1 if exponent%2 else 0x7f7fffff]*16)
    # Exact ties plus both sides of a midpoint, including perturbations smaller
    # than the long-double ULP that must remain uncertain.
    for exponent in (-100,-50,0,50,100):
        for n in (16,17,128,129,512,513):
            for small_terms in (2,6):
                a=[bits(2.0**exponent)]+[bits(2.0**(exponent-27))]*small_terms+[0]*(n-1-small_terms)
                add(a,[0]*n,1)
                a[-1]=bits(2.0**(exponent-40));add(a,[0]*n,1)
                a[1]-=1;add(a,[0]*n,1)
        # Analytic rescue fixtures: 1/32 ULP above the first double midpoint
        # at n=128, or 1/8 ULP at n=512. These are correctness fixtures only.
        for n,extras in ((128,2),(512,8)):
            a=[bits(2.0**exponent)]+[bits(2.0**(exponent-27))]*2+[bits(2.0**(exponent-29))]*extras
            a += [0]*(n-len(a));add(a,[0]*n,2)
    rng=random.Random(0x6c42b9e173)
    for trial in range(384):
        n=(1,3,16,17,127,128,129,511,512,513,4096,4097)[trial%12]
        a=[];b=[]
        for _ in range(n):
            x=(rng.getrandbits(1)<<31)|(rng.randrange(255)<<23)|rng.getrandbits(23)
            y=(rng.getrandbits(1)<<31)|(rng.randrange(255)<<23)|rng.getrandbits(23)
            a.append(x);b.append(y)
        add(a,b)
    struct.pack_into('<I',output,8,count)
    args.output.write_bytes(output)
    report={'status':'generated_independent_oracle','cases':count,'kinds':counts,'maximum_dimension':262144,
            'bytes':len(output),'sha256':hashlib.sha256(output).hexdigest(),'seed':'0x6c42b9e173',
            'oracle':'Python arbitrary signed integers in units 2^-149; exact squares and sum; Fraction to binary64 once',
            'native_executions':0}
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
