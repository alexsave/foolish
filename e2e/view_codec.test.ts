// Masked-view codec ("view" v1) — the get_game packed round trip. For real
// kernel-driven games: durable blob -> serializeViewBlob(seat) (the kernel's
// per-viewer masking) -> encodeGameResponse envelope -> decodePackedGame (the
// client's render-boundary materialization). The decoded JS game must equal
// personalize_game(game, pid) — the retired JSON path's output — on every
// shared field, for every seat and for the spectator; and the raw view blob
// must never carry another player's hand identities.
//
// Pure kernel + TS codec test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    Game, Card, AnimationEvent, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
    PersonalGame, PrivatePlayer,
} from '../supabase/functions/_shared/types.ts';
import {
    serializeGameState, serializeViewBlob, kernelLegalMoves, kernelShouldAct,
    __setKernelSeedSource,
} from '../supabase/functions/_shared/wasm/engine.ts';
import { start_game } from '../supabase/functions/_shared/game_lifecycle.ts';
import { game_done, personalize_game } from '../supabase/functions/_shared/common_utils.ts';
import { handleAttack } from '../supabase/functions/_shared/actions/attack.ts';
import { handleCover } from '../supabase/functions/_shared/actions/cover.ts';
import { handlePass } from '../supabase/functions/_shared/actions/pass.ts';
import { handlePickup } from '../supabase/functions/_shared/actions/pickup.ts';
import { handleGood } from '../supabase/functions/_shared/actions/good.ts';
import { AwireKindName } from '../supabase/functions/_shared/wire/awire.ts';
import {
    decodePackedGame, encodeGameResponse, PackedGameRoster,
    VIEW_FORMAT_VERSION,
} from '../supabase/functions/_shared/wire/view.ts';
import { kernelViewFromPacked } from '../supabase/functions/_shared/wasm/bots.ts';

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
// as supabase/functions/get_game/index.ts builds it.
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
            // The kernel says "hand":null for a seat that is not the viewer —
            // the count is real, the identities never crossed.
            assert.equal(vp.hand, null, `${tag}: seat ${i} hand masked for viewer ${seat}`);
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
