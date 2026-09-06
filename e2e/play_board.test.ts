/* =============================================================================
 * WHAT A GESTURE MEANS: the kernel's answer, reached from TypeScript
 * =============================================================================
 * Five web sites used to answer "what does this gesture mean" in slightly
 * different words - DragContext.determineGameAction, ActionButtons'
 * handleCoverClick and its good gate, KeyboardInputHandler and KeyboardPlayMode.
 * c/src/legal.h's play_* rules are one answer for all of them, and this proves
 * the TS side of that crossing.
 *
 * The rules take a PUBLISHED PAIR - the menu the kernel enumerated for a seat,
 * and the table it was enumerated on - and read nothing else. So the menu is
 * produced first (kernelMenuWire) and everything after is pure argument.
 *
 * Pure kernel/wasm test - needs no Postgres.
 *
 * MUTATION-CHECKED (2026-09-06), each applied, run, and reverted:
 *   writePlayBoard spells "no cover" 0xff (engine.ts WIRE_NONE) instead of
 *   legal.h's 0xfe
 *       -> "every single-card cover in the menu resolves onto its own battle"
 *          fails. This one was a real bug caught before it shipped, not a
 *          hypothetical: 0xff makes battle_is_uncovered false everywhere, so
 *          the board answers "nothing is coverable" and answers it silently.
 *   playCoverableBattles reads the mask as one byte instead of eight
 *       -> NOTHING fails, and the eight-byte read stays anyway. A bout caps at
 *          six attacks, so no reachable board carries a battle index past 7 and
 *          no test can honestly discriminate this. Recorded rather than
 *          answered with a board the game cannot deal.
 *   play_best_cover_target picks the leftmost coverable battle (the web's old
 *   rule) instead of the highest
 *       -> "the cover button aims at the highest attack, not the leftmost"
 *          fails
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { start_game } from '../server/api/common/game_lifecycle.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { game_done } from '../server/api/common/common_utils.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import {
    kernelMenuWire, playResolve, playCoverableBattles, playBestCoverTarget,
    playCanSayGood, PLAY_TARGET_TABLE, PLAY_TARGET_HAND, PlayBoard,
} from '../sdk/ts/wasm/bots.ts';
import {
    Game, Card, Battle, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {}; console.warn = () => {}; console.error = () => {};
}

const C = (suit: number, value: number): Card => ({ suit, value });
const sameCard = (a: Card, b: Card) => a.suit === b.suit && a.value === b.value;

const mkPlayer = (i: number): PrivatePlayer => ({
    player_id: `seat-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: STRATEGY_KEY.RANDOM,
});

const seedBytes = (np: number, s: number): Uint8Array =>
    new Uint8Array(Array.from({ length: 32 }, (_, i) => (i * 31 + s * 13 + np) & 0xff));
const seedHex = (b: Uint8Array) =>
    Array.from(b).map((x) => x.toString(16).padStart(2, '0')).join('');

const mkGame = (np: number, gameSeed?: string): Game => ({
    players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
    deck: [], logs: [], id: 'pb', name: 'pb', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [], game_seed: gameSeed ?? null,
} as unknown as Game);

const boardFor = (g: Game, seat: number): PlayBoard => ({
    menu: kernelMenuWire(g, seat),
    battles: g.table_battles,
    powerSuit: g.power_suit,
    isDefender: g.defender === seat,
});

/** Walk a seeded game, handing every mid-game position to `visit`. */
async function walkPositions(np: number, s: number, visit: (g: Game) => void): Promise<void> {
    const seed = seedBytes(np, s);
    const game = mkGame(np, seedHex(seed));
    __setDealSeedOverride(seed);
    try {
        start_game(game);
        for (let guard = 0; guard < 4000 && game_done(game) === null; guard++) {
            visit(game);
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return;
        }
    } finally {
        __setDealSeedOverride(null);
    }
}

