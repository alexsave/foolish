/* =============================================================================
 * A5 — a replay is the game replayed, and the web renders it as live play
 * =============================================================================
 * docs/C_CORE_CONSOLIDATION.md §4.6 (F4.2 / A5).
 *
 * The kernel rebuilds the real Game a v6 code describes and replays it through
 * the real engine, serializing the SAME packed evwire frames live play
 * broadcasts. This asserts the two halves of that claim that only the web can
 * check:
 *
 *   1. the frames decode with decodeEventWire — the client's LIVE decoder, not
 *      a replay-specific one; and
 *   2. what comes out is the game the engine actually played.
 *
 * If those hold, a replay screen has no projection to keep in step with live
 * play, because it is not rendering a projection. It is rendering live play.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { start_game } from '../server/api/common/game_lifecycle.ts';
import { game_done } from '../server/api/common/common_utils.ts';
import {
    Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey,
} from '../server/api/core/types.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { kernelReplayEncodeV6FromGame, replayEventFrames, replayStepCount } from '../sdk/ts/wasm/bots.ts';
import { decodeEventWire } from '../sdk/ts/wire/evwire.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import type { ViewRoster } from '../sdk/ts/wire/view.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {};
    console.warn = () => {};
    console.error = () => {};
    console.info = () => {};
}

const SEED_HEX = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';
const hexToBytes = (h: string) =>
    new Uint8Array((h.match(/.{2}/g) ?? []).map(b => parseInt(b, 16)));

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
    player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: strategy,
});

function mkGame(np: number, strategy: StrategyKey): Game {
    return {
        players: Array.from({ length: np }, (_, i) => mkPlayer(i, strategy)),
        deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [],
        game_seed: SEED_HEX,
    } as unknown as Game;
}

// A seeded game played to the end, exactly as the finalize path sees one. The
// deal-seed override is load-bearing: without it the TS engine deals from its
// own RNG and the kernel — which re-deals from the seed to encode — rebuilds a
// DIFFERENT game, which surfaces as "logged attack not in menu".
async function playSeeded(np: number): Promise<Game | null> {
    const game = mkGame(np, 'handwritten' as StrategyKey);
    __setDealSeedOverride(hexToBytes(SEED_HEX));
    try {
        start_game(game);
        for (let guard = 0; guard < 20000 && game_done(game) === null; guard++) {
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return null;
        }
    } finally {
        __setDealSeedOverride(null);
    }
    return game_done(game) !== null ? game : null;
}

const roster = (game: Game): ViewRoster => ({
    id: game.id, name: game.name,
    players: game.players.map(p => ({
        player_id: p.player_id, name: p.name, is_ai: p.is_ai,
        strategy_key: p.strategy_key,
    })),
});

test('a v6 replay decodes as LIVE evwire frames, and is the game that was played', async () => {
    for (let np = 2; np <= 4; np++) {
        const game = await playSeeded(np);
        assert.ok(game, `${np}p seeded game completed`);
        if (!game) continue;

        const code = kernelReplayEncodeV6FromGame(game, hexToBytes(SEED_HEX), undefined, 1 << 20);
        const steps = replayStepCount(code);
        assert.ok(steps > 0, `${np}p: the code reports a step count`);

        // Spectator: the viewer every replay share is watched as.
        const frames = replayEventFrames(code, -1);
        assert.equal(frames.length, steps, `${np}p: one frame per step`);

        const ctx = { preGood: [], prevGoodTs: null, now: () => 0 };
        let decodedSteps = 0;
        let totalEvents = 0;
        let last: any = null;
        for (const frame of frames) {
            // The LIVE decoder. Not a replay-specific one — that is the point.
            const seq = decodeEventWire(frame, roster(game), ctx);
            assert.ok(seq, `${np}p: step ${decodedSteps} decodes with the live decoder`);
            if (!seq) break;
            decodedSteps++;
            totalEvents += seq.events.length;
            // Every frame carries its step's committed board (the trailer) —
            // which is exactly the per-step board a replay scrubber renders.
            last = seq.game;
        }
        assert.equal(decodedSteps, steps, `${np}p: every frame decoded`);
        assert.ok(totalEvents > 0, `${np}p: the frames carry animation events`);

        // The replay IS the game: the board the last frame carries must be the
        // board the engine really finished on.
        assert.ok(last, `${np}p: the frames carry board state`);
        assert.equal(last.discard_pile_length, game.discard_pile_length,
            `${np}p: replay ends on the played discard count`);
        assert.equal(last.deck_length, 0, `${np}p: a finished game drained its stock`);
        assert.equal(last.players.length, np, `${np}p: every seat came back`);
    }
});

test('a replay refuses a v5 code rather than inventing hands', () => {
    // v5 hides the deal, so its "hands" are retrodiction and there is no deck to
    // rebuild. The kernel says so instead of guessing (docs §4.6).
    const notV6 = new Uint8Array([0x05, 0x00, 0x00, 0x00]);
    assert.throws(() => replayStepCount(notV6), /replay|version/i);
});
