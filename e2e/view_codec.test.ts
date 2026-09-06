// Masked-view codec ("view" v1) — the get_game packed round trip. For real
// kernel-driven games: durable blob -> serializeViewBlob(seat) (the kernel's
// per-viewer masking) -> encodeGameResponse envelope -> decodePackedGame (the
// client's render-boundary materialization). The decoded JS game must equal
// personalize_game(game, pid) — the retired JSON path's output — on every
// shared field, for every seat and for the spectator.
//
// The raw view blob must also never carry another player's hand identities.
// That line stood in this header for a long time with nothing checking it: the
// per-seat assertion in checkView reads the blob back through the kernel's own
// decoder, which reports a non-viewer hand as null regardless of the bytes. The
// invariant is now asserted where it lives, on the bytes.
//
// Pure kernel + TS codec test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    Game, Card, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
    PersonalGame, PrivatePlayer,
} from '../server/api/core/types.ts';
import {
    serializeGameState, serializeViewBlob, kernelLegalMoves, kernelShouldAct,
    __setKernelSeedSource,
} from '../sdk/ts/wasm/engine.ts';
import { start_game } from '../server/api/common/game_lifecycle.ts';
import { game_done, personalize_game } from '../server/api/common/common_utils.ts';
import { handleAttack } from '../server/api/common/actions/attack.ts';
import { handleCover } from '../server/api/common/actions/cover.ts';
import { handlePass } from '../server/api/common/actions/pass.ts';
import { handlePickup } from '../server/api/common/actions/pickup.ts';
import { handleGood } from '../server/api/common/actions/good.ts';
import { AwireKindName } from '../sdk/ts/wire/awire.ts';
import {
    decodePackedGame, encodeGameResponse, PackedGameRoster,
    VIEW_FORMAT_VERSION,
} from '../sdk/ts/wire/view.ts';
import { kernelViewFromPacked } from '../sdk/ts/wasm/bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

// Deterministic RNG (same LCG as the fuzz suite) so failures reproduce.
let seed = Number(process.env.FUZZ_SEED || 0x5eed1e55) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

// Pin the kernel's per-action reseed so whole games replay deterministically.
let moveSeed = 1;
__setKernelSeedSource(() => moveSeed);