test('every single-card cover in the menu resolves onto its own battle', async () => {
    let checked = 0;
    for (let s = 0; s < 4; s++) {
        await walkPositions(3, s, (g) => {
            const seat = g.defender;
            const board = boardFor(g, seat);
            const moves = calculateLegalMoves(g, g.players[seat].player_id);

            for (const m of moves) {
                if (m.type !== 'cover' || !m.cards || m.cards.length !== 1) continue;
                const attack = m.attack_cards![0];
                const battleIndex = g.table_battles.findIndex(
                    (b: Battle) => !b.defense && sameCard(b.attack, attack));
                if (battleIndex < 0) continue;

                const hit = playResolve(board, m.cards, battleIndex);
                assert.ok(hit, 'a legal cover drop resolves');
                assert.equal(hit!.move.type, 'cover', 'and it resolves to a cover');
                assert.ok(sameCard(hit!.move.cards![0], m.cards[0]), 'with the dragged card');
                assert.ok(hit!.move.attack_cards!.some((a: Card) => sameCard(a, attack)),
                    'onto the battle it was dropped on');
                checked++;
            }
        });
    }
    assert.ok(checked > 40, `enough covers exercised (${checked})`);
});

test('the coverable set is the menu\'s own cover set', async () => {
    let checked = 0;
    for (let s = 0; s < 4; s++) {
        await walkPositions(3, s, (g) => {
            const seat = g.defender;
            if (g.table_battles.length === 0) return;
            const board = boardFor(g, seat);
            const hand = g.players[seat].hand;

            for (const card of hand) {
                const kernelSet = playCoverableBattles(board, [card]);
                // The same question asked of the menu directly: which uncovered
                // battles does a single-card cover with THIS card name?
                const fromMenu = new Set<number>();
                for (const m of calculateLegalMoves(g, g.players[seat].player_id)) {
                    if (m.type !== 'cover' || !m.cards || m.cards.length !== 1) continue;
                    if (!sameCard(m.cards[0], card)) continue;
                    g.table_battles.forEach((b: Battle, i: number) => {
                        if (!b.defense && sameCard(b.attack, m.attack_cards![0])) fromMenu.add(i);
                    });
                }
                assert.deepEqual([...kernelSet].sort((a, b) => a - b),
                    [...fromMenu].sort((a, b) => a - b),
                    'the kernel set is the menu set');
                checked++;
            }
        });
    }
    assert.ok(checked > 100, `enough selections exercised (${checked})`);
});

test('the cover button aims at the highest attack, not the leftmost', () => {
    // The behaviour change this PR lands. Trump is spades (suit 3). The table
    // holds a low attack first and a higher one second, and the selection beats
    // both. The web used to take the leftmost - "the order the attackers
    // happened to throw in" - and the kernel takes the highest.
    const g = mkGame(2);
    g.power_suit = 3;
    g.status = GAME_STATUS.PLAYING;
    g.first_attacker = 0;
    g.defender = 1;
    g.players[0].status = PLAYER_STATUS.IN;
    g.players[1].status = PLAYER_STATUS.IN;
    g.players[0].hand = [C(0, 5), C(0, 10)];
    g.players[0].hand_length = 2;
    g.players[1].hand = [C(0, 12)];
    g.players[1].hand_length = 1;
    g.table_battles = [
        { attack: C(0, 5), defense: null },
        { attack: C(0, 10), defense: null },
    ];

    const board = boardFor(g, 1);
    const sel = [C(0, 12)];

    assert.deepEqual(playCoverableBattles(board, sel), [0, 1], 'the queen beats both');
    assert.equal(playBestCoverTarget(board, sel), 1,
        'the button aims at the ten (battle 1), not the five (battle 0)');
});

