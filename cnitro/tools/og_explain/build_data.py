#!/usr/bin/env python3
# Merge a decoded replay (decode_to_json.mjs) with octogen's deliberation dump
# (og_explain OG_EXPLAIN sink) into page_data.json for gen_html.py. EVERYTHING
# about the specific game is DERIVED here — trump, who won, the fool, the flip,
# the agree/differ tally, and which moves to flag — so the page is correct for
# whatever game is passed in. Nothing is hardcoded.
#
#   build_data.py replay_decoded.json delib.jsonl <seed> page_data.json
import json, sys

rd = json.load(open(sys.argv[1]))
delib = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
SEED = sys.argv[3]
OUT = sys.argv[4]

logs = rd['logs']
by_ply = {r['ply']: r for r in delib}

TRUMP = rd['powerSuit']
FLIP = rd['trumpCard']
FIRST = rd['firstAttacker']
FOOL = rd['fool']
ELIM = rd['eliminationOrder']
NPLAYERS = rd.get('playerCount', 2)

VAL = {5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A'}
SUITSYM = {0: '♠', 1: '♥', 2: '♣', 3: '♦'}   # S H C D
SU = {0: 'S', 1: 'H', 2: 'C', 3: 'D'}


def card(s, v):
    if v == -1:
        return {'r': '?', 'suit': s, 'trump': False, 'red': False, 'hidden': True, 'str': '??'}
    return {'r': VAL.get(v, '?'), 'suit': s, 'sym': SUITSYM[s], 'trump': (s == TRUMP),
            'red': (s in (1, 3)), 'hidden': False, 'str': VAL.get(v, '?') + SUITSYM[s]}


VALREV = {v: k for k, v in VAL.items()}
SUREV = {'S': 0, 'H': 1, 'C': 2, 'D': 3}


def tok_to_sv(t):
    """Parse an og_explain token ('9C*','10H','AS') into (suit, value)."""
    t = t.rstrip('*')
    suit = SUREV[t[-1]]
    return (suit, VALREV[t[:-1]])


def opp_count(d, rd):
    """Total cards held by everyone except the deciding seat."""
    seat = d['seat']
    return sum(c for i, c in enumerate(d['opp_counts']) if i != seat)


def recorded_label(o):
    """The recorded move rendered in og_explain's token grammar."""
    a = o['action']
    def tk(c):
        return c['r'] + SU[c['suit']] + ('*' if c['trump'] else '')
    if a['kind'] == 'attack':
        return 'attack ' + ' '.join(tk(c) for c in a['cards'])
    if a['kind'] == 'pass':
        return 'pass ' + ' '.join(tk(c) for c in a['cards'])
    if a['kind'] == 'cover':
        return 'cover ' + ' '.join(tk(c) + '->' + tk(t) for c, t in zip(a['cards'], a['targets']))
    if a['kind'] == 'pickup':
        return 'pickup'
    if a['kind'] == 'good':
        return 'good'
    return a['kind']


def norm(s):
    """Normalize a move label so card ORDER doesn't affect equality."""
    p = s.replace(',', ' ').split()
    return p[0] + ' ' + ' '.join(sorted(p[1:]))


# ---- walk the logs: reconstruct the table + per-seat hand counts ------------
# (octogen's belief about hidden cards is dumped by the engine per decision; we
# only reconstruct the public board here for the step-through.)
hc = [6] * NPLAYERS
table = []            # [{attack, defense|None}]
out = []
for i, l in enumerate(logs):
    t, seat = l['t'], l['seat']
    cards = [(x['p']['suit'], x['p']['value'], x['tg']) for x in l['cards']]
    action = None
    if t == 'attack':
        for s, v, tg in cards:
            table.append({'attack': card(s, v), 'defense': None})
        if seat is not None:
            hc[seat] -= len(cards)
        action = {'kind': 'attack', 'seat': seat, 'cards': [card(s, v) for s, v, tg in cards]}
    elif t == 'pass':
        for s, v, tg in cards:
            table.append({'attack': card(s, v), 'defense': None})
        if seat is not None:
            hc[seat] -= len(cards)
        action = {'kind': 'pass', 'seat': seat, 'cards': [card(s, v) for s, v, tg in cards]}
    elif t == 'cover':
        for s, v, tg in cards:
            for b in table:
                if (b['defense'] is None and tg and b['attack']['suit'] == tg['suit']
                        and b['attack']['r'] == VAL.get(tg['value'])):
                    b['defense'] = card(s, v)
                    break
        if seat is not None:
            hc[seat] -= len(cards)
        action = {'kind': 'cover', 'seat': seat, 'cards': [card(s, v) for s, v, tg in cards],
                  'targets': [card(tg['suit'], tg['value']) for s, v, tg in cards if tg]}
    elif t == 'pickup':
        ncard = len(l['cards'])
        if seat is not None:
            hc[seat] += ncard
        table = []
        action = {'kind': 'pickup', 'seat': seat, 'n': ncard,
                  'cards': [card(x['p']['suit'], x['p']['value']) for x in l['cards']]}
    elif t == 'good':
        action = {'kind': 'good', 'seat': seat}
    elif t == 'discard':
        table = []
        action = {'kind': 'discard', 'cards': [card(x['p']['suit'], x['p']['value']) for x in l['cards']]}
    elif t == 'draw':
        n = len(l['cards'])
        if seat is not None:
            hc[seat] += n
        rev = [card(x['p']['suit'], x['p']['value']) for x in l['cards'] if x['p']['value'] != -1]
        action = {'kind': 'draw', 'seat': seat, 'n': n, 'reveal': rev}
    elif t == 'defender_change':
        action = {'kind': 'defender_change', 'def': l['def']}
    elif t == 'player_out':
        action = {'kind': 'player_out', 'seat': seat}
    elif t == 'game_start':
        action = {'kind': 'game_start'}

    rec = None
    if i in by_ply:
        d = by_ply[i]
        # "octogen known state" — octogen's ACTUAL belief, dumped from the engine
        # (og_build_belief), not reconstructed here. For each opponent it PINS the
        # cards it watched them take (still in hand); the rest of their hand plus
        # the face-down deck form the unknown pool it samples over. Pinned +
        # pool == deck + opponent, always; once the deck empties the pool is just
        # the opponent's un-pinned cards, so octogen knows the whole hand — the
        # public deduction the exact endgame solver runs on.
        bel = d.get('belief', {})
        og_seat = d['seat']
        pinned_all = bel.get('pinned', [[]] * NPLAYERS)
        voids_all = bel.get('voids', [[]] * NPLAYERS)
        floors = bel.get('floor', [0] * NPLAYERS)
        pool_toks = bel.get('pool', [])
        oppc = opp_count(d, rd)

        def tcard(tn):
            s, v = tok_to_sv(tn)
            return card(s, v)

        # Per-opponent belief so 3+ player games render each seat octogen has
        # partial knowledge of. (The driver is 2-player today, but the page is
        # not — it loops over however many opponents appear.)
        opps = []
        total_pinned = 0
        for p in range(NPLAYERS):
            if p == og_seat:
                continue
            pinned_p = pinned_all[p]
            total_pinned += len(pinned_p)
            opps.append({'seat': p,
                         'count': d['opp_counts'][p],
                         'pinned': [tcard(t) for t in pinned_p],
                         'voids': [tcard(t) for t in voids_all[p]],
                         'floor': floors[p]})
        # pinned across all opponents + the shared unknown pool == deck + all opps
        assert total_pinned + len(pool_toks) == d['deck'] + oppc, \
            f"ply {i}: pinned {total_pinned} + pool {len(pool_toks)} != deck {d['deck']} + opp {oppc}"
        rec = {'ply': i, 'hand': d['hand'], 'table': d['table'], 'opp_counts': d['opp_counts'],
               'deck': d['deck'], 'solver': d['solver'], 'candidates': d['candidates'],
               'chosen': d['chosen'],
               'known': {'opps': opps,
                         'pinned_total': total_pinned,
                         'pool': [tcard(t) for t in pool_toks],
                         'deck': d['deck'], 'opp_count': oppc}}
    out.append({'i': i, 't': t, 'seat': seat, 'def': l.get('def'), 'action': action,
                'table': [{'a': b['attack'], 'd': b['defense']} for b in table],
                'hc': list(hc), 'decision': rec})

# ---- agreement stats + auto-flagged (octogen would differ, not forced) ------
match = forced = total = 0
flagged = []
for o in out:
    d = o['decision']
    if not d:
        continue
    total += 1
    rec = recorded_label(o)
    if len(d['candidates']) == 1:
        forced += 1
    if norm(rec) == norm(d['chosen']):
        match += 1
    else:
        if len(d['candidates']) > 1:
            flagged.append(o['i'])

data = {
    'meta': {
        'players': NPLAYERS, 'trump': TRUMP, 'trumpSym': SUITSYM[TRUMP],
        'seed': SEED, 'firstAttacker': FIRST, 'fool': FOOL,
        'winner': ELIM[0] if ELIM else FIRST,
        'ogSeat': delib[0]['seat'] if delib else 1,
        'flip': card(FLIP['suit'], FLIP['value']),
        'nlogs': len(logs),
        'decisions': total, 'match': match, 'forced': forced,
    },
    'flagged': flagged,
    'logs': out,
}
json.dump(data, open(OUT, 'w'))
print(f"logs {len(out)} decisions {total} match {match} forced {forced} flagged {flagged} -> {OUT}",
      file=sys.stderr)