const mkPlayer = (i: number, isAi: boolean): PrivatePlayer => ({
    player_id: `player-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY,
    is_ai: isAi, hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: isAi ? STRATEGY_KEY.RANDOM : STRATEGY_KEY.HUMAN,
});

const mkLobby = (numPlayers: number): Game => ({
    id: 'viewgame', name: 'view codec', status: GAME_STATUS.WAITING,
    players: Array.from({ length: numPlayers }, (_, i) => mkPlayer(i, i % 2 === 1)),
    deck: [], deck_length: 0, discard_pile_length: 0, flipped: null,
    power_suit: 0, first_attacker: 0, defender: 0, table_battles: [],
    elimination_order: [], good_timestamp: null, good_players: [], logs: [],
});

const dispatch = (g: Game, pid: string, kind: AwireKindName, cards?: Card[], attacks?: Card[]): AnimationEvent[] => {
    switch (kind) {
        case 'attack': return handleAttack(g, pid, cards!);
        case 'cover': return handleCover(g, pid, cards!, attacks!);
        case 'pass': return handlePass(g, pid, cards!);
        case 'pickup': return handlePickup(g, pid);
        case 'good': return handleGood(g, pid);
    }
};

// The get_game roster (identity + the column-authoritative fields), exactly
// as server/impls/supabase/functions/get_game/index.ts builds it.
const rosterFor = (game: Game): PackedGameRoster => ({
    id: game.id,
    name: game.name,
    status: game.status,
    players: game.players.map((p) => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
    good_players: game.good_players ?? [],
    good_timestamp: game.good_timestamp ?? null,
});

// One full round trip + assertions for one viewer of one state.
function checkView(game: Game, blob: Uint8Array, seat: number, version: number, tag: string): void {
    const viewBlob = serializeViewBlob(blob, seat);
    assert.equal(viewBlob[0], VIEW_FORMAT_VERSION, `${tag}: view blob format byte`);
    assert.equal(viewBlob[1], seat < 0 ? 0xff : seat, `${tag}: view blob viewer byte`);

    // Personalization on the raw bytes: parse the masked payload — every
    // non-viewer hand must be fully hidden (counts intact), the viewer's own
    // hand fully real. The deck is always masked but its LENGTH is real.
    // Read back by the kernel (A8/F7) — the TS parser that used to shadow
    // view.c's layout here is gone. The blob leads with [fmt | viewer].
    const state = kernelViewFromPacked(viewBlob.subarray(2), seat);
    state.players.forEach((vp, i) => {
        assert.equal(vp.handCount, game.players[i].hand.length, `${tag}: seat ${i} hand count real for viewer ${seat}`);
        if (i === seat) {
            assert.ok(vp.hand, `${tag}: the viewer's own hand is present`);
            vp.hand!.forEach((c, j) => assert.deepEqual({ suit: c.s, value: c.v }, game.players[i].hand[j],
                                                        `${tag}: own hand card ${j} real`));
        } else {
            // The decoder says hand: null for a seat that is not the viewer.
            // NOTE what this does and does not prove: packed_read.ts emits null
            // BECAUSE the seat is not the viewer, not because the bytes were
            // hidden, so a payload full of real identities would satisfy it
            // too. It pins the decoder's contract, nothing about the masking.
            // The masking itself is asserted on the bytes, in "a masked view
            // does not depend on the hands it is masking" below.
            assert.equal(vp.hand, null, `${tag}: seat ${i} reported as null for viewer ${seat}`);
        }
    });
    assert.equal(state.deckCount, game.deck.length, `${tag}: deck length real`);

    // Envelope round trip — the exact get_game packed response.
    const roster = rosterFor(game);
    const bytes = encodeGameResponse(version, seat, roster, viewBlob);
    const dec = decodePackedGame(bytes, () => 424242);
    assert.ok(dec, `${tag}: packed game response decodes`);
    assert.equal(dec!.version, version, `${tag}: version survives the envelope`);
    assert.equal(dec!.seat, seat < 0 ? -1 : seat, `${tag}: seat survives the envelope`);
    assert.equal(dec!.game.version, version, `${tag}: decoded game carries the version`);

    // The decoded game must equal the retired JSON path's personalize_game
    // on every shared field. (Spectator: an id not in the game yields the
    // PublicGame branch, same as get_game for a non-member.)
    const pid = seat >= 0 ? game.players[seat].player_id : 'spectator-nobody';
    const expected = personalize_game(game, pid);
    const dg = dec!.game;
    assert.deepEqual(dg.players, expected.players, `${tag}: public players (names/status/hand_length)`);
    assert.deepEqual(dg.table_battles, expected.table_battles, `${tag}: table battles`);
    assert.equal(dg.deck_length, expected.deck_length, `${tag}: deck_length`);
    assert.equal(dg.discard_pile_length, expected.discard_pile_length, `${tag}: discard_pile_length`);
    assert.deepEqual(dg.flipped, expected.flipped, `${tag}: flipped`);
    assert.equal(dg.status, expected.status, `${tag}: status (column-authoritative)`);
    assert.equal(dg.power_suit, expected.power_suit, `${tag}: power_suit`);
    assert.equal(dg.first_attacker, expected.first_attacker, `${tag}: first_attacker`);
    assert.equal(dg.defender, expected.defender, `${tag}: defender`);
    assert.deepEqual(dg.good_players, expected.good_players, `${tag}: good_players order`);
    assert.equal(dg.good_timestamp, expected.good_timestamp, `${tag}: good_timestamp value`);
    assert.deepEqual(dg.elimination_order, expected.elimination_order, `${tag}: elimination_order`);

    if (seat >= 0) {
        const self = (dg as PersonalGame).self;
        const expSelf = (expected as PersonalGame).self;
        assert.ok(self, `${tag}: player view carries self`);
        assert.deepEqual(self.hand, expSelf.hand, `${tag}: own hand identical, in order`);
        assert.equal(self.awaiting_attack, expSelf.awaiting_attack, `${tag}: awaiting_attack real for the viewer`);
        assert.equal(self.player_id, expSelf.player_id, `${tag}: self identity`);
        assert.equal(self.hand_length, expSelf.hand.length, `${tag}: self hand_length`);
    } else {
        assert.ok(!('self' in dg), `${tag}: spectator view is a PublicGame — no self`);
    }
}

