import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { wasmChooseMoveDirect, __ensureBots, __botsWasmBytes, STRAT } from '../sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();

// Per-decision octogen latency + bots.wasm linear-memory footprint. Drives the
// recorded game to a representative deck-alive state, then times many octogen
// decisions there (varying the world seed each iter). Set OG_TRUMP_KEEP to A/B
// the tie-break tax; swap the shipped wasm to A/B against HEAD. Skipped in CI.
test('octogen latency + memory', { skip: !process.env.BENCH_LAT }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const N = Number(process.env.BENCH_N || '120');
    const STOP = Number(process.env.BENCH_STOP_MOVE || '30'); // drive this many actions, then bench
    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    start_game_packed(g);
    const C = (s: number, v: number) => ({ suit: s, value: v });
    const cvt = (l: any) => ({ log_type: l.t, player_id: l.seat != null ? `p${l.seat}` : null, defender_index: l.def ?? -1, card_pairs: (l.cards || []).map((c: any) => ({ primary: c.p, target: c.tg })) });
    const env: Record<string, string> = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    for (const k of Object.keys(process.env)) if (k.startsWith('OG_')) env[k] = process.env[k]!;

    // drive to a mid-game deck-alive state
    let applied = 0, benchIdx = -1;
    for (let i = 0; i < rd.logs.length && applied < STOP; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        let aiMask = 0; const humanSeats: number[] = [];
        g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; else humanSeats.push(k); });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, humanSeats);
        if (!run || !run.ok) break;
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
        applied++; benchIdx = i;
    }
    g.belief_logs = rd.logs.slice(0, benchIdx + 1).map(cvt);
    const seat = 1;
    const trumps = g.players[seat].hand.filter((c: any) => c.suit === g.power_suit).length;

    const seedFor = (k: number) => (k.toString(16).padStart(2, '0') + HEX.slice(2));
    // warmup (also triggers the one-time flag load / TT alloc)
    for (let k = 0; k < 8; k++) { g.game_seed = seedFor(k); wasmChooseMoveDirect(g, `p${seat}`, STRAT.octogen, { env }); }
    const memBytes = __botsWasmBytes();

    const t: number[] = [];
    for (let k = 0; k < N; k++) {
        g.game_seed = seedFor(1000 + k);
        const t0 = Number(process.hrtime.bigint());
        wasmChooseMoveDirect(g, `p${seat}`, STRAT.octogen, { env });
        t.push((Number(process.hrtime.bigint()) - t0) / 1e6);
    }
    t.sort((a, b) => a - b);
    const pct = (p: number) => t[Math.min(t.length - 1, Math.floor(p * t.length))];
    const mean = t.reduce((a, b) => a + b, 0) / t.length;
    process.stdout.write(
        `LAT octogen  OG_TRUMP_KEEP=${env.OG_TRUMP_KEEP ?? '(default)'}  n=${N}  ` +
        `deck=${g.deck.length} handTrumps=${trumps}  ` +
        `mean=${mean.toFixed(2)}ms p50=${pct(0.5).toFixed(2)} p90=${pct(0.9).toFixed(2)} max=${t[t.length - 1].toFixed(2)}  ` +
        `wasmMem=${(memBytes / 1048576).toFixed(2)}MB (${memBytes}B)\n`,
    );
});
