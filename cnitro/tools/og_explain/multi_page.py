#!/usr/bin/env python3
# Render a MULTI-BOT replay X-ray: a decoded replay + a mixed deliberation dump
# (octogen OG_EXPLAIN records for the octogen seats, {legal,chosen} records for
# the random seats) -> one self-contained interactive page showing what EVERY bot
# was thinking at its own turns. Octogen seats get the full Monte-Carlo / endgame
# / belief X-ray; random seats get their legal-move menu with the pick they made
# highlighted (uniform 1/N). Nothing about the specific game is hardcoded.
#
#   multi_page.py replay_decoded.json delib.jsonl <seed> <octo_seats_csv> out.html
import json, sys

rd = json.load(open(sys.argv[1]))
delib = [json.loads(l) for l in open(sys.argv[2]) if l.strip()]
SEED = sys.argv[3]
OCTO = set(int(x) for x in sys.argv[4].split(',') if x != '')
OUT = sys.argv[5]

logs = rd['logs']
by_ply = {r['ply']: r for r in delib}
TRUMP = rd['powerSuit']
FLIP = rd['trumpCard']
FIRST = rd['firstAttacker']
FOOL = rd['fool']
ELIM = rd['eliminationOrder']
NP = rd.get('playerCount', 2)
TRUMP_KEEP = 0.040

VAL = {1: '2', 2: '3', 3: '4', 4: '5', 5: '6', 6: '7', 7: '8', 8: '9', 9: '10',
       10: 'J', 11: 'Q', 12: 'K', 13: 'A'}
SUITSYM = {0: '♠', 1: '♥', 2: '♣', 3: '♦'}
SU = {0: 'S', 1: 'H', 2: 'C', 3: 'D'}
VALREV = {v: k for k, v in VAL.items()}
SUREV = {'S': 0, 'H': 1, 'C': 2, 'D': 3}


def card(s, v):
    known_s = isinstance(s, int) and 0 <= s <= 3
    r = VAL.get(v, '?')
    sym = SUITSYM[s] if known_s else '?'
    return {'r': r, 'suit': s if known_s else -1, 'sym': sym,
            'trump': (known_s and s == TRUMP), 'red': (known_s and s in (1, 3)),
            'hidden': (r == '?'), 'str': (r + sym) if (r != '?' or known_s) else '??'}


def tok_to_sv(t):
    t = t.rstrip('*')
    suit = SUREV.get(t[-1:], -1)
    return (suit, VALREV.get(t[:-1], -1))


def tcard(tn):
    s, v = tok_to_sv(tn)
    return card(s, v)


def recorded_label(action):
    a = action

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
    p = s.replace(',', ' ').split()
    return p[0] + ' ' + ' '.join(sorted(p[1:]))


# ---- walk the logs: reconstruct the public board + per-seat hand counts -------
hc = [6] * NP
table = []
out = []
for i, l in enumerate(logs):
    t, seat = l['t'], l['seat']
    cards = [(x['p']['suit'], x['p']['value'], x['tg']) for x in l['cards']]
    action = None
    if t == 'attack' or t == 'pass':
        for s, v, tg in cards:
            table.append({'attack': card(s, v), 'defense': None})
        if seat is not None:
            hc[seat] -= len(cards)
        action = {'kind': t, 'seat': seat, 'cards': [card(s, v) for s, v, tg in cards]}
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
        recmove = recorded_label(action) if action else '?'
        if d.get('kind') == 'random':
            legal = d['legal']
            chosen = d['chosen']
            rec = {'kind': 'random', 'seat': seat, 'ply': i,
                   'legal': legal, 'chosen': chosen,
                   'choiceCount': len(legal), 'recorded': recmove,
                   'opp_counts': d.get('opp_counts', []), 'deck': d.get('deck', 0)}
        else:
            og_seat = d['seat']
            bel = d.get('belief', {})
            pinned_all = bel.get('pinned', [[]] * NP)
            voids_all = bel.get('voids', [[]] * NP)
            floors = bel.get('floor', [0] * NP)
            pool_toks = bel.get('pool', [])
            oppc = sum(c for j, c in enumerate(d['opp_counts']) if j != og_seat)
            opps = []
            total_pinned = 0
            for p in range(NP):
                if p == og_seat:
                    continue
                pinned_p = pinned_all[p]
                total_pinned += len(pinned_p)
                opps.append({'seat': p, 'count': d['opp_counts'][p],
                             'pinned': [tcard(x) for x in pinned_p],
                             'voids': [tcard(x) for x in voids_all[p]],
                             'floor': floors[p]})
            deck_alive = d['deck'] > 0
            for c in d['candidates']:
                n_trump = sum(1 for cd in c.get('cards', []) if str(cd).endswith('*'))
                tax = TRUMP_KEEP * n_trump if (deck_alive and c.get('type') == 'attack') else 0.0
                c['trumpTax'] = round(tax, 4)
                c['adjScore'] = round(c['score'] + tax, 4) if c.get('score') is not None else None
            match = norm(recmove) == norm(d['chosen'])
            rec = {'kind': 'octogen', 'seat': og_seat, 'ply': i,
                   'hand': d['hand'], 'table': d['table'], 'opp_counts': d['opp_counts'],
                   'deck': d['deck'], 'solver': d['solver'], 'candidates': d['candidates'],
                   'chosen': d['chosen'], 'recorded': recmove, 'match': match,
                   'forced': len(d['candidates']) <= 1,
                   'known': {'opps': opps, 'pinned_total': total_pinned,
                             'pool': [tcard(x) for x in pool_toks],
                             'deck': d['deck'], 'opp_count': oppc}}
    out.append({'i': i, 't': t, 'seat': seat, 'def': l.get('def'), 'action': action,
                'table': [{'a': b['attack'], 'd': b['defense']} for b in table],
                'hc': list(hc), 'decision': rec})

# ---- standings + stats -------------------------------------------------------
place_of = {s: k + 1 for k, s in enumerate(ELIM)}
place_of[FOOL] = NP  # not eliminated = fool = last
standings = sorted(range(NP), key=lambda s: place_of.get(s, NP))
octo_dec = octo_match = rand_dec = 0
octo_differ = []
for o in out:
    d = o['decision']
    if not d:
        continue
    if d['kind'] == 'octogen':
        octo_dec += 1
        if d['match']:
            octo_match += 1
        elif not d['forced']:
            octo_differ.append(o['i'])
    else:
        rand_dec += 1

data = {
    'meta': {
        'players': NP, 'trump': TRUMP, 'trumpSym': SUITSYM[TRUMP],
        'seed': SEED, 'firstAttacker': FIRST, 'fool': FOOL,
        'octoSeats': sorted(OCTO), 'winner': ELIM[0] if ELIM else FIRST,
        'standings': [{'seat': s, 'place': place_of.get(s, NP),
                       'kind': 'octogen' if s in OCTO else 'random'} for s in standings],
        'flip': card(FLIP['suit'], FLIP['value']),
        'nlogs': len(logs), 'octoDecisions': octo_dec, 'octoMatch': octo_match,
        'randDecisions': rand_dec, 'trumpKeep': TRUMP_KEEP,
    },
    'octoDiffer': octo_differ,
    'logs': out,
}

sys.stderr.write(f"multi_page: {len(out)} logs, {octo_dec} octogen ({octo_match} match) + "
                 f"{rand_dec} random decisions, differ={octo_differ}\n")

# ---- render ------------------------------------------------------------------
from multi_render import render  # noqa: E402
html = render(data)
open(OUT, 'w').write(html)
sys.stderr.write(f"wrote {OUT} ({len(html)} bytes)\n")
