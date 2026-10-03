"""Off-target model of src/aprs_decoder.cpp (AFSK 1200 / AX.25): generates the
self-test frame with de-emphasis tilt, noise and rate error, and counts the
frames each demodulator variant decodes. Needs numpy.

    python tools/aprs_sim.py [trials per noise level]
"""
import numpy as np, sys
FS=12000; SPB=10; MARK=1200; SPACE=2200
def fcs(b):
    crc=0xFFFF
    for x in b:
        crc^=x
        for _ in range(8): crc=(crc>>1)^0x8408 if crc&1 else crc>>1
    return crc^0xFFFF
def frame():
    calls=[("APRS",0),("CT1ABC",9),("WIDE1",1)]; out=[]
    for i,(c,s) in enumerate(calls):
        c=c.ljust(6); out+= [ord(ch)<<1 for ch in c]; out.append(0x60|(s<<1)|(1 if i==2 else 0))
    out+=[3,0xF0]+[ord(c) for c in "!3842.50N/00909.00W>Teste APRS 1200"]
    f=fcs(out); return out+[f&255,f>>8]
FR=frame()
def bits():
    b=[]; flag=[(0x7E>>i)&1 for i in range(8)]
    b+=flag*40; run=0
    for x in FR:
        for i in range(8):
            v=(x>>i)&1; b.append(v); run=run+1 if v else 0
            if run==5: b.append(0); run=0
    b+=flag*4; return b
BITS=bits()
def gen(space_gain,noise,rate,rng):
    bl=FS/(1200*rate); n=int(len(BITS)*bl)+10
    t_bit=0; bi=0; mark=True
    if BITS[0]==0: mark=not mark
    f=np.zeros(n); 
    for s in range(n):
        f[s]=MARK if mark else SPACE
        t_bit+=1
        if t_bit>=bl:
            t_bit-=bl; bi+=1
            if bi<len(BITS) and BITS[bi]==0: mark=not mark
            if bi>=len(BITS): f=f[:s+1]; break
    ph=np.cumsum(2*np.pi*f/FS); amp=np.where(f==MARK,1.0,space_gain)
    x=0.3*amp*np.sin(ph)+noise*rng.uniform(-1,1,(4,len(f))).sum(0)
    return np.r_[x,np.zeros(1024)]
def demod(x,variant):
    n=len(x); t=np.arange(n)
    def corr(fhz):
        z=x*np.exp(-2j*np.pi*fhz*t/FS)
        c=np.convolve(z,np.ones(SPB),'full')[:n]
        return np.abs(c)
    m=corr(MARK); s=corr(SPACE)
    A,D=variant.get('att',0.3),variant.get('dec',0.0003)
    def agc(v):
        pk=0;vl=0;o=np.zeros(n)
        for i in range(n):
            mm=v[i]; pk+=(A if mm>pk else D)*(mm-pk); vl+=(A if mm<vl else D)*(mm-vl)
            sp=pk-vl; o[i]=(mm-vl)/sp if sp>1e-9 else 0
        return o
    if variant.get('agc',True): mn=agc(m); sn=agc(s)
    else:
        mn=m; sn=s*variant.get('tilt',1.0)
    d=mn-sn
    if variant.get('lp',0):
        k=variant['lp']; d=np.convolve(d,np.ones(k)/k,'same')
    # dpll + hdlc
    step=int(4294967296*1200/FS)
    def wrap(v): return (v+2**31)%2**32-2**31
    pll=0; prev_level=False; prev_raw=False; good=0
    pat=0; inf=False; ones=0; acc=0; bp=0; fr=[]; got=0
    for i in range(n):
        level=d[i]>0
        b4=pll; pll=wrap(pll+step)
        if b4>0 and pll<0:
            bit=1 if level==prev_raw else 0; prev_raw=level
            pat=((pat>>1)|(bit<<7))&255
            if pat==0x7E:
                if inf and bp==7 and len(fr)>=18 and fcs(fr[:-2])==(fr[-2]|fr[-1]<<8) and fr[:-2]==FR[:-2]: got+=1
                inf=True; fr=[]; bp=0; ones=0; continue
            if bit:
                ones+=1
                if ones>=7: inf=False; continue
            else:
                if ones==5: ones=0; continue
                ones=0
            if not inf: continue
            acc=((acc>>1)|(bit<<7))&255; bp+=1
            if bp==8: bp=0; fr.append(acc)
        if level!=prev_level:
            near=-2*step<pll<2*step
            good=min(good+1,64) if near else max(good-1,0)
            pll=wrap(int(pll*(0.74 if good>32 else 0.5)))
            prev_level=level
    return got>0
# 'firmware' mirrors src/aprs_decoder.cpp (SMOOTH = 5); the others are the
# variants it was compared with.
variants={'firmware':{'lp':5}, 'sem suavizar':{}, 'suavizar 3':{'lp':3}}
rng=np.random.default_rng(1)
for noise in [0.06,0.08,0.10]:
    xs=[gen(0.5,noise,1.0,rng) for _ in range(int(sys.argv[1]) if len(sys.argv)>1 else 12)]
    print('ruido',noise, {k:sum(demod(x,v) for x in xs) for k,v in variants.items()}, 'de',len(xs))
