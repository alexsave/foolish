#!/usr/bin/env python3
# analyze.py: trip 1 offline analysis (single-threaded). Reads campaign/trip1/out/raw (synced bucket) and writes text tables to
# campaign/trip1/out/analysis/. Window: the play window [t_go+2 s, t_go+23 s] of every run, from the load box's t_go_clk
# (epoch and CLOCK_MONOTONIC taken together); moves counted from the ev.* M lines (t_reply_us in the load box's monotonic
# window); perf/bpf samples cut in the server's CLOCK_MONOTONIC via the server's clk_prof_LABEL.txt (epoch and mono pair).
# Usage: analyze.py moves LABEL... | perf LABEL | bpf LABEL | loadcpu LABEL... | irq LABEL...
import sys, os, re, gzip, tarfile, io, collections
T = '/Users/alex/Dev/foolish-bench-results/campaign/trip1/out'
RAW = T + '/raw'; OUT = T + '/analysis'; os.makedirs(OUT, exist_ok=True)
GHZ = 2.5e9

def clkpair(path):
    w = open(path).read().split(); return float(w[1]), float(w[3])

def load_window(lbl):
    """play window in load epoch, load mono, and the M-line move count in it (from the run's tgz)"""
    tf = tarfile.open(f'{RAW}/load/{lbl}.tgz')
    e0 = m0 = None; n = 0; lat = []
    mem = {}
    for ti in tf:
        if ti.name.endswith('/t_go_clk'):
            w = tf.extractfile(ti).read().decode().split(); e0, m0 = float(w[1]), float(w[3])
    tf = tarfile.open(f'{RAW}/load/{lbl}.tgz')
    a = (m0 + 2) * 1e6; b = (m0 + 23) * 1e6
    for ti in tf:
        if '/ev.' in ti.name:
            for l in tf.extractfile(ti):
                if l[:2] == b'M ':
                    p = l.split(); r = int(p[4])
                    if a <= r < b: n += 1; lat.append((r - int(p[3])) / 1000)
    lat.sort()
    pc = lambda q: lat[min(len(lat) - 1, int(q * (len(lat) - 1) + .5))]
    return dict(e0=e0 + 2, e1=e0 + 23, m0=m0 + 2, m1=m0 + 23, moves=n, p50=pc(.5), p99=pc(.99))

def srv_mono_window(lbl, w):
    e, m = clkpair(f'{RAW}/srv/clk_prof_{lbl}.txt'); off = e - m
    pid = int(open(f'{RAW}/srv/clk_prof_{lbl}.txt').read().split('pid')[1].split()[0])
    return w['e0'] - off, w['e1'] - off, pid

HDR = re.compile(r'^\s*(.+?)\s+(-?\d+)/(\d+)\s+\[(\d+)\]\s+(\d+\.\d+):\s+(\d+)\s+(\S+):\s*(.*)$')
FR = re.compile(r'^\s+([0-9a-f]+)\s+(.+?)\s+\((.*)\)$')

def samples(path):
    """yields (comm, pid, tid, cpu, t, period, frames[(ip, sym, dso)] leaf first) from a perf script dump with callchains"""
    cur = None
    with gzip.open(path, 'rt', errors='replace') as f:
        for l in f:
            if l.startswith('\t') or (l.startswith(' ') and cur is not None and FR.match(l) and not HDR.match(l)):
                m = FR.match(l)
                if m and cur is not None: cur[6].append((m.group(1), m.group(2), m.group(3)))
                continue
            if not l.strip():
                if cur is not None: yield cur; cur = None
                continue
            m = HDR.match(l)
            if m:
                if cur is not None: yield cur
                cur = [m.group(1).strip(), int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5)), int(m.group(6)), []]
                rest = m.group(8).strip()
                if rest:   # flat dump: ip sym (dso) on the header line
                    mm = re.match(r'([0-9a-f]+)\s+(.+?)\s+\((.*)\)$', rest)
                    if mm: cur[6].append((mm.group(1), mm.group(2), mm.group(3)))
    if cur is not None: yield cur

