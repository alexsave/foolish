// Multi-bot drive: reconstruct a seed-dealt replay through the DEPLOYED wasm and
// emit ONE deliberation record per bot decision — OG_EXPLAIN for every octogen
// seat, and a lightweight {legal, chosen} record for every other (random) seat.
// Same masked-belief discipline as _wasm_drive.test.ts, so octogen's picks are
// bit-exact. Records are keyed by ply (= kernel num_logs at the decision) and
// tagged with the acting seat, so build_data_multi.py can place each one.
//
//   RECON_SEED=<64hex> RECON_RD=<replay_decoded.json> OGX_MULTI_DELIB=<out.jsonl> \
//     OGX_OCTO_SEATS=0,1,2,3 TSX_TSCONFIG_PATH=e2e/tsconfig.json \
//     node --import tsx --test e2e/_wasm_multi_drive.test.ts
import { test } from 'node:test';
import { readFileSync, writeFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { encodeAction } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, __ogExplainDump, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();

const VAL: Record<number, string> = { 1: '2', 2: '3', 3: '4', 4: '5', 5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A' };
const SU = ['S', 'H', 'C', 'D'];

test('multi-bot deliberation drive', { skip: !process.env.OGX_MULTI_DELIB }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const OUT = process.env.OGX_MULTI_DELIB!;
    const NP = Number(rd.playerCount || 2);
    const TRUMP = (rd.trumpCard || {}).suit ?? rd.powerSuit ?? 0;
    const OCTO = new Set((process.env.OGX_OCTO_SEATS || '').split(',').filter((s) => s !== '').map(Number));

    const tok = (s: number, v: number) => (VAL[v] || '?') + (SU[s] || '?') + (s === TRUMP ? '*' : '');
    const labelFromCards = (kind: string, cards: { suit: number; value: number }[], targets?: { suit: number; value: number }[]) => {
        if (kind === 'pickup') return 'pickup';
        if (kind === 'good') return 'good';
        if (kind === 'cover' && targets) return 'cover ' + cards.map((c, i) => `${tok(c.suit, c.value)}->${tok(targets[i].suit, targets[i].value)}`).join(' ');
        return `${kind} ` + cards.map((c) => tok(c.suit, c.value)).join(' ');
    };

    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: Array.from({ length: NP }, (_, i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: OCTO.has(i) ? (STRATEGY_KEY as any).OCTOGEN : (STRATEGY_KEY as any).RANDOM })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    const startRun: any = start_game_packed(g);
    const tc = rd.trumpCard || {};
    if (g.power_suit !== tc.suit) {
        throw new Error(`deal mismatch: wasm trump=${g.power_suit} but replay trump=${tc.suit} — seed does not reproduce this game`);
    }
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    const C = (s: number, v: number) => ({ suit: s, value: v });

    // Accumulate the kernel's OWN masked session-log stream (what the server
    // stores as belief_log_bytes) so octogen sees the exact belief the server did.
    let belief = new Uint8Array(0);
    const appendBelief = (chunk: Uint8Array) => {
        const merged = new Uint8Array(belief.length + chunk.length);
        merged.set(belief); merged.set(chunk, belief.length); belief = merged;
    };
    delete g.belief_logs;
    if (startRun && startRun.logsWire && startRun.logsWire.length > 2) appendBelief(logsFromKernelExport(startRun.logsWire, 1));

    const records: string[] = [];
    let octoRecords = 0, randRecords = 0;
    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;

        if (OCTO.has(l.seat)) {
            g.belief_log_bytes = belief;                     // EXACT server belief
            __ogExplainDump(true);                           // clear sink
            wasmChooseMoveDirect(g, `p${l.seat}`, STRAT.octogen, { env });
            const dump = __ogExplainDump(true).trim();
            if (dump) { records.push(dump); octoRecords++; }
        } else {
            // Random seat: enumerate the legal moves at this public+own state and
            // mark the one the replay recorded. No RNG re-run needed — the recorded
            // move IS what random picked; the options are a deterministic function
            // of the position (full hands are reconstructed from the deal seed).
            const legalMoves = calculateLegalMoves(g, `p${l.seat}`) as any[];
            const legal = legalMoves.map((m) => labelFromCards(m.type, m.cards || [], m.attack_cards));
            const chosenTargets = l.t === 'cover' ? l.cards.map((c: any) => c.tg) : undefined;
            const chosen = labelFromCards(l.t, l.cards.map((c: any) => c.p), chosenTargets);
            const opp_counts = g.players.map((p: any) => (p.hand ? p.hand.length : p.hand_length || 0));
            records.push(JSON.stringify({ kind: 'random', ply: i, seat: l.seat, legal, chosen, opp_counts, deck: g.deck ? g.deck.length : 0 }));
            randRecords++;
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
        appendBelief(logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
    }
    if (!records.length) throw new Error('no deliberation records — is the OG_EXPLAIN wasm swapped in? (make bots-wasm-explain)');
    writeFileSync(OUT, records.join('\n') + '\n');
    process.stderr.write(`multi drive: ${octoRecords} octogen + ${randRecords} random records -> ${OUT} (elim=${JSON.stringify(g.elimination_order)})\n`);
});
