#!/usr/bin/env python3
# Turn a decoded replay (decode_to_json.mjs output) into the moves file the
# og_explain driver consumes: one recorded action per line,
#   <type> <seat> <suit,value>...        attack | pass
#   cover <seat> <suit,value>:<tsuit,tvalue>...
#   pickup <seat>   |   good <seat>
# Only the deciding actions are emitted (draws / discards / defender_change are
# replayed by the engine itself). Usage: make_moves.py replay_decoded.json moves.txt
import json, sys

rd = json.load(open(sys.argv[1]))
out = []
for l in rd['logs']:
    t, seat = l['t'], l['seat']
    if t in ('attack', 'pass'):
        cards = ' '.join(f"{c['p']['suit']},{c['p']['value']}" for c in l['cards'])
        out.append(f"{t} {seat} {cards}")
    elif t == 'cover':
        pairs = ' '.join(f"{c['p']['suit']},{c['p']['value']}:{c['tg']['suit']},{c['tg']['value']}"
                         for c in l['cards'] if c['tg'])
        out.append(f"cover {seat} {pairs}")
    elif t in ('pickup', 'good'):
        out.append(f"{t} {seat}")
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
print(f"{len(out)} moves -> {sys.argv[2]}", file=sys.stderr)
