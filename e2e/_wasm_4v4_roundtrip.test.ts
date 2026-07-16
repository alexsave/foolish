// 8-player 4v4 round-trip determinism proof, SERVER PATH. Self-play a seeded
// 4-octogen (0-3) vs 4-random (4-7) game driving EVERY seat through
// wasmChooseMoveDirect + belief_log_bytes (exactly how the deployed server picks
// moves), then reconstruct by re-dealing the same seed and re-driving the
// recorded moves, re-querying octogen. octogen must reproduce 100% — this is the
// server's own belief path on both ends. Contrast: offlinefun/octogen_4v4.ts
// drives octogen via getBotStrategy().chooseMove (the game.logs object-marshal
// fallback), a DIFFERENT belief path, which flips co-optimal near-ties.
import { test } from 'node:test';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../sdk/ts/wasm/bots.ts';
import { game_done } from '../server/api/common/common_utils.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../server/api/core/types.ts';

__ensureBots();
const SEED = process.env.SC_SEED || 'faf1703f99448c193b6a9252f24affec2b5dcb68fae6d3bc3c69ea852c137f82';
const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
const NP = 8;
const OCTO = new Set([0, 1, 2, 3]);
const cat = (a: Uint8Array, b: Uint8Array) => { const m = new Uint8Array(a.length + b.length); m.set(a); m.set(b, a.length); return m; };
const mkGame = () => ({
    players: Array.from({ length: NP }, (_, i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: OCTO.has(i) ? (STRATEGY_KEY as any).OCTOGEN : (STRATEGY_KEY as any).RANDOM })),
    deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], game_seed: SEED,
}) as any;
const mkMove = (p: any) => { const m: any = { kind: p.type }; if (p.type !== 'pickup' && p.type !== 'good') m.cards = p.cards; if (p.type === 'cover') m.attack_cards = p.attack_cards; return m; };
const kOf = (t: string, cards?: any[], atk?: any[]) => `${t}|${(cards || []).map((c) => c.suit + '.' + c.value).sort().join(',')}|${(atk || []).map((c) => c.suit + '.' + c.value).sort().join(',')}`;
const stratOf = (seat: number) => OCTO.has(seat) ? STRAT.octogen : STRAT.random;
const drive = (g: any, seat: number, move: any) => { let m = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) m |= 1 << k; }); return runPackedGameAction(g, seat, encodeAction(move), m, []) as any; };

test('8-player 4v4 round-trips through reconstruction exactly (server path)', { skip: !process.env.SC_RUN }, () => {
    // ---- Phase A: self-play forward through the server belief path ----
    __setDealSeedOverride(Uint8Array.from(SEED.match(/../g)!.map((b) => parseInt(b, 16))));
    const A = mkGame(); start_game_packed(A);
    let belA = new Uint8Array(0);
    const rec: Array<{ seat: number; pick: any; octo: boolean }> = [];
    let guard = 0;
    while (game_done(A) == null && guard++ < 4000) {
        let actor = -1, pick: any = null;
        for (let s = 0; s < NP; s++) {
            if (belA.length) A.belief_log_bytes = belA; else delete A.belief_log_bytes;
            const q = wasmChooseMoveDirect(A, `p${s}`, stratOf(s), { env });
            if (q) { actor = s; pick = q; break; }
        }
        if (actor < 0) break;
        rec.push({ seat: actor, octo: OCTO.has(actor), pick: { type: pick.type, cards: pick.cards, attack_cards: pick.attack_cards } });
        const run = drive(A, actor, mkMove(pick));
        if (!run?.ok) { console.log(`play stuck at move ${rec.length}`); break; }
        belA = cat(belA, logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(A, run.post, `p${actor}`);
    }
    console.log(`play-forward: ${rec.length} moves, elim=${JSON.stringify(A.elimination_order)}`);

    // ---- Phase B: re-deal same seed, re-drive recorded moves, re-query octogen ----
    __setDealSeedOverride(Uint8Array.from(SEED.match(/../g)!.map((b) => parseInt(b, 16))));
    const B = mkGame(); start_game_packed(B);
    let belB = new Uint8Array(0);
    let octoTotal = 0, octoMatch = 0; const diffs: any[] = [];
    let step = 0;
    for (const s of rec) {
        if (belB.length) B.belief_log_bytes = belB; else delete B.belief_log_bytes;
        if (s.octo) {
            octoTotal++;
            const q = wasmChooseMoveDirect(B, `p${s.seat}`, STRAT.octogen, { env });
            const got = q ? kOf(q.type, (q as any).cards, (q as any).attack_cards) : 'null';
            const want = kOf(s.pick.type, s.pick.cards, s.pick.attack_cards);
            if (got === want) octoMatch++; else diffs.push({ step, seat: s.seat, want, got });
        }
        const run = drive(B, s.seat, mkMove(s.pick));
        if (!run?.ok) { console.log(`re-drive stuck at step ${step}`); break; }
        belB = cat(belB, logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(B, run.post, `p${s.seat}`);
        step++;
    }
    console.log(`ROUND-TRIP (server path): octogen ${octoMatch}/${octoTotal} reproduced  ${diffs.length === 0 ? '✓ EXACT' : '✗ ' + diffs.length + ' diverged: ' + JSON.stringify(diffs.slice(0, 6))}`);
});
