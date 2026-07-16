// Confirms the per-decision strategy seed (state_fnv) both VARIES across a game
// (so world sampling isn't the same every decision) and REPRODUCES across a
// replay (re-deal same seed + re-drive -> identical seed at each decision). The
// seed now folds public board state that moves each turn, so it changes, while
// staying a pure function of the move log, so it replays.
import { test } from 'node:test';
import assert from 'node:assert';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { encodeAction } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, __strategySeedProbe, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';

__ensureBots();
const SEED = process.env.SC_SEED || 'a57de70a4b96a19c661dedab5cd448bb5eca2379642badb14043cebf15912478';
const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
const cat = (a: Uint8Array, b: Uint8Array) => { const m = new Uint8Array(a.length + b.length); m.set(a); m.set(b, a.length); return m; };
const mkGame = () => ({ players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })), deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], game_seed: SEED }) as any;
const mkMove = (p: any) => { const m: any = { kind: p.type }; if (p.type !== 'pickup' && p.type !== 'good') m.cards = p.cards; if (p.type === 'cover') m.attack_cards = p.attack_cards; return m; };
const drive = (g: any, seat: number, move: any) => { let m = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) m |= 1 << k; }); return runPackedGameAction(g, seat, encodeAction(move), m, []) as any; };

function run(collectSeeds: number[]) {
    __setDealSeedOverride(Uint8Array.from(SEED.match(/../g)!.map((b) => parseInt(b, 16))));
    const g = mkGame(); start_game_packed(g);
    let bel = new Uint8Array(0);
    const rec: Array<{ seat: number; pick: any }> = [];
    let guard = 0;
    while (game_done(g) == null && guard++ < 400) {
        let actor = -1, pick: any = null;
        for (let s = 0; s < 2; s++) {
            if (bel.length) g.belief_log_bytes = bel; else delete g.belief_log_bytes;
            const q = wasmChooseMoveDirect(g, `p${s}`, STRAT.octogen, { env });
            if (q) { actor = s; pick = q; collectSeeds.push(__strategySeedProbe()); break; }
        }
        if (actor < 0) break;
        rec.push({ seat: actor, pick });
        const r = drive(g, actor, mkMove(pick)); if (!r?.ok) break;
        bel = cat(bel, logsFromKernelExport(r.logsWire, 1));
        applyKernelStateToGame(g, r.post, `p${actor}`);
    }
    return rec;
}

test('strategy seed varies per decision and reproduces across a replay', { skip: !process.env.SC_RUN }, () => {
    const seedsA: number[] = []; run(seedsA);
    const seedsB: number[] = []; run(seedsB);
    const distinct = new Set(seedsA).size;
    console.log(`decisions=${seedsA.length}  distinct seeds=${distinct}  first5=${seedsA.slice(0, 5).map((s) => s.toString(16))}`);
    console.log(`reproducible across replay: ${JSON.stringify(seedsA) === JSON.stringify(seedsB)}`);
    // VARIES: most decisions get a distinct seed (public board moves each turn).
    // Repeats are legitimate identical public positions — forcing those apart
    // would need a non-public counter, which is what broke reproducibility.
    assert.ok(distinct > seedsA.length * 0.6, `seed should vary per decision: only ${distinct}/${seedsA.length} distinct`);
    assert.ok(distinct > 1, 'seed is constant across the whole game — degenerate');
    // REPRODUCES: same seed at every decision on a re-run from the same deal seed
    assert.deepStrictEqual(seedsA, seedsB, 'seeds not reproducible across replay');
});
