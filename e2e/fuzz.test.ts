// Adversarial / illegal-input fuzzer. Fires malformed and rule-breaking action
// requests — the kind a malicious or buggy client could POST — through the REAL
// server validation+execution path (verify_player_in_game + the real handlers,
// exactly as server/impls/supabase/functions/action/index.ts dispatches) under the REAL CAS
// commit, and asserts the hard safety invariant after EVERY attempt:
//
//   card conservation holds — no input ever duplicates or loses a card.
//
// Plus targeted checks that obviously-illegal inputs are rejected, not applied.

import './harness.ts';
import { test, before, beforeEach, after } from 'node:test';
import assert from 'node:assert/strict';
import { applySchema, resetDb, seedGame, uuid, pgPool } from './harness.ts';
import { executeWithGameLock, loadCompleteGame } from '../server/impls/supabase/functions/_shared/adapter/utils.ts';
import { verify_player_in_game } from '../server/api/common/common_utils.ts';
import { packedProducts, start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { Game, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, PrivatePlayer, Card } from '../server/api/core/types.ts';
import { handleAttack } from '../server/api/common/actions/attack.ts';
import { handleCover } from '../server/api/common/actions/cover.ts';
import { handlePass } from '../server/api/common/actions/pass.ts';
import { handlePickup } from '../server/api/common/actions/pickup.ts';
import { handleGood } from '../server/api/common/actions/good.ts';
import { legalMovesFor, applyPlayerMove, checkCardConservation } from './dispatch.ts';
import { FuzzReq, fuzzGenerators, fuzzRng } from './helpers/fuzz_moves.ts';

// Deterministic RNG so a found exploit reproduces from the printed seed. The
// generators live in e2e/helpers/fuzz_moves.ts so e2e/table_parity.test.ts can
// drive the same hostile inputs through the C Table.
const rng = fuzzRng(Number(process.env.FUZZ_SEED || 0x1234abcd));
const { rnd, pick } = rng;

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

// Adversarial request generators against the current state.
const GENERATORS = fuzzGenerators(rng, uuid);

// Distinguish a clean rule rejection from a crash-class error (the kind that would
// be a confusing 500 if wrap400 didn't catch-all): TypeError/RangeError or a
// low-level "cannot read undefined / not a function / stack" message.
function isCrashClass(e: any): boolean {
    if (e instanceof RangeError) return true;
    const m = String(e?.message ?? e);
    return /cannot read|is not a function|is not iterable|maximum call stack|out of memory|reading '/i.test(m);
}

// loadCompleteGame is a top-level import, not an `await import` per call: under
// the e2e runner's TS loader a dynamic import re-runs the resolver every time
// (~1.9ms) even for a module already in the registry, and this is called once per
// fuzz iteration.
const loadGame = (gameId: string): Promise<Game> => loadCompleteGame(gameId);
async function freshGame(): Promise<string> {
    const gameId = `f${uuid().slice(0, 6)}`;
    await seedGame(gameId, [
        { id: uuid(), name: 'H0', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'H1', is_ai: false, strategy_key: 'human' },
        { id: uuid(), name: 'B0', is_ai: true, strategy_key: 'random' },
    ]);
    await executeWithGameLock(gameId, async (g) => ({ game: g, events: [], packed: packedProducts(start_game_packed(g)) }), 'start', false);
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
        const g = mkGame([player('atk', hand.slice()), player('def', [card(3, 13)])], 1);
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
            if (moves.length) { try { await executeWithGameLock(gameId, async (gg) => ({ game: gg, ...applyPlayerMove(gg, pick(moves)) }), `m${i}`, true); } catch { /* */ } }
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