def iskern(fr): return fr[0].startswith('ffff') or fr[2].startswith('[kernel') or (fr[2].startswith('[') and fr[2] not in ('[vdso]', '[unknown]'))
SYS = re.compile(r'^__arm64_sys_(\w+)$')
SOFT = {'handle_softirqs', '__do_softirq', 'net_rx_action'}
HARD = {'gic_handle_irq', 'el1_interrupt', 'el0_interrupt', 'handle_domain_irq', 'generic_handle_domain_irq', '__el0_irq_handler_common'}

def thread_name(comm, tid, pid, names):
    if pid != PID: return f'other:{comm}' if pid != 0 else 'idle(swapper)'
    if tid == pid: return 'loop(main)'
    return names.get(tid, f'{comm}:{tid}')

def perf(lbl):
    global PID
    w = load_window(lbl); s0, s1, PID = srv_mono_window(lbl, w); mv = w['moves']
    us = lambda c: c / GHZ * 1e6 / mv
    o = open(f'{OUT}/perf_{lbl}.txt', 'w')
    pr = lambda *a: print(*a, file=o)
    pr(f'# perf {lbl}: play window load epoch [{w["e0"]:.3f},{w["e1"]:.3f}], server mono [{s0:.3f},{s1:.3f}], moves {mv}, server pid {PID}')
    pr(f'# source: raw/proc/{lbl}/cc_{lbl}.txt.gz (perf script with callchains), us = sum(period)/2.5e9/moves*1e6 (cycles at 2.5 GHz; for cpu-clock period is ns, so the unit is ns of wall time)')
    tot = collections.Counter(); bycpu = collections.Counter(); byth = collections.Counter(); byth_uk = collections.Counter()
    selfsym = collections.defaultdict(collections.Counter); incl = collections.defaultdict(collections.Counter)
    syscall = collections.defaultdict(collections.Counter); sys_leaf = collections.defaultdict(collections.Counter); sys_child = collections.defaultdict(collections.Counter)
    ctx = collections.Counter(); soft_in_sys = collections.Counter(); cpu_ctx = collections.Counter(); comms = {}
    nsamp = 0; tmin = 1e18; tmax = 0
    for comm, pid, tid, cpu, t, per, fr in samples(f'{RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
        if not (s0 <= t < s1): continue
        nsamp += 1; tmin = min(tmin, t); tmax = max(tmax, t)
        if pid == PID: comms[tid] = comm
        th = thread_name(comm, tid, pid, {})
        th = f'{th}' if pid != PID or tid == pid else f'{comm}:{tid}'
        k = 'kernel' if fr and iskern(fr[0]) else 'user'
        tot[k] += per; bycpu[(cpu, k)] += per; byth[(th, cpu)] += per; byth_uk[(th, k)] += per
        if fr: selfsym[(th, k)][fr[0][1]] += per
        seen = set()
        for f in fr:
            if f[1] not in seen: seen.add(f[1]); incl[th][f[1]] += per
        names = [f[1] for f in fr]
        isoft = next((i for i, n in enumerate(names) if n in SOFT), None)
        ihard = next((i for i, n in enumerate(names) if n in HARD), None)
        isys = next((i for i, n in enumerate(names) if SYS.match(n)), None)
        if pid == 0: c = 'idle-task' + ('+softirq' if isoft is not None else '+hardirq' if ihard is not None else '')
        elif isoft is not None: c = 'softirq'
        elif ihard is not None: c = 'hardirq'
        elif isys is not None: c = 'syscall'
        elif k == 'kernel': c = 'kernel-other'
        else: c = 'user'
        ctx[(th, c)] += per; cpu_ctx[(cpu, c)] += per
        if isys is not None:
            sc = SYS.match(names[isys]).group(1); syscall[th][sc] += per
            sys_leaf[(th, sc)][names[0]] += per
            if isys > 0: sys_child[(th, sc)][names[isys - 1]] += per
            if isoft is not None and isoft < isys: soft_in_sys[(th, sc, cpu)] += per
        elif isoft is not None: soft_in_sys[(th, '(not in a syscall)', cpu)] += per
    T_ = sum(tot.values())
    pr(f'samples in window {nsamp}, first {tmin:.3f} last {tmax:.3f}; server threads {sorted(comms.items())}')
    pr(f'\n## total per move: {us(T_):.2f} us (user {us(tot["user"]):.2f}, kernel {us(tot["kernel"]):.2f})')
    pr('\n## by CPU (us per move)')
    for c in sorted(set(c for c, _ in bycpu)): pr(f'cpu{c} user {us(bycpu[(c,"user")]):.2f} kernel {us(bycpu[(c,"kernel")]):.2f} total {us(bycpu[(c,"user")]+bycpu[(c,"kernel")]):.2f}')
    pr('\n## by thread (us per move): user, kernel, total, and per CPU')
    ths = sorted(set(t for t, _ in byth_uk), key=lambda t: -(byth_uk[(t,'user')] + byth_uk[(t,'kernel')]))
    for t in ths:
        u, kk = byth_uk[(t, 'user')], byth_uk[(t, 'kernel')]
        if us(u + kk) < 0.01: continue
        pc = ' '.join(f'cpu{c}={us(byth[(t,c)]):.2f}' for c in (0, 1) if byth[(t, c)])
        pr(f'{t:28s} user {us(u):7.2f} kernel {us(kk):7.2f} total {us(u+kk):7.2f}  {pc}')
    pr('\n## context per thread (us per move): user, syscall, softirq, hardirq, kernel-other, idle-task*')
    for t in ths:
        row = {c: ctx[(t, c)] for (tt, c) in ctx if tt == t}
        if us(sum(row.values())) < 0.01: continue
        pr(f'{t:28s} ' + ' '.join(f'{c}={us(v):.2f}' for c, v in sorted(row.items(), key=lambda x: -x[1])))
    pr('\n## context per CPU (us per move)')
    for c in (0, 1):
        row = {x: v for (cc, x), v in cpu_ctx.items() if cc == c}
        pr(f'cpu{c} ' + ' '.join(f'{x}={us(v):.2f}' for x, v in sorted(row.items(), key=lambda x: -x[1])))
    pr('\n## syscalls: inclusive us per move per thread, top kernel leaf symbols and the frame under __arm64_sys_*')
    for t in ths:
        for sc, v in syscall[t].most_common():
            if us(v) < 0.05: continue
            lf = ', '.join(f'{s} {us(x):.2f}' for s, x in sys_leaf[(t, sc)].most_common(6))
            ch = ', '.join(f'{s} {us(x):.2f}' for s, x in sys_child[(t, sc)].most_common(4))
            pr(f'{t:24s} {sc:16s} {us(v):6.2f} | callees: {ch} | leaves: {lf}')
    pr('\n## softirq (handle_softirqs/__do_softirq/net_rx_action on the chain) by thread, syscall it interrupted, CPU (us per move)')
    for (t, sc, c), v in sorted(soft_in_sys.items(), key=lambda x: -x[1]):
        if us(v) >= 0.02: pr(f'{t:24s} cpu{c} {sc:20s} {us(v):.2f}')
    for t in ths:
        for k in ('user', 'kernel'):
            ss = selfsym[(t, k)]
            if us(sum(ss.values())) < 0.1: continue
            pr(f'\n## top 30 self symbols: {t} {k} (us per move, share of the thread {k})')
            sm = sum(ss.values())
            for s, v in ss.most_common(30): pr(f'{us(v):7.3f} {100*v/sm:5.1f}%  {s}')
    for t in ths:
        ii = incl[t]
        if us(byth_uk[(t, 'user')] + byth_uk[(t, 'kernel')]) < 0.2: continue
        pr(f'\n## top 60 inclusive symbols: {t} (us per move)')
        for s, v in ii.most_common(60): pr(f'{us(v):7.3f}  {s}')
    pr('\n## named inclusive symbols per thread (us per move), for the leads')
    pat = re.compile(r'^(h_action|cb_table_run|cb_section_commit|pub_deliver|flush_txn|flusher|list_write.*|sqlite3_step|sqlite3VdbeExec|run_replay_v6.*|coder_.*|bn_divmod.*|finalize.*|j_game_commit|conn_.*|ws_frames|loop_.*|req_cb|job_run.*|crc32c.*|__aarch64_.*|clock_gettime|__clock_gettime|__arm64_sys_clock_gettime|.*mutex.*|__lock|__unlock|futex.*|__timedwait.*|memcpy|memset|malloc|free|epoll.*|sqlite3.*Insert|sqlite3BtreeInsert|sqlite3_exec|sqlite3WalFrames|sqlite3PagerCommitPhaseOne)$')
    for t in ths:
        ii = incl[t]; rows = [(s, v) for s, v in ii.items() if pat.match(s) and us(v) >= 0.02]
        if not rows: continue
        pr(f'-- {t}'); [pr(f'   {us(v):7.3f}  {s}') for s, v in sorted(rows, key=lambda x: -x[1])]
    # all sqlite3* on a thread: inclusive of the outermost sqlite3 frame
    pr('\n## sqlite inclusive (samples with any sqlite3* frame) and atomics/crc32c self, per thread (us per move)')
    o.close()
    o2 = open(f'{OUT}/perf_{lbl}.txt', 'a')
    sq = collections.Counter(); at = collections.Counter(); crc = collections.Counter()
    for comm, pid, tid, cpu, t, per, fr in samples(f'{RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
        if not (s0 <= t < s1) or pid != PID: continue
        th = 'loop(main)' if tid == pid else f'{comm}:{tid}'
        if any(f[1].startswith('sqlite3') for f in fr): sq[th] += per
        if fr and fr[0][1].startswith('__aarch64_'): at[th] += per
        if fr and fr[0][1].startswith('crc32c'): crc[th] += per
    for th in sorted(set(sq) | set(at) | set(crc)): print(f'{th:24s} sqlite_incl {us(sq[th]):.3f} atomics_self {us(at[th]):.3f} crc32c_self {us(crc[th]):.3f}', file=o2)
    o2.close()
    print(open(f'{OUT}/perf_{lbl}.txt').read()[:3000])

def parse_bt(path):
    """yields (nsecs, {map: {key: value}}, {histmap: {key: {lo: count}}}) per TICK"""
    ticks = []; cur = None; hm = None; hk = None
    for l in open(path, errors='replace'):
        l = l.rstrip('\n')
        if l.startswith('TICK '): cur = (int(l.split()[1]), collections.defaultdict(dict), collections.defaultdict(dict)); continue
        if l == 'ENDTICK': ticks.append(cur); cur = None; continue
        if cur is None: continue
        m = re.match(r'^@(\w+)(?:\[(.*)\])?: (-?\d+)$', l)
        if m: cur[1][m.group(1)][m.group(2) or ''] = int(m.group(3)); hm = None; continue
        m = re.match(r'^@(\w+)(?:\[(.*)\])?: $', l)
        if m: hm, hk = m.group(1), m.group(2) or ''; cur[2][hm][hk] = {}; continue
        m = re.match(r'^\[(-?[\d.]+[KMG]?)(?:, ([\d.]+[KMG]?)\)|\])?\s+(\d+) \|', l) or re.match(r'^\((\.\.\.), (-?\d+)\)\s+(\d+) \|', l) or re.match(r'^\[(-?\d+), (\.\.\.)\)\s+(\d+) \|', l)
        if m and hm: cur[2][hm][hk][m.group(1)] = int(m.group(3))
    return ticks

def kv(s):
    m = {'K': 1024, 'M': 1024**2, 'G': 1024**3}
    if s == '...': return -1
    return float(s[:-1]) * m[s[-1]] if s[-1] in m else float(s)

NR = {22: 'epoll_pwait', 441: 'epoll_pwait2', 206: 'sendto', 207: 'recvfrom', 98: 'futex', 63: 'read', 64: 'write', 66: 'writev', 67: 'pread64',
      68: 'pwrite64', 69: 'preadv', 70: 'pwritev', 82: 'fsync', 83: 'fdatasync', 211: 'sendmsg', 212: 'recvmsg', 113: 'clock_gettime', 57: 'close',
      56: 'openat', 25: 'fcntl', 222: 'mmap', 215: 'munmap', 226: 'mprotect', 233: 'madvise', 214: 'brk', 21: 'epoll_ctl', 242: 'accept4', 203: 'connect',
      208: 'setsockopt', 209: 'getsockopt', 101: 'nanosleep', 115: 'clock_nanosleep', 124: 'sched_yield', 129: 'kill', 131: 'tgkill', 134: 'rt_sigaction',
      135: 'rt_sigprocmask', 62: 'lseek', 80: 'fstat', 79: 'newfstatat', 35: 'unlinkat', 278: 'getrandom', 269: 'sendmmsg', 243: 'recvmmsg', 172: 'getpid',
      178: 'gettid', 19: 'eventfd2', 20: 'epoll_create1', 59: 'pipe2', 46: 'ftruncate', 47: 'fallocate', 72: 'pselect6', 73: 'ppoll', 167: 'prctl', 261: 'prlimit64',
      29: 'ioctl', 23: 'dup', 24: 'dup3', 210: 'shutdown', 198: 'socket', 200: 'bind', 201: 'listen', 204: 'getsockname', 205: 'getpeername', 220: 'clone', 435: 'clone3', 93: 'exit', 94: 'exit_group'}

def bpf(lbl):
    w = load_window(lbl); s0, s1, pid = srv_mono_window(lbl, w); mv = w['moves']
    ticks = parse_bt(f'{RAW}/srv/bpf_{lbl}.txt')
    tin = [t for t in ticks if s0 * 1e9 <= t[0] <= s1 * 1e9]
    a, b = tin[0], tin[-1]; span = (b[0] - a[0]) / 1e9
    # moves inside [a, b] exactly: rescale by the ev count in the tick span (counted directly)
    mvs = moves_in(lbl, a[0] / 1e9, b[0] / 1e9)
    o = open(f'{OUT}/bpf_{lbl}.txt', 'w'); pr = lambda *x: print(*x, file=o)
    comm = {}
    for l in open(f'{RAW}/srv/bpf_{lbl}.txt', errors='replace'):
        m = re.match(r'^@comm\[(\d+)\]: (.*)$', l.strip())
        if m: comm[int(m.group(1))] = m.group(2)
    name = lambda tid: 'loop(main)' if tid == pid else f'{comm.get(tid, "?")}:{tid}'
    pr(f'# bpf {lbl}: ticks {len(tin)} in window, span {span:.3f} s (server mono {a[0]/1e9:.3f}..{b[0]/1e9:.3f}), moves in span {mvs} (from ev M lines), source raw/srv/bpf_{lbl}.txt')
    d = lambda mp, k: b[1][mp].get(k, 0) - a[1][mp].get(k, 0)
    pr('\n## syscalls per move by thread and syscall (count/move, us inside per move, us per call)')
    rows = []
    for k in b[1]['sc']:
        tid, nr = map(int, k.split(', ')); n = d('sc', k); ns = d('sct', k)
        if n > 0: rows.append((tid, nr, n, ns))
    per_th = collections.Counter()
    for tid, nr, n, ns in sorted(rows, key=lambda r: (r[0] != pid, r[0], -r[2])):
        per_th[name(tid)] += n
        if n / mvs >= 0.001: pr(f'{name(tid):16s} {NR.get(nr, nr)!s:14s} {n/mvs:8.4f}/move  {ns/1e3/mvs:8.3f} us/move  {ns/1e3/n:9.2f} us/call')
    pr('\n## syscalls per move per thread total: ' + ', '.join(f'{t} {v/mvs:.3f}' for t, v in per_th.most_common()) + f'; all {sum(per_th.values())/mvs:.3f}')
    pr('\n## sched_switch-outs per move by thread and prev_state (0 runnable or yield, 1 S sleep, 2 D, 256+ preempted)')
    for k in sorted(b[1]['sw'], key=lambda k: -d('sw', k)):
        tid, st = map(int, k.split(', ')); n = d('sw', k)
        if n: pr(f'{name(tid):16s} state {st:5d} {n/mvs:.4f}/move ({n} in span)')
    pr('\n## loop thread off-CPU time by (prev_state, syscall it was inside): ms in span, share of span, count, us per off')
    for k in sorted(b[1]['lo_off'], key=lambda k: -d('lo_off', k)):
        st, nr = map(int, k.split(', ')); ns = d('lo_off', k); n = d('lo_offn', k)
        if n: pr(f'state {st:4d} in {NR.get(nr, nr) if nr >= 0 else "(no syscall: preempted in user/kernel)"!s:20s} {ns/1e6:9.2f} ms {100*ns/1e9/span:6.2f}% n={n} {ns/1e3/n:8.1f} us/off')
    pr(f'loop wakeups {d("lo_wakes","")}, run-queue wait after wakeup {d("lo_rq_wake_ns","")/1e6:.2f} ms ({d("lo_rq_wake_ns","")/1e3/mvs:.3f} us/move), after preemption {d("lo_rq_pre_ns","")/1e6:.2f} ms')
    pr('\n## histograms (window difference): key, bucket low edge: count; mean from bucket midpoints (approximate)')
    for hm in ('ep_ev', 'send_b', 'recv_b', 'futex_ns', 'wr_b', 'lo_rq_wake', 'lo_rq_pre'):
        for k in b[2].get(hm, {}):
            hb = b[2][hm][k]; ha = a[2].get(hm, {}).get(k, {})
            dd = {x: hb[x] - ha.get(x, 0) for x in hb if hb[x] - ha.get(x, 0) > 0}
            if not dd: continue
            n = sum(dd.values()); keys = sorted(dd, key=kv)
            if hm == 'ep_ev': mean = sum(kv(x) * c for x, c in dd.items()) / n
            else: mean = sum((kv(x) * 1.5 if kv(x) > 0 else 0) * c for x, c in dd.items()) / n
            kn = k
            try:
                kk = [int(x) for x in k.split(', ')]; kn = name(kk[0]) + (f' {NR.get(kk[1], kk[1])}' if len(kk) > 1 else '')
            except ValueError: pass
            pr(f'@{hm}[{kn}] n={n} ({n/mvs:.4f}/move) mean~{mean:.1f}: ' + ' '.join(f'{x}:{dd[x]}' for x in keys))
    o.close(); print(open(f'{OUT}/bpf_{lbl}.txt').read())

def moves_in(lbl, sm0, sm1):
    """moves whose reply falls in server mono [sm0, sm1], mapped through both boxes' epoch-mono pairs"""
    e, m = clkpair(f'{RAW}/srv/clk_prof_{lbl}.txt'); soff = e - m
    tf = tarfile.open(f'{RAW}/load/{lbl}.tgz'); le = lm = None
    for ti in tf:
        if ti.name.endswith('/t_go_clk'): w = tf.extractfile(ti).read().decode().split(); le, lm = float(w[1]), float(w[3])
    loff = le - lm
    a = (sm0 + soff - loff) * 1e6; bb = (sm1 + soff - loff) * 1e6; n = 0
    tf = tarfile.open(f'{RAW}/load/{lbl}.tgz')
    for ti in tf:
        if '/ev.' in ti.name:
            for l in tf.extractfile(ti):
                if l[:2] == b'M ':
                    r = int(l.split()[4])
                    if a <= r < bb: n += 1
    return n

def children(lbl, thsel, funcs):
    """for the thread selector (loop | TID) and each function F: inclusive us/move of F and of each direct callee of F (outermost F on the chain)"""
    global PID
    w = load_window(lbl); s0, s1, PID = srv_mono_window(lbl, w); mv = w['moves']
    us = lambda c: c / GHZ * 1e6 / mv
    inc = collections.Counter(); ch = collections.defaultdict(collections.Counter); selfc = collections.Counter()
    for comm, pid, tid, cpu, t, per, fr in samples(f'{RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
        if not (s0 <= t < s1) or pid != PID: continue
        if not ((thsel == 'loop' and tid == pid) or str(tid) == thsel): continue
        names = [f[1] for f in fr]
        for F in funcs:
            idx = [i for i, n in enumerate(names) if n == F]
            if not idx: continue
            i = idx[-1]; inc[F] += per
            if i == 0: selfc[F] += per; ch[F]['(self)'] += per
            else: ch[F][names[i - 1]] += per
    out = []
    for F in funcs:
        out.append(f'{F} inclusive {us(inc[F]):.3f} us/move; callees: ' + ', '.join(f'{c} {us(v):.3f}' for c, v in ch[F].most_common(14) if us(v) >= 0.01))
    txt = '\n'.join(out); print(txt)
    open(f'{OUT}/children_{lbl}_{thsel}.txt', 'a').write(txt + '\n')

def clk(lbl):
    """cpu-clock run: share of each CPU's wall time (period = ns) in the play window by owner and context"""
    global PID
    w = load_window(lbl); s0, s1, PID = srv_mono_window(lbl, w)
    tot = collections.Counter(); cat = collections.Counter(); idle_leaf = collections.defaultdict(collections.Counter)
    for comm, pid, tid, cpu, t, per, fr in samples(f'{RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
        if not (s0 <= t < s1): continue
        names = [f[1] for f in fr]
        soft = any(n in SOFT for n in names); hard = any(n in HARD for n in names)
        k = fr and iskern(fr[0])
        if pid == 0: who = 'idle-task'
        elif pid == PID: who = 'loop' if tid == pid else f'{comm}:{tid}'
        else: who = f'other:{comm}'
        ctx = 'softirq' if soft else 'hardirq' if hard else ('kernel' if k else 'user') if pid != 0 else 'idle'
        if pid == 0 and ctx == 'idle' and names: idle_leaf[cpu][names[0]] += per
        tot[cpu] += per; cat[(cpu, who, ctx)] += per
    o = open(f'{OUT}/clk_{lbl}.txt', 'w')
    print(f'# cpu-clock {lbl}: share of each CPU\'s sampled wall time in the play window (server mono [{s0:.3f},{s1:.3f}]), source raw/proc/{lbl}/cc_{lbl}.txt.gz', file=o)
    for c in sorted(tot):
        print(f'cpu{c} sampled {tot[c]/1e9:.3f} s of {s1-s0:.1f} s', file=o)
        for (cc, who, ctx), v in sorted(cat.items(), key=lambda x: -x[1]):
            if cc == c and v / tot[c] >= 0.001: print(f'   {who:22s} {ctx:8s} {100*v/tot[c]:6.2f}%', file=o)
        print('   idle-task leaf symbols (pure idle): ' + ', '.join(f'{s} {100*v/tot[c]:.2f}%' for s, v in idle_leaf[c].most_common(5)), file=o)
    o.close(); print(open(f'{OUT}/clk_{lbl}.txt').read())

if __name__ == '__main__':
    PID = 0
    cmd = sys.argv[1]
    if cmd == 'moves':
        for l in sys.argv[2:]: w = load_window(l); print(f'{l} play_moves={w["moves"]} moves_per_s={w["moves"]/21:.1f} p50_ms={w["p50"]:.2f} p99_ms={w["p99"]:.2f} epoch=[{w["e0"]:.3f},{w["e1"]:.3f}]')
    elif cmd == 'perf': perf(sys.argv[2])
    elif cmd == 'bpf': bpf(sys.argv[2])
    elif cmd == 'clk': clk(sys.argv[2])
    elif cmd == 'children': children(sys.argv[2], sys.argv[3], sys.argv[4:])
