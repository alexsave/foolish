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


# The full 36-card durak deck: 9 ranks (values 5..13 = 6..A) x 4 suits.
FULL_DECK = {(s, v) for s in range(4) for v in range(5, 14)}

# ---- walk the logs: reconstruct table + hand counts + the discard pile ------
# "Visible to octogen" at any instant = its own hand + the flip + the discard
# pile + whatever is on the table right now. Everything else (the face-down
# deck + the opponent's hand) is hidden. A card the opponent PICKS UP leaves the
# table and becomes hidden again — so we track the growing discard set and the
# CURRENT table, never a cumulative "ever seen" set (which would wrongly keep
# picked-up cards visible).
hc = [6] * NPLAYERS
table = []            # [{attack, defense|None}]
discard = set()       # (suit,value) of every card sent to the discard pile
FLIP_SV = (FLIP['suit'], FLIP['value'])
out = []
for i, l in enumerate(logs):
    t, seat = l['t'], l['seat']
    cards = [(x['p']['suit'], x['p']['value'], x['tg']) for x in l['cards']]
    if t == 'discard':
        for x in l['cards']:
            if x['p']['value'] != -1:
                discard.add((x['p']['suit'], x['p']['value']))
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
        # "octogen known state": the cards HIDDEN from octogen right now = the
        # whole deck minus what it holds, the flip, the discard pile, and the
        # cards on the table. That hidden pool is split between the face-down
        # deck and the opponent's hand: |pool| == deck + opponent_cards exactly.
        # When the deck empties, the pool collapses onto the opponent's hand —
        # public deduction — which is what lets the endgame solver PROVE the line.
        og_hand = {tok_to_sv(tn) for tn in d['hand']}
        tbl = set()
        for b in d['table']:
            tbl.add(tok_to_sv(b['attack']))
            if b.get('defense'):
                tbl.add(tok_to_sv(b['defense']))
        visible = og_hand | discard | {FLIP_SV} | tbl
        pool = sorted(FULL_DECK - visible)
        oppc = opp_count(d, rd)
        assert len(pool) == d['deck'] + oppc, \
            f"ply {i}: pool {len(pool)} != deck {d['deck']} + opp {oppc}"
        rec = {'ply': i, 'hand': d['hand'], 'table': d['table'], 'opp_counts': d['opp_counts'],
               'deck': d['deck'], 'solver': d['solver'], 'candidates': d['candidates'],
               'chosen': d['chosen'],
               'known': {'pool': [card(s, v) for (s, v) in pool],
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
