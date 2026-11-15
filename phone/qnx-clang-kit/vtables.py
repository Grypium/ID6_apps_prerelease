import subprocess,struct,sys
def syms(f):
    out=subprocess.run(['llvm-readelf','-s','-W',f],capture_output=True,text=True).stdout
    S={}
    for l in out.splitlines():
        p=l.split()
        if len(p)>=8 and p[0].endswith(':') and p[3]=='OBJECT' and p[7].startswith(('_ZTV','_ZTI','_ZTS')):
            S[p[7]]=(int(p[1],16),int(p[2]))
    return S
def relocs(f):
    out=subprocess.run(['llvm-readelf','-r','-W',f],capture_output=True,text=True).stdout
    R={}
    for l in out.splitlines():
        p=l.split()
        if len(p)>=3 and p[0].isalnum() and len(p[0])==8:
            try: off=int(p[0],16)
            except: continue
            R[off]=(p[2], p[4] if len(p)>4 else '')
    return R
def fsyms(f):
    out=subprocess.run(['llvm-readelf','-s','-W',f],capture_output=True,text=True).stdout
    A={}
    for l in out.splitlines():
        p=l.split()
        if len(p)>=8 and p[0].endswith(':') and p[3]=='FUNC': A.setdefault(int(p[1],16)&~1,p[7])
    return A
def vt(f):
    data=open(f,'rb').read(); S=syms(f); R=relocs(f); F=fsyms(f)
    # map vaddr->file offset via program headers
    ph=subprocess.run(['llvm-readelf','-l','-W',f],capture_output=True,text=True).stdout
    segs=[(int(p[2],16),int(p[1],16),int(p[4],16)) for p in (l.split() for l in ph.splitlines()) if p and p[0]=='LOAD']
    def off(va):
        for v,o,sz in segs:
            if v<=va<v+sz: return va-v+o
    out={}
    for n,(va,size) in S.items():
        if not n.startswith('_ZTV'): continue
        slots=[]
        for i in range(0,size,4):
            a=va+i
            if a in R and R[a][1]: slots.append(R[a][1])
            else:
                w=struct.unpack_from('<I',data,off(a))[0]
                slots.append(F.get(w&~1, hex(w)) if a in R else str(w))
        out[n]=slots
    return out
ref,new=vt(sys.argv[1]),vt(sys.argv[2])
for n in sorted(set(ref)|set(new)):
    a,b=ref.get(n),new.get(n)
    if a is None or b is None: print('only in', 'new' if a is None else 'ref', n); continue
    print(f'{n}: {len(a)} slots ref, {len(b)} new')
    for i,(x,y) in enumerate(zip(a,b)):
        if x!=y: print(f'   slot {i}: ref {x}  new {y}')
