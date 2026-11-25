#!/usr/bin/env python3
# perf3.py: trip 3 offline perf and bpftrace analysis through trip1/scripts/analyze.py's functions (unchanged), pointed at
# campaign/trip3/out/raw and writing to campaign/trip3/out/analysis:
#   perf perfM3       -> analysis/perf_perfM3.txt (per thread user/kernel, contexts, syscalls, top self, top inclusive)
#   children perfM3   -> analysis/children_perfM3_loop.txt (the loop subsystems trip 1 reported, inclusive and callees)
#   bpf bpfM1 bpfM2   -> analysis/bpf_LABEL.txt (syscalls per move per thread, the loop's off-CPU time)
import sys
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
A.RAW = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out/raw'
A.OUT = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out/analysis'
import os; os.makedirs(A.OUT, exist_ok=True)
A.PID = 0
LOOPF = ['cb_loop_run', 'ws_frames', 'pump', 'job_run_frame', 'h_action', 'cb_table_run', 'cb_section_commit', 'cb_fanout_publish',
         'rt_publish_view', 'rt_publish_cast', 'pub_deliver', 'j_game_commit', 'cb_journal_game_acquire', 'flush_now', 'flush_dirty',
         'sendto', 'conn_io', 'recvfrom', 'cb_server_inline_begin', 'cb_server_inline_done', 'cb_thread_cpu_us', 'clock_gettime',
         '__clock_gettime', 'cb_poller_wait.part.0', 'table_commit_products_flags', 'table_act', 'conn_try_parse', 'drain_posts']
cmd = sys.argv[1]
if cmd == 'perf': A.perf(sys.argv[2])
elif cmd == 'children':
    p = f'{A.OUT}/children_{sys.argv[2]}_loop.txt'
    if os.path.exists(p): os.remove(p)
    A.children(sys.argv[2], 'loop', LOOPF)
elif cmd == 'bpf':
    for l in sys.argv[2:]: A.bpf(l)
