// Adversarial / illegal-input fuzzer. Fires malformed and rule-breaking action
// requests — the kind a malicious or buggy client could POST — through the REAL
// server validation+execution path (verify_player_in_game + the real handlers,
// exactly as supabase/functions/action/index.ts dispatches) under the REAL CAS
// commit, and asserts the hard safety invariant after EVERY attempt:
//
//   card conservation holds — no input ever duplicates or loses a card.
//
// Plus targeted checks that obviously-illegal inputs are rejected, not applied.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock } from '../supabase/functions/_shared/utils.ts';
import { verify_player_in_game } from '../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { Game, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, PrivatePlayer, Card } from '../supabase/functions/_shared/types.ts';
import { handleAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/actions/good.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation } from './dispatch.ts';

// Deterministic RNG so a found exploit reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0x1234abcd) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
const pick = <T>(a: T[]): T => a[ri(a.length)];

interface FuzzReq { type: string; player_id: string; cards?: any; cover_cards?: any; attack_cards?: any }

// The REAL action-endpoint dispatch (mirrors action/index.ts): membership check
// then the real handler. No swallowing — illegal input must surface as a throw.
function applyAction(game: Game, req: FuzzReq): AnimationEvent[] {
    verify_player_in_game(game, req.player_id);
    switch (req.type) {
        case 'attack': return handleAttack(game, req.player_id, req.cards);
        case 'cover': return handleCover(game, req.player_id, req.cover_cards, req.attack_cards);
        case 'pass': return handlePass(game, req.player_id, req.cards);
        case 'pickup': return handlePickup(game, req.player_id);
        case 'good': return handleGood(game, req.player_id);
        default: throw new Error(`unknown action type: ${req.type}`);
    }
}

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

// Adversarial request generators against the current state.
const GENERATORS: ((g: Game) => FuzzReq)[] = [
    // 1) DUPLICATE identical card in one attack — the object-identity dedup hole.
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
    // 17) bounded-large payload (DoS attempt — must stay bounded, not hang/OOM)
    (g) => ({ type: 'attack', player_id: g.players[g.first_attacker].player_id, cards: Array(300).fill(0).map(() => ({ ...(someHandCard(g) ?? garbageCard()) })) }),
    // 18) null / missing required fields
    (g) => ({ type: pick(['attack', 'cover', 'pass']), player_id: pick([null, undefined, '']) as any, cards: null, cover_cards: null, attack_cards: null }),
];

// Distinguish a clean rule rejection from a crash-class error (the kind that would
// be a confusing 500 if wrap400 didn't catch-all): TypeError/RangeError or a
// low-level "cannot read undefined / not a function / stack" message.
function isCrashClass(e: any): boolean {
    if (e instanceof RangeError) return true;
    const m = String(e?.message ?? e);
    return /cannot read|is not a function|is not iterable|maximum call stack|out of memory|reading '/i.test(m);
}

async function loadGame(gameId: string): Promise<Game> {
    const { loadCompleteGame } = await import('../supabase/functions/_shared/utils.ts');
    return loadCompleteGame(gameId);
}
async function freshGame(): Promise<string> {
    const gameId = `f${uuid().slice(0, 6)}`;
    await seedGame(gameId, [
        { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
    ]);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: start_game(g) as AnimationEvent[] }), 'start', false);
    return gameId;
}

// ---- handpicked, pure validation (no DB): the always-reject invariants -------
export function registerAttackValidation(): void {
    const card = (suit: number, value: number): Card => ({ suit, value });
    const player = (id: string, hand: Card[]): PrivatePlayer => ({
        player_id: id, name: id, status: PLAYER_STATUS.IN, is_ai: false,
        hand, awaiting_attack: false, hand_length: hand.length, strategy_key: STRATEGY_KEY.HUMAN,
    });
    const mkGame = (players: PrivatePlayer[], defender = 1): Game => ({
        id: 'g', name: 'g', deck_length: 0, discard_pile_length: 0, flipped: null,
        status: GAME_STATUS.PLAYING, power_suit: 0, first_attacker: 0, defender,
        table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], deck: [], logs: [], players,
    });

    test('attack: forged card, identical-duplicate, and non-member attacks are all rejected', () => {
        const hand = [card(0, 10), card(1, 10), card(2, 12)];
        const g = mkGame([player('atk', hand.slice()), player('def', [card(3, 14)])], 1);
        // forged card not in hand
        assert.throws(() => handleAttack(g, 'atk', [card(3, 9)]), /not in/i, 'forged card');
        // the object-identity duplicate hole: [X, X] must be rejected, never duplicated
        const x = card(0, 10);
        assert.throws(() => handleAttack(g, 'atk', [{ ...x }, { ...x }]), /duplicate/i, 'identical duplicate');
        // a player who isn't in the game
        assert.throws(() => handleAttack(g, 'ghost', [card(0, 10)]), /not found in game/i, 'non-member');
    });
}

