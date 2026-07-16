// Drive a decoded replay through the DEPLOYED wasm octogen (a bots.wasm built
// with -DOG_EXPLAIN_BUILD, swapped in by explain.py --wasm) and emit one
// OG_EXPLAIN JSONL record per octogen decision — byte-compatible with the native
// og_explain sink, so build_data.py consumes either identically. Lives in e2e/
// so the deal-seed override resolves to a single engine.ts module instance
// (from sdk/c/tools it duplicated and the deal went random). Skipped in CI.
//
//   RECON_SEED=<64hex> RECON_RD=<replay_decoded.json> OGX_WASM_DELIB=<out.jsonl> \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test e2e/_wasm_drive.test.ts
import { test } from 'node:test';
import { readFileSync, writeFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, __ogExplainDump, STRAT } from '../sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();

test('wasm octogen deliberation drive', { skip: !process.env.OGX_WASM_DELIB }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const OUT = process.env.OGX_WASM_DELIB!;
    const NP = Number(rd.playerCount || 2);
    const OGSEAT = Number(process.env.OGX_SEAT || '1');
    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: Array.from({ length: NP }, (_, i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    const startRun: any = start_game_packed(g);
    const tc = rd.trumpCard || {};
    if (g.power_suit !== tc.suit) {
        throw new Error(`deal mismatch: wasm trump=${g.power_suit} but replay trump=${tc.suit} — seed does not reproduce this game`);
    }
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    const C = (s: number, v: number) => ({ suit: s, value: v });

    // octogen is DETERMINISTIC given (state, game_seed, belief): its per-decision
    // world-sampling seed is state_fnv(g_rng_base) over the ORDERED hands+deck and
    // num_logs (wasm_api.c). To break ties the same way the server did, we must
    // feed the SAME belief it saw — the kernel's OWN masked session-log stream,
    // accumulated from each action's logsWire (exactly what the server stores in
    // belief_log_bytes), NOT the decoded-replay slice (which counts game_start /
    // masks differently and shifts num_logs). Timestamps aren't hashed, so any
    // value works.
    let belief = new Uint8Array(0);
    const appendBelief = (chunk: Uint8Array) => {
        const merged = new Uint8Array(belief.length + chunk.length);
        merged.set(belief); merged.set(chunk, belief.length); belief = merged;
    };
    delete g.belief_logs;
    // Seed with the game-start record the engine emitted (deal/flip) — the
    // server's session log begins with it, so octogen's num_logs starts at 1.
    if (startRun && startRun.logsWire && startRun.logsWire.length > 2) appendBelief(logsFromKernelExport(startRun.logsWire, 1));

    const records: string[] = [];
    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        if (l.seat === OGSEAT) {
            g.belief_log_bytes = belief;                     // EXACT server belief (kernel stream)
            __ogExplainDump(true);                           // clear sink
            wasmChooseMoveDirect(g, `p${OGSEAT}`, STRAT.octogen, { env });
            const dump = __ogExplainDump(true).trim();       // one og_ex_emit record
            if (dump) records.push(dump);
        }
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        let aiMask = 0; const humanSeats: number[] = [];
        g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; else humanSeats.push(k); });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, humanSeats);
        if (!run || !run.ok) { process.stderr.write(`drive stopped at log ${i} (${l.t} p${l.seat}) reason=${run?.reason}\n`); break; }
        appendBelief(logsFromKernelExport(run.logsWire, 1));  // grow the belief exactly as the server does
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
    }
    if (!records.length) throw new Error('no OG_EXPLAIN records — is the OG_EXPLAIN wasm swapped in? (make bots-wasm-explain)');
    writeFileSync(OUT, records.join('\n') + '\n');
    process.stderr.write(`wasm drive: ${records.length} deliberation records -> ${OUT} (elim=${JSON.stringify(g.elimination_order)})\n`);
});
