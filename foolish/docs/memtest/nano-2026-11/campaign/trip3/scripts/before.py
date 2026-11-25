#!/usr/bin/env python3
# before.py: the same loop inclusive list as perf3.py children, on trip 1's perfcy run (cbase_D, 9c5057b64; trip1/out/raw
# read only), written to trip3/out/analysis/children_perfcy_loop.txt, so the campaign's before and after use one method.
import sys, os
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip3/scripts')
A.OUT = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out/analysis'; A.PID = 0
import importlib.util
spec = importlib.util.spec_from_file_location('p3', '/Users/alex/Dev/foolish-bench-results/campaign/trip3/scripts/perf3.py')
p = f'{A.OUT}/children_perfcy_loop.txt'
if os.path.exists(p): os.remove(p)
LOOPF = open('/Users/alex/Dev/foolish-bench-results/campaign/trip3/scripts/perf3.py').read().split('LOOPF = ')[1].split(']')[0] + ']'
A.children('perfcy', 'loop', eval(LOOPF))
