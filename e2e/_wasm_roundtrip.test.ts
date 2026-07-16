// Round-trip determinism proof (the user's methodology). Deal a seeded game,
// self-play it forward recording every move + the octogen decision that made it,
// then RECONSTRUCT by re-dealing the same seed and re-driving the recorded moves,
// re-querying octogen at each turn. With octogen's RNG seeded ONLY from the deal
// seed (not num_logs / hands / deck), a recorded game must reconstruct 100% —
// nothing the replay carries or drops can move a tie-break. Runs under node
// --test. Set SC_SEED to vary the game; skipped in CI.
import { test } from 'node:test';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { encodeAction } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { game_done } from '../supabase/functions/_shared/common/common_utils.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();

const SEED = process.env.SC_SEED || 'a57de70a4b96a19c661dedab5cd448bb5eca2379642badb14043cebf15912478';
const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
const cat = (a: Uint8Array, b: Uint8Array) => { const m = new Uint8Array(a.length + b.length); m.set(a); m.set(b, a.length); return m; };
const mkGame = () => ({
    players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
    deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], game_seed: SEED,
}) as any;
const mkMove = (p: any) => { const m: any = { kind: p.type }; if (p.type !== 'pickup' && p.type !== 'good') m.cards = p.cards; if (p.type === 'cover') m.attack_cards = p.attack_cards; return m; };
const kOf = (t: string, cards?: any[], atk?: any[]) => `${t}|${(cards || []).map((c) => c.suit + '.' + c.value).sort().join(',')}|${(atk || []).map((c) => c.suit + '.' + c.value).sort().join(',')}`;
const drive = (g: any, seat: number, move: any) => { let m = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) m |= 1 << k; }); return runPackedGameAction(g, seat, encodeAction(move), m, []) as any; };

test('seeded game round-trips through reconstruction exactly', { skip: !process.env.SC_RUN }, () => {
    // ---- Phase A: self-play forward, recording (seat, move) ----
    __setDealSeedOverride(Uint8Array.from(SEED.match(/../g)!.map((b) => parseInt(b, 16))));
    const A = mkGame();
    const sA: any = start_game_packed(A);
    let belA = new Uint8Array(0); void sA;
    const rec: Array<{ seat: number; pick: any }> = [];
    let guard = 0;
    while (game_done(A) == null && guard++ < 400) {
        let actor = -1, pick: any = null;
        for (let s = 0; s < 2; s++) {
            if (belA.length) A.belief_log_bytes = belA; else delete A.belief_log_bytes;
            const q = wasmChooseMoveDirect(A, `p${s}`, STRAT.octogen, { env });
            if (q) { actor = s; pick = q; break; }
        }
        if (actor < 0) break;
        rec.push({ seat: actor, pick: { type: pick.type, cards: pick.cards, attack_cards: pick.attack_cards } });
        const run = drive(A, actor, mkMove(pick));
        if (!run?.ok) { console.log(`play stuck at move ${rec.length}`); break; }
        belA = cat(belA, logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(A, run.post, `p${actor}`);
    }
    console.log(`play-forward: ${rec.length} moves, elim=${JSON.stringify(A.elimination_order)}`);

    // ---- Phase B: re-deal same seed, re-drive recorded moves, re-query ----
    __setDealSeedOverride(Uint8Array.from(SEED.match(/../g)!.map((b) => parseInt(b, 16))));
    const B = mkGame();
    const sB: any = start_game_packed(B);
    let belB = new Uint8Array(0); void sB;
    let match = 0, mism = 0;
    for (const step of rec) {
        if (belB.length) B.belief_log_bytes = belB; else delete B.belief_log_bytes;
        const q = wasmChooseMoveDirect(B, `p${step.seat}`, STRAT.octogen, { env });
        const got = q ? kOf(q.type, (q as any).cards, (q as any).attack_cards) : 'null';
        const want = kOf(step.pick.type, step.pick.cards, step.pick.attack_cards);
        if (got === want) match++; else { mism++; if (mism <= 6) console.log(`  MISMATCH move: want ${want} got ${got}`); }
        const run = drive(B, step.seat, mkMove(step.pick));
        if (!run?.ok) { console.log(`re-drive stuck`); break; }
        belB = cat(belB, logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(B, run.post, `p${step.seat}`);
    }
    console.log(`ROUND-TRIP: ${match}/${rec.length} moves reproduced  ${mism === 0 ? '✓ EXACT' : '✗ ' + mism + ' diverged'}`);
});
