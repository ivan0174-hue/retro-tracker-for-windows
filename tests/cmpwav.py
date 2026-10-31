#!/usr/bin/env python3
"""Compare two 16-bit stereo WAV renders: duration and RMS-envelope correlation."""
import sys, struct, array, math
def load(p):
    d=open(p,'rb').read()
    i=12; data=None
    while i+8<=len(d):
        cid=d[i:i+4]; sz=struct.unpack('<I',d[i+4:i+8])[0]
        if cid==b'data': data=d[i+8:i+8+sz]; break
        i+=8+sz+(sz&1)
    a=array.array('h'); a.frombytes(data[:len(data)//4*4]); return a
def env(a, win=2400):
    out=[]
    n=len(a)//2
    for s in range(0,n-win+1,win):
        acc=0
        for k in range(s*2,(s+win)*2,8):
            v=(a[k]+a[k+1])/2; acc+=v*v
        out.append(math.sqrt(acc/(win/4)))
    return out
a=load(sys.argv[1]); b=load(sys.argv[2])
da=len(a)/2/48000; db=len(b)/2/48000
ea=env(a); eb=env(b); n=min(len(ea),len(eb))
ea=ea[:n]; eb=eb[:n]
ma=sum(ea)/n; mb=sum(eb)/n
cov=sum((x-ma)*(y-mb) for x,y in zip(ea,eb)); va=math.sqrt(sum((x-ma)**2 for x in ea)); vb=math.sqrt(sum((y-mb)**2 for y in eb))
corr=cov/(va*vb) if va*vb>0 else 0
peak=max(abs(x) for x in a[::7]) if a else 0
print(f"dur {da:7.2f}s vs {db:7.2f}s  envcorr {corr:5.3f}  peakA {peak}")
