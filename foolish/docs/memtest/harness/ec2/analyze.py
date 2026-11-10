import sys,re,collections
R='/Users/alex/Dev/cdom-memtest/results/'
win={}
for l in open(R+'windows.txt'):
    p=l.split(); win[p[0]]=(float(p[1]),float(p[2]))
def pc(a,q): return a[min(len(a)-1,int(q*(len(a)-1)+.5))]
for l in ['idle','hum2','sat2','sat2r2']:
    rows=[l.split() for l in open(R+f'srv/rss_{l}.log') if len(l.split())==3]
    rows=[(float(t),int(r),int(h)) for t,r,h in rows]
    idle=rows[min(4,len(rows)-1)][1]
    w=win.get(l,(rows[0][0],rows[-1][0]))
    ws=sorted(r for t,r,h in rows if w[0]<=t<=w[1])
    hwm=[x.split()[1] for x in open(R+f'srv/status_{l}.txt') if x.startswith('VmHWM')][0]
    print(f"{l}: n={len(ws)} idle_at_start={idle/1024:.1f}MB p50={pc(ws,.5)/1024:.1f} p99={pc(ws,.99)/1024:.1f} max_sampled={ws[-1]/1024:.1f} VmHWM_end={int(hwm)/1024:.1f}MB")
def cls(name,perm):
    if name=='[heap]': return 'heap'
    if name=='[stack]': return 'stack'
    if name in('[vdso]','[vvar]','[vsyscall]'): return name
    if name.endswith('/cbase'): return 'cbase '+('text' if 'x' in perm else 'rodata' if perm.startswith('r--') else 'data')
    if name.startswith('/'):
        b=name.rsplit('/',1)[1]; b=re.sub(r'\.so.*','',b); return b
    return 'anon mmap'
def smaps(f):
    agg=collections.defaultdict(lambda: collections.Counter()); cur=None
    for l in open(f):
        p=l.split()
        if re.match(r'^[0-9a-f]+-[0-9a-f]+ ',l):
            name=p[5] if len(p)>5 else ''
            cur=cls(name,p[1])
            # anonymous mapping right after cbase data = bss
            agg[cur]['n']+=1
        elif p[0] in('Rss:','Shared_Clean:','Shared_Dirty:','Private_Clean:','Private_Dirty:','Swap:'):
            agg[cur][p[0][:-1]]+=int(p[1])
    return agg
a=smaps(R+'srv/cap_idle_smaps.txt'); b=smaps(R+'srv/cap_sat2mid_smaps.txt')
keys=sorted(set(a)|set(b),key=lambda k:-(b[k]['Rss']))
print("| mapping | idle Rss | idle Shared_Clean | idle Private_Clean | idle Private_Dirty | sat2 Rss | sat2 Shared_Clean | sat2 Private_Clean | sat2 Private_Dirty |")
print("|---|---|---|---|---|---|---|---|---|")
for k in keys+['TOTAL']:
    if k=='TOTAL':
        A=sum((a[x] for x in a),collections.Counter()); B=sum((b[x] for x in b),collections.Counter())
    else: A,B=a[k],b[k]
    f=lambda c,s: str(c[s])
    print(f"| {k} | "+" | ".join(f(A,s) for s in('Rss','Shared_Clean','Private_Clean','Private_Dirty'))+" | "+" | ".join(f(B,s) for s in('Rss','Shared_Clean','Private_Clean','Private_Dirty'))+" |")
print("Shared_Dirty/Swap idle", sum(a[x]['Shared_Dirty'] for x in a), sum(a[x]['Swap'] for x in a), "sat2", sum(b[x]['Shared_Dirty'] for x in b), sum(b[x]['Swap'] for x in b))
