// VALIDATION (pure, no Postgres): a small deterministic slice of replay_codec.test.ts.
// Plays a few short engine games and round-trips them through the replay codec,
// asserting the decoded log stream reproduces the original byte-for-byte. Any
// rules drift between the server engine and _shared/replay/* surfaces here. Plus
// the scale-free timing self-test. (The full test sweeps 20 games x 7 player
// counts; this guard plays a handful so CI stays fast.)

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.info = () => {}; }

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { start_game, game_done } from '../supabase/functions/_shared/common_utils.ts';
import { Game, GameLog, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey, LOG_TYPE } from '../supabase/functions/_shared/types.ts';
import { shouldBotActCore, processBotAction } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/bot_strategy.ts';
import { ReplayInput, SeatLog, DecodedReplay } from '../supabase/functions/_shared/replay/core.ts';
import { encodeReplay, verifyRoundTrip } from '../supabase/functions/_shared/replay/encode.ts';
import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';
import { urlToGame, base64Decode, bytesToBigint } from '../supabase/functions/_shared/replay/codec.ts';
import { encodeExtrasFromGaps, decodeExtras } from '../supabase/functions/_shared/replay/extras.ts';

const mkGame = (np: number): Game => ({
    players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
        player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY, is_ai: true,
        hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'random' as StrategyKey,
    })),
    deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING, deck_length: 0,
    discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0,
    table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
});

async function playGame(np: number): Promise<Game | null> {
    const game = mkGame(np);
    start_game(game);
    let actions = 0;
    while (game_done(game) === null) {
        if (++actions > 100000) return null;
        const eligible = game.players.filter((p, i) => shouldBotActCore(game, p, i) && calculateLegalMoves(game, p.player_id).length > 0);
        if (eligible.length === 0) return null;
        let acted = false;
        for (const p of eligible) if (await processBotAction(game, p)) { acted = true; break; }
        if (!acted) return null;
    }
    return game;
}

const norm = (logs: GameLog[] | DecodedReplay['logs'], seatOf: (pid: any) => number | null) =>
    logs.filter((l: any) => l.log_type !== LOG_TYPE.GOOD).map((l: any) => ({
        log_type: l.log_type,
        seat: 'seat' in l ? l.seat : seatOf(l.player_id),
        card_pairs: l.card_pairs.map((p: any) => ({
            primary: { suit: p.primary.suit, value: p.primary.value },
            target: p.target ? { suit: p.target.suit, value: p.target.value } : null,
        })),
        defender_index: l.defender_index ?? null,
    }));

function roundTrip(game: Game) {
    const input: ReplayInput = { playerIds: game.players.map((p) => p.player_id), logs: game.logs, flipped: game.flipped };
    const enc = encodeReplay(input);
    assert.equal(urlToGame(enc.url), enc.x, 'url serialization round-trip');
    assert.equal(bytesToBigint(base64Decode(enc.base64)), enc.x, 'base64 serialization round-trip');

    const dec = decodeReplay(enc.x);
    const seatOf = (pid: string | null) => (pid === null ? null : game.players.findIndex((p) => p.player_id === pid));
    const a = norm(game.logs, seatOf).map((l) => JSON.stringify(l));
    const b = norm(dec.logs, seatOf).map((l) => JSON.stringify(l));
    for (let i = 0; i < Math.max(a.length, b.length); i++) {
        assert.equal(a[i], b[i], `replay stream drift at entry ${i}:\n  original: ${a[i] ?? '<end>'}\n  decoded:  ${b[i] ?? '<end>'}`);
    }
    const elim = game.elimination_order.map((pid) => game.players.findIndex((p) => p.player_id === pid));
    assert.deepEqual(elim, dec.eliminationOrder, 'elimination order');
    assert.equal(game.players.findIndex((p) => p.player_id === game_done(game)), dec.fool, 'fool');
    assert.equal(game.discard_pile_length, dec.discardPileLength, 'discard pile length');
    verifyRoundTrip(input); // the public UI verifier must agree
}

test('replay codec round-trips short engine games byte-exact (2..4 players)', async () => {
    let played = 0;
    for (const np of [2, 3, 4]) {
        for (let g = 0; g < 2; g++) {
            const game = await playGame(np);
            if (!game) continue;
            played++;
            roundTrip(game);
        }
    }
    assert.ok(played > 0, 'at least one game completed');
});

test('replay extras: time scale holds from 1ns to 1 week units', () => {
    for (const scale of [1e-9, 1e-6, 1e-3, 1, 3600, 86400 * 7]) {
        const raw = [1, 2.5, 7, 0.3, 40, 12, 0.9, 100];
        const blob = encodeExtrasFromGaps(null, 1750000000, raw.map((r) => r * scale));
        const back = decodeExtras(blob, 2, raw.length);
        back.moveGaps!.forEach((g, i) => {
            const want = raw[i] * scale;
            assert.ok(Math.abs(g - want) <= want * 0.08, `scale ${scale}: gap ${i} got ${g}, want ${want}`);
        });
    }
});
