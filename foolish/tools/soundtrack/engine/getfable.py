# getfable.py task_output_jsonl outdir : pull the agent's SubagentHandback text, split score (before NOTES / PARTS AND ARCS) from the plan
import json, sys, re
msg = None
for line in open(sys.argv[1]):
    try: o = json.loads(line)
    except Exception: continue
    m = o.get('message') or {}
    c = m.get('content')
    if isinstance(c, list):
        for b in c:
            if b.get('type') == 'tool_use' and b.get('name') == 'SubagentHandback': msg = b['input']['message']
open(sys.argv[2] + '/fable_suite.md', 'w').write(msg)
s = msg
if '```' in s:
    a = s.index('```'); a = s.index('\n', a) + 1; s = s[a:s.index('```', a)]
for marker in ['\nNOTES', '\nPARTS AND ARCS', '\n**Summary', '\nSUMMARY']:
    i = s.find(marker)
    if i > 0: s = s[:i]
open(sys.argv[2] + '/score.txt', 'w').write(s)
print(s.count('SECTION'), 'sections')
