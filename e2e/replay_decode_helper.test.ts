// e2e/helpers/replay_decode.ts, the kernel's decode as tests and tools read it
// (scripts/decode_replay.mjs, c/tools/og_explain/decode_to_json.mjs): the whole
// log stream comes back, it agrees with the code's own summary, and a code the
// kernel refuses is an error that names the refusal.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as G from '../sdk/ts/gen/game_layout.bots.ts';
import { kernelB32Encode, kernelReplayLink } from '../sdk/ts/wasm/bots.ts';
import { decodeReplayCode, decodeReplayLink } from './helpers/replay_decode.ts';
import { seededCode } from './helpers/seeded_codes.ts';

const SEEDED: [number, number][] = [[2, 7], [3, 41], [4, 42], [8, 43]];
const MOVES = new Set([G.LOG_ATTACK, G.LOG_COVER, G.LOG_PASS, G.LOG_PICKUP]);

test('every seeded code decodes to its whole stream, which says what its summary says', () => {
    for (const [np, s] of SEEDED) {
        const d = decodeReplayCode(seededCode(np, s));
        const where = `${np}p seed ${s}`;
        assert.equal(d.summary.numPlayers, np, where);
        assert.equal(d.logs[0].type, G.LOG_GAME_START, `${where}: the stream opens with the game start`);
        for (let seat = 0; seat < np; seat++) {
            const deal = d.logs[1 + seat];
            assert.deepEqual([deal.type, deal.seat, deal.pairs.length], [G.LOG_DRAW, seat, 6], `${where}: seat ${seat}'s dealt hand`);
            assert.ok(deal.pairs.every((p) => p.target === null && p.primary.suit >= 0), `${where}: a dealt hand is real cards`);
        }
        assert.equal(d.logs.filter((l) => MOVES.has(l.type)).length, d.summary.moves, `${where}: one move per extras gap`);
        const out = d.logs.filter((l) => l.type === G.LOG_PLAYER_OUT).map((l) => l.seat);
        assert.deepEqual(out, [...d.summary.elimination], `${where}: players go out in the summary's order`);
        const covers = d.logs.filter((l) => l.type === G.LOG_COVER);
        assert.ok(covers.length > 0 && covers.every((l) => l.pairs.every((p) => p.target !== null)), `${where}: a cover names what it covers`);
    }
});

test('a share link decodes to the same game, with the names its extras carry', () => {
    const code = seededCode(3, 41);
    const link = kernelReplayLink(kernelB32Encode(code), ['Ann', 'Bob', 'Cy']);
    const d = decodeReplayLink(link);
    assert.deepEqual(d.logs, decodeReplayCode(code).logs);
    assert.deepEqual(d.names, ['Ann', 'Bob', 'Cy']);
});

test('a code the kernel refuses is an error naming the refusal', () => {
    assert.throws(() => decodeReplayCode(new Uint8Array([7])), /replay decode: REPLAY_E[A-Z]+/);
});