test('a trump outranks every non-trump when the button chooses', () => {
    // Strength is not rank alone: a low trump on the table outranks a high
    // non-trump, so the button must aim at the trump even though it is second
    // and lower by pip.
    const g = mkGame(2);
    g.power_suit = 3;
    g.status = GAME_STATUS.PLAYING;
    g.first_attacker = 0;
    g.defender = 1;
    g.players[0].status = PLAYER_STATUS.IN;
    g.players[1].status = PLAYER_STATUS.IN;
    g.players[0].hand = [C(0, 12), C(3, 4)];
    g.players[0].hand_length = 2;
    g.players[1].hand = [C(3, 9)];
    g.players[1].hand_length = 1;
    g.table_battles = [
        { attack: C(0, 12), defense: null },   // queen of a plain suit
        { attack: C(3, 4), defense: null },    // four of trumps
    ];

    const board = boardFor(g, 1);
    const sel = [C(3, 9)];
    assert.deepEqual(playCoverableBattles(board, sel), [0, 1], 'the trump nine beats both');
    assert.equal(playBestCoverTarget(board, sel), 1, 'the trump four outranks the plain queen');
});

test('a drop back in the hand is a rearrange for both roles', async () => {
    // The attacker branch of play_resolve reads only the cards, so without the
    // hand being answered first a resolver told "the hand" hands back a
    // perfectly good attack.
    let checked = 0;
    await walkPositions(3, 0, (g) => {
        for (let seat = 0; seat < g.players.length; seat++) {
            const hand = g.players[seat].hand;
            if (hand.length === 0) continue;
            const board = boardFor(g, seat);
            assert.equal(playResolve(board, [hand[0]], PLAY_TARGET_HAND), null,
                'the hand is never a play');
            checked++;
        }
    });
    assert.ok(checked > 20, `enough hand drops exercised (${checked})`);
});

test('good is offered only over a fully covered, non-empty table', async () => {
    let sawTrue = 0, sawFalse = 0;
    for (let s = 0; s < 3; s++) {
        await walkPositions(3, s, (g) => {
            for (let seat = 0; seat < g.players.length; seat++) {
                if (g.players[seat].status !== PLAYER_STATUS.IN) continue;
                const board = boardFor(g, seat);
                const can = playCanSayGood(board);
                const fullyCovered = g.table_battles.length > 0
                    && g.table_battles.every((b: Battle) => b.defense);
                if (!fullyCovered) {
                    assert.equal(can, false,
                        'no good over an uncovered attack, and none on an empty table');
                    sawFalse++;
                } else if (can) {
                    sawTrue++;
                }
            }
        });
    }
    assert.ok(sawFalse > 50, `enough not-yet positions (${sawFalse})`);
    assert.ok(sawTrue > 0, `at least one position where good is live (${sawTrue})`);
});

test('an open-table drop passes when a pass is legal, else auto-covers only when unambiguous', async () => {
    let passes = 0, covers = 0;
    for (let s = 0; s < 4; s++) {
        await walkPositions(3, s, (g) => {
            const seat = g.defender;
            if (g.table_battles.length === 0) return;
            const board = boardFor(g, seat);
            const moves = calculateLegalMoves(g, g.players[seat].player_id);
            for (const card of g.players[seat].hand) {
                const hit = playResolve(board, [card], PLAY_TARGET_TABLE);
                if (!hit) continue;
                if (hit.move.type === 'pass') {
                    assert.ok(moves.some((m) => m.type === 'pass' && m.cards?.length === 1
                        && sameCard(m.cards[0], card)), 'a resolved pass is in the menu');
                    passes++;
                } else {
                    assert.equal(hit.move.type, 'cover', 'the only other open-table play is a cover');
                    // Unambiguous: exactly one menu cover uses this selection.
                    const n = moves.filter((m) => m.type === 'cover' && m.cards?.length === 1
                        && sameCard(m.cards[0], card)).length;
                    assert.equal(n, 1, 'an auto-cover is only offered when it is unambiguous');
                    covers++;
                }
            }
        });
    }
    assert.ok(passes + covers > 20, `enough open-table drops (${passes} pass, ${covers} cover)`);
});
