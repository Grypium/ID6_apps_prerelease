import subprocess,sys,os
OUT=sys.argv[1] if len(sys.argv)>1 else "out-2d9990b"
REF=sys.argv[2] if len(sys.argv)>2 else "ref"
def dyn(f):
    out=subprocess.run(['llvm-readelf','--dyn-syms','-W',f],capture_output=True,text=True).stdout
    und,defs=set(),{}
    for l in out.splitlines():
        p=l.split()
        if len(p)>=8 and p[0].endswith(':') and p[7]:
            n=p[7].split('@')[0]
            if p[6]=='UND': und.add(n)
            elif p[4] in('GLOBAL','WEAK'): defs[n]=(p[3],int(p[2],0) if p[2].isdigit() else p[2])
    return und,defs
def needed(f):
    out=subprocess.run(['llvm-readelf','-d',f],capture_output=True,text=True).stdout
    return [l.split('[')[1].rstrip(']') for l in out.splitlines() if 'NEEDED' in l]
noise={'_Jv_RegisterClasses','__register_frame_info','__deregister_frame_info','__cxa_finalize','_init_array','_fini_array','_preinit_array','_init_libc','__aeabi_uldivmod','__gnu_uldivmod_helper','_Ctype','_Dclass'}
linknoise={'__bss_start','__bss_start__','__bss_end__','_bss_end__','__end__','_end','_edata','__data_start','__exidx_start','__exidx_end','_btext','_stack','_init','_fini','__dso_handle'}
for name,ref,new in [(n,f'{REF}/{d}/{n}',f'{OUT}/{n}') for d,n in [('BoostDeck','libBoostDeck.so'),('ShiftDeck','libShiftDeck.so'),('MhdMaps','libMhdMaps.so'),('BoostDeck','boostdeck-collector-qnx'),('ShiftDeck','shift-deck-qnx'),('MhdMaps','mhd-maps-qnx'),('tools','gaugedeck-fileops')]]:
    ru,rd=dyn(ref); nu,nd=dyn(new)
    print(f'== {name}')
    print('  NEEDED ref:',needed(ref)); print('  NEEDED new:',needed(new))
    extra=sorted(nu-ru-noise); 
    if extra: print('  EXTRA imports (not in official):',extra)
    miss=sorted((ru-nu)-noise)
    if miss: print('  imports official had, new does not:',miss)
    rdx={k:v for k,v in rd.items() if k not in linknoise}; ndx={k:v for k,v in nd.items() if k not in linknoise}
    if set(rdx)-set(ndx): print('  MISSING exports:',sorted(set(rdx)-set(ndx)))
    if set(ndx)-set(rdx): print('  extra exports:',sorted(set(ndx)-set(rdx))[:15])
    for k in sorted(set(rdx)&set(ndx)):
        if rdx[k][0]=='OBJECT' and rdx[k]!=ndx[k]: print(f'  size differs {k}: ref {rdx[k]} new {ndx[k]}')
