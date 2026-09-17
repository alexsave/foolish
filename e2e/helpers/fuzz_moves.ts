// fuzz_moves.ts - the adversarial action generator of e2e/fuzz.test.ts, shared.
//
// Moved out of fuzz.test.ts unchanged so a second suite can drive the SAME
// hostile inputs through another pipeline (e2e/table_parity.test.ts holds the C
// Table to today's TS pipeline on them). A test file cannot be imported for its
// helpers: importing it registers its tests. The generators draw from an
// explicit RNG, so fuzz.test.ts reproduces a found exploit from FUZZ_SEED
// exactly as before.

import { Card, Game, PLAYER_STATUS } from '../../server/api/core/types.ts';

export interface FuzzRng {
    rnd(): number;
    ri(n: number): number;
    pick<T>(a: T[]): T;
}

// Deterministic LCG, so a found exploit reproduces from the printed seed.
export function fuzzRng(seed: number): FuzzRng {
    let s = seed >>> 0;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 0x100000000; };
    const ri = (n: number) => Math.floor(rnd() * n);
    return { rnd, ri, pick: <T>(a: T[]): T => a[ri(a.length)] };
}

export interface FuzzReq { type: string; player_id: string; cards?: any; cover_cards?: any; attack_cards?: any }

// Adversarial request generators against the current state. `uuid` names a
// player who is not in the game.
export function fuzzGenerators(r: FuzzRng, uuid: () => string): ((g: Game) => FuzzReq)[] {
    const { ri, pick } = r;
    const garbageCard = (): Card => ({ suit: pick([-1, 0, 1, 2, 3, 7, 99]), value: pick([-1, 0, 1, 9, 13, 14, 99]) });
    const someHandCard = (g: Game): Card | null => {
        const withHands = g.players.filter((p) => p.hand && p.hand.length);
        if (!withHands.length) return null;
        return pick(pick(withHands).hand);
    };
    const attackerId = (g: Game): string => {
        const atks = g.players.filter((_, i) => i !== g.defender && g.players[i].status === PLAYER_STATUS.IN);
        return (atks.length ? pick(atks) : g.players[0]).player_id;
    };
    return [
        // 1) DUPLICATE identical card in one attack - the object-identity dedup hole.
        (g) => { const c = someHandCard(g) ?? garbageCard(); return { type: 'attack', player_id: g.players[g.first_attacker].player_id, cards: [{ ...c }, { ...c }] }; },
        // 2) duplicate identical cover card
        (g) => { const c = someHandCard(g) ?? garbageCard(); const a = g.table_battles[0]?.attack ?? garbageCard(); return { type: 'cover', player_id: g.players[g.defender].player_id, cover_cards: [{ ...c }, { ...c }], attack_cards: [{ ...a }, { ...a }] }; },
        // 3) duplicate identical pass card
        (g) => { const c = someHandCard(g) ?? garbageCard(); return { type: 'pass', player_id: g.players[g.defender].player_id, cards: [{ ...c }, { ...c }] }; },
        // 4) forged card not in hand
        (g) => ({ type: 'attack', player_id: attackerId(g), cards: [{ suit: ri(4), value: 1 + ri(13) }] }),
        // 5) out-of-range garbage card
        (g) => ({ type: 'attack', player_id: attackerId(g), cards: [garbageCard()] }),
        // 6) wrong role: defender attacks
        (g) => { const c = someHandCard(g) ?? garbageCard(); return { type: 'attack', player_id: g.players[g.defender].player_id, cards: [{ ...c }] }; },
        // 7) wrong role: an attacker tries to cover/pickup
        (g) => ({ type: pick(['cover', 'pickup']), player_id: attackerId(g), cover_cards: [garbageCard()], attack_cards: [garbageCard()] }),
        // 8) player not in the game
        (g) => ({ type: pick(['attack', 'cover', 'pass', 'pickup', 'good']), player_id: uuid(), cards: [garbageCard()], cover_cards: [garbageCard()], attack_cards: [garbageCard()] }),
        // 9) empty / null / huge payloads
        (g) => ({ type: 'attack', player_id: attackerId(g), cards: pick([[], null, undefined, Array(20).fill(someHandCard(g) ?? garbageCard())]) }),
        // 10) mixed-value first attack
        (g) => { const h = g.players[g.first_attacker].hand; return { type: 'attack', player_id: g.players[g.first_attacker].player_id, cards: h.length >= 2 ? [h[0], h[1]] : [garbageCard(), garbageCard()] }; },
        // 11) cover with non-covering / off-table attack_cards
        (g) => ({ type: 'cover', player_id: g.players[g.defender].player_id, cover_cards: [someHandCard(g) ?? garbageCard()], attack_cards: [garbageCard()] }),
        // 12) good by the defender / out of turn
        (g) => ({ type: 'good', player_id: g.players[g.defender].player_id }),
        // 13) mismatched cover/attack array lengths
        (g) => ({ type: 'cover', player_id: g.players[g.defender].player_id, cover_cards: [someHandCard(g) ?? garbageCard()], attack_cards: [] }),
        // 14) malformed types: cards is a string / object / number instead of an array
        (g) => ({ type: 'attack', player_id: attackerId(g), cards: pick(['not-an-array', { suit: 0, value: 5 }, 42, true]) as any }),
        // 15) card fields are strings / objects / nested junk
        (g) => ({ type: 'attack', player_id: attackerId(g), cards: [{ suit: '0' as any, value: '5' as any }, { suit: {} as any, value: [] as any }] }),
        // 16) injection-ish strings in player_id / type (parameterized queries must shrug)
        (g) => ({ type: pick(["attack'; DROP TABLE games;--", '__proto__', 'constructor']) as any, player_id: pick(["1' OR '1'='1", "'; DELETE FROM player_hands; --", '../../etc/passwd']) }),
        // 17) bounded-large payload (DoS attempt - must stay bounded, not hang/OOM)
        (g) => ({ type: 'attack', player_id: g.players[g.first_attacker].player_id, cards: Array(300).fill(0).map(() => ({ ...(someHandCard(g) ?? garbageCard()) })) }),
        // 18) null / missing required fields
        (g) => ({ type: pick(['attack', 'cover', 'pass']), player_id: pick([null, undefined, '']) as any, cards: null, cover_cards: null, attack_cards: null }),
    ];
}