// ---------------------------------------------------------------------------
// THE MASKING ITSELF, ON THE BYTES
// ---------------------------------------------------------------------------
// checkView above asserts "seat i hand masked for viewer s" - and cannot fail.
// It reads the blob back with kernelViewFromPacked, which reports
// hand: null for any seat that is not the viewer BECAUSE it is not the viewer,
// not because the bytes were hidden. So the identities could all be sitting in
// the payload and that assertion would still pass. The file header has claimed
// this invariant since it was written; nothing was checking it.
//
// Measured, with state_put's masked-hand memset replaced by the real cards:
// twelve of the other seats' actual card ids present in a seat-0 view, and the
// whole suite green - view_codec, client, packed_wire_stream,
// packed_roster_wire, server, action_handlers, meta, 47 assertions.
//
// The invariant here is layout-free, which matters because the TS mirror of
// that layout is deliberately gone: A MASKED VIEW MUST NOT DEPEND ON WHAT IT IS
// MASKING. Change a hand the viewer cannot see, and the bytes it receives must
// be identical. Any leak - ordered, unordered, partial - changes them.
test('a masked view does not depend on the hands it is masking', () => {
    let compared = 0;
    for (let g = 0; g < 6; g++) {
        const game = mkLobby(2 + (g % 5));
        moveSeed = (g * 7919 + 3) >>> 0;
        start_game(game);
        game.status = GAME_STATUS.PLAYING;

        for (let viewer = -1; viewer < game.players.length; viewer++) {
            const before = serializeViewBlob(serializeGameState(game), viewer);

            // Rewrite every hand the viewer may NOT see, keeping the counts
            // (a count is public and legitimately in the blob).
            const saved = game.players.map((p) => p.hand.slice());
            game.players.forEach((p, i) => {
                if (i === viewer) return;
                p.hand = p.hand.map((c, j) => ({
                    // A different identity, still inside the card space.
                    suit: (c.suit + 1 + j) % 4,
                    value: ((c.value + 5 + j) % 13) + 1,
                }));
            });
            const after = serializeViewBlob(serializeGameState(game), viewer);
            game.players.forEach((p, i) => { p.hand = saved[i]; });

            assert.deepEqual([...after], [...before],
                `game ${g} viewer ${viewer}: the view changed when a hidden hand changed`);
            compared++;

            // And the counts really were non-trivial - a game with empty hands
            // would satisfy the above for the wrong reason.
            const hidden = game.players.reduce((n, p, i) => n + (i === viewer ? 0 : p.hand.length), 0);
            assert.ok(hidden > 0, `game ${g} viewer ${viewer}: nothing was actually being masked`);
        }
    }
    assert.ok(compared >= 20, `expected a spread of viewers, got ${compared}`);
});

test('view codec: blob -> serializeViewBlob -> encodeGameResponse -> decodePackedGame equals personalize_game for every seat + spectator', () => {
    const GAMES = Number(process.env.VIEW_GAMES || 10);
    let checks = 0, ends = 0;

    for (let g = 0; g < GAMES; g++) {
        const game = mkLobby(2 + (g % 5)); // 2..6 players (36- and 52-card decks)
        moveSeed = (g * 6151 + 17) >>> 0;
        start_game(game);
        game.status = GAME_STATUS.PLAYING;

        const checkAllSeats = (mv: number) => {
            const blob = serializeGameState(game);
            for (let seat = -1; seat < game.players.length; seat++) {
                checkView(game, blob, seat, g * 1000 + mv, `game ${g} move ${mv} viewer ${seat}`);
                checks++;
            }
        };
        checkAllSeats(0); // the fresh deal: full hands, flipped trump, no battles

        for (let mv = 1; mv <= 250 && game.status === GAME_STATUS.PLAYING; mv++) {
            const eligible = game.players.filter((p) => kernelShouldAct(game, p.player_id));
            if (eligible.length === 0) break;
            const actor = eligible[ri(eligible.length)];
            const menu = kernelLegalMoves(game, actor.player_id).filter((m) => m.type !== 'wait');
            if (menu.length === 0) continue;
            const m = menu[ri(menu.length)];
            moveSeed = (moveSeed * 48271 + mv + 1) >>> 0;
            dispatch(game, actor.player_id, m.type as AwireKindName, m.cards, m.attack_cards);

            // End-of-game mirror of executeWithGameLock's check_win_sync, so
            // the codec is also proven on GAME_OVER states (elimination order,
            // OUT/IDLE statuses, empty table).
            if (game_done(game) !== null) {
                game.status = GAME_STATUS.GAME_OVER;
                for (const p of game.players) p.status = p.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
                ends++;
            }
            if (mv % 3 === 0 || game.status !== GAME_STATUS.PLAYING) checkAllSeats(mv);
        }
    }

    assert.ok(checks > 300, `exercised enough view round-trips (${checks})`);
    assert.ok(ends >= 2, `enough games reached GAME_OVER under the codec check (${ends})`);
    console.error(`[view codec] ${checks} seat round-trips, ${ends} finished games`);
});