if (!process.env.VALIDATION_ONLY) {
before(async () => { await applySchema(); });
beforeEach(async () => { await resetDb(); });

test('adversarial fuzz: no illegal/malformed input ever duplicates or loses a card', async () => {
    const ITER = Number(process.env.FUZZ_ITERS || 3000);
    const violations: string[] = [];
    let gameId = await freshGame();
    let attempts = 0, rejected = 0, committed = 0, crashClass = 0;

    for (let i = 0; i < ITER; i++) {
        let g = await loadGame(gameId);
        if (g.status !== 'playing') { gameId = await freshGame(); g = await loadGame(gameId); }

        // 35% legal move to keep the game evolving through phases; else adversarial.
        if (rnd() < 0.35) {
            const moves = legalMovesFor(g);
            if (moves.length) { try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyPlayerMove(gg, pick(moves)) }), `m${i}`, true); } catch { /* */ } }
            continue;
        }

        attempts++;
        const req = pick(GENERATORS)(g);
        try {
            await executeWithGameLock(gameId, async (gg) => ({ game: gg, events: applyAction(gg, req) }), `f${i}`, true);
            committed++;
        } catch (e) {
            rejected++; // rejection is the desired outcome for illegal input
            if (isCrashClass(e)) crashClass++;
        }

        const chk = await checkCardConservation(gameId);
        if (!chk.ok) violations.push(`seed=${process.env.FUZZ_SEED || '0x1234abcd'} iter=${i} req=${JSON.stringify(req)} -> ${chk.detail}`);
    }

    // Hard invariant: no adversarial input may ever duplicate or lose a card.
    assert.equal(violations.length, 0, `card conservation broken by adversarial input:\n  ${violations.slice(0, 3).join('\n  ')}`);
    assert.ok(attempts > 100 && rejected > 0, `fuzz ran (attempts=${attempts} rejected=${rejected} committed=${committed} crashClass=${crashClass})`);
    // Malformed payloads must now produce clean rule rejections, not low-level
    // TypeErrors — the input guards make crash-class errors impossible.
    assert.equal(crashClass, 0, `malformed input produced ${crashClass} ungraceful crash-class error(s)`);
    // The process surviving ITER hostile requests IS the no-crash assertion.
    console.error(`[fuzz] attempts=${attempts} rejected=${rejected} committed=${committed} crashClass=${crashClass}`);
});

test('targeted: forged cards and non-members are always rejected', async () => {
    const gameId = await freshGame();
    const g = await loadGame(gameId);
    const attacker = g.players[g.first_attacker].player_id;
    // A forged card = a real card the attacker does not hold. (An out-of-range
    // {99,99} is WRONG here: the marshal clamp maps it onto the ace of
    // diamonds, and on deals where the attacker holds that card the "forged"
    // attack is legitimately legal — a 1-in-6 flake.)
    const hand = g.players[g.first_attacker].hand as { suit: number; value: number }[];
    let forged: { suit: number; value: number } | null = null;
    for (let s = 0; s < 4 && !forged; s++)
        for (let v = 5; v <= 13 && !forged; v++)
            if (!hand.some((c) => c.suit === s && c.value === v)) forged = { suit: s, value: v };
    assert.throws(() => applyAction(g, { type: 'attack', player_id: attacker, cards: [forged!] }), /not in/i, 'forged card rejected');
    // a player who isn't in the game
    assert.throws(() => applyAction(g, { type: 'attack', player_id: uuid(), cards: [g.players[g.first_attacker].hand[0]] }), /not in/i, 'non-member rejected');
});

test('regression: sending the same card twice in one move is rejected (no duplication)', async () => {
    const gameId = await freshGame();
    const g = await loadGame(gameId);
    const fa = g.players[g.first_attacker];
    const dup = fa.hand[0];
    // attack with [X, X] — the object-identity dedup hole — must be rejected.
    assert.throws(() => applyAction(g, { type: 'attack', player_id: fa.player_id, cards: [{ ...dup }, { ...dup }] }), /duplicate/i, 'duplicate attack rejected');
    // and the durable state is untouched (the throw happened before any commit).
    const chk = await checkCardConservation(gameId);
    assert.ok(chk.ok, `state intact after rejected duplicate: ${chk.detail}`);
});

registerAttackValidation();

after(async () => { await pgPool.end(); });
}
