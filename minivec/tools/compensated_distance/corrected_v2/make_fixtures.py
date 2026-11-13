"""Independent integer binary32 squared-distance oracle. No native execution."""
import argparse,hashlib,json,random,struct
from pathlib import Path

def integer(bits):
    exponent=(bits>>23)&255;fraction=bits&0x7fffff
    assert exponent!=255
    magnitude=((1<<23)|fraction)<<(exponent-1) if exponent else fraction
    return -magnitude if bits>>31 else magnitude

def rounded(s):
    if not s:return 0
    top=s.bit_length()-1;exponent=top-298
    assert -1022<=exponent<=1023
    shift=top-52
    if shift>0:
        significand=s>>shift;tail=s- (significand<<shift);half=1<<(shift-1)
        if tail>half or (tail==half and significand&1):significand+=1
    else:significand=s<<(-shift)
    if significand==(1<<53):significand>>=1;exponent+=1
    assert 1<<52<=significand<1<<53
    return ((exponent+1023)<<52)|(significand- (1<<52))

def admitted(a,b):
    if len(a)<16 or len(a)>4096 or len(a)%4:return False
    for x,y in zip(a,b):
        ex=(x>>23)&255;ey=(y>>23)&255
        if ex==255 or ey==255:return False
        if x&0x7fffffff and y&0x7fffffff and abs(max(ex,1)-max(ey,1))>28:return False
    return True

def bits(x):return struct.unpack('<I',struct.pack('<f',x))[0]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();rng=random.Random(0x6d2c20261114);cases=[]
    def add(a,b):
        assert len(a)==len(b)
        exact=sum((integer(x)-integer(y))**2 for x,y in zip(a,b))
        # Midpoint neighbours mutate their local arrays after this call.
        cases.append((list(a),list(b),rounded(exact),admitted(a,b)))
    for n in (0,1,15,16,17,32,128,512,4096,4097):
        add([0]*n,[0x80000000]*n);add([bits(.75)]*n,[bits(.25)]*n)
    for exponent in range(255):
        for mantissa in (0,1,0x3fffff,0x400000,0x7ffffe,0x7fffff):
            a=(exponent<<23)|mantissa
            for sign in (0,0x80000000):
                add([a|sign]*16,[a^0x80000000^sign]*16)
                other=max(0,exponent-28)<<23|0x7fffff
                add([a|sign]*16,[other]*16)
    for gap in (27,28,29,30):
        for high in (31,100,200,254):
            x=(high<<23)|0x7fffff;y=((high-gap)<<23)|1
            for signs in ((0,0),(0,0x80000000),(0x80000000,0)):
                add([x|signs[0]]*128,[y|signs[1]]*128)
    for exponent in (-100,-50,0,50,100):
        for halves in (2,6):
            a=[bits(2.0**exponent)]+[bits(2.0**(exponent-27))]*halves+[0]*(15-halves)
            b=[0]*16;add(a,b)
            a[-1]=bits(2.0**(exponent-40));add(a,b)
            a[1]-=1;add(a,b)
    for trial in range(1600):
        n=(16,32,64,128,512)[trial%5];a=[];b=[]
        for _ in range(n):
            ex=rng.randrange(255);ey=rng.randrange(max(0,ex-28),min(254,ex+28)+1)
            x=(rng.getrandbits(1)<<31)|(ex<<23)|rng.getrandbits(23)
            y=(rng.getrandbits(1)<<31)|(ey<<23)|rng.getrandbits(23)
            if trial%4==0:y=0
            if trial%4==1:x=bits(rng.uniform(-4,4));y=bits(rng.uniform(-4,4))
            if trial%4==2:y=x^0x80000000
            a.append(x);b.append(y)
        add(a,b)
    for trial in range(24):
        n=4096;a=[];b=[]
        for j in range(n):
            if trial%2:x=bits(rng.uniform(-4,4));y=bits(rng.uniform(-4,4))
            else:x=(rng.randrange(255)<<23)|rng.getrandbits(23);y=0
            a.append(x);b.append(y)
        add(a,b)
    output=bytearray(b'MVDCMP1\0'+struct.pack('<I',len(cases)))
    for a,b,expected,guard in cases:
        output+=struct.pack('<IQI',len(a),expected,int(guard))
        if a:output+=struct.pack('<'+'I'*(len(a)*2),*(a+b))
    args.output.write_bytes(output)
    report={'status':'generated_integer_oracle_fixtures_only','cases':len(cases),
      'admitted_coordinate_shape_cases':sum(c[3] for c in cases),
      'bytes':len(output),'sha256':hashlib.sha256(output).hexdigest(),
      'database_runs':0,'native_executions':0,
      'oracle':'Decode signed binary32 integers in units 2^-149; integer square/sum; integer ties-to-even binary64 rounding; no candidate/reference FP arithmetic.'}
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
if __name__=='__main__':main()
