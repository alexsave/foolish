// Drive the EXACT recorded picks from a kernel-path generator record (gen.json,
// from _wasm_4v4_gen) through the DEPLOYED wasm, dumping OG_EXPLAIN for every
// octogen seat and a {legal, chosen} record for every other seat. Structurally
// mirrors _wasm_multi_verify (which reproduces octogen 100% on exact picks) so
// the deliberation is byte-faithful to how the wasm bot actually played — no
// spurious "would differ" flags. Records are keyed by ply (= accumulated masked
// log count = kernel num_logs) so multi_page.py places each on gen.rd.logs.
//
//   RECON_SEED=<hex> OGX_GEN_JSON=<gen.json> OGX_MULTI_DELIB=<out.jsonl> \
//     OGX_OCTO_SEATS=0,1,2,3 TSX_TSCONFIG_PATH=e2e/tsconfig.json \
//     node --import tsx --test e2e/_wasm_multi_exact.test.ts
import { test } from 'node:test';
import { readFileSync, writeFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { calculateLegalMoves } from '../supabase/functions/_shared/common/bot_strategy.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { logsFromKernelExport, decodeLogs } from '../sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, __ogExplainDump, STRAT } from '../sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();
const VAL: Record<number, string> = { 1: '2', 2: '3', 3: '4', 4: '5', 5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A' };
const SU = ['S', 'H', 'C', 'D'];

test('exact-move multi-bot deliberation drive', { skip: !process.env.OGX_MULTI_DELIB }, () => {
    const HEX = process.env.RECON_SEED!;
    const gen = JSON.parse(readFileSync(process.env.OGX_GEN_JSON!, 'utf8'));
    const OUT = process.env.OGX_MULTI_DELIB!;
    const NP = Number(gen.playerCount || gen.rd.playerCount || 8);
    const TRUMP = (gen.rd.trumpCard || {}).suit ?? gen.rd.powerSuit ?? 0;
    const OCTO = new Set((process.env.OGX_OCTO_SEATS || gen.octogenSeats.join(',')).split(',').filter((s: string) => s !== '').map(Number));
    const C = (s: number, v: number) => ({ suit: s, value: v });

    const tok = (s: number, v: number) => (VAL[v] || '?') + (SU[s] || '?') + (s === TRUMP ? '*' : '');
    const labelFrom = (kind: string, cards: any[], targets?: any[]) => {
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
    if (g.power_suit !== (gen.rd.trumpCard || {}).suit) {
        throw new Error(`deal mismatch: wasm trump=${g.power_suit} but record trump=${(gen.rd.trumpCard || {}).suit}`);
    }
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };

    // Belief = accumulated masked kernel session log (== belief_log_bytes). Track
    // nlogs (decoded record count) purely to key each decision by its ply.
    let belief = new Uint8Array(0);
    let nlogs = 0;
    const appendBelief = (chunk: Uint8Array) => { const m = new Uint8Array(belief.length + chunk.length); m.set(belief); m.set(chunk, belief.length); belief = m; };
    delete g.belief_logs;
    if (startRun && startRun.logsWire && startRun.logsWire.length > 2) {
        const w = logsFromKernelExport(startRun.logsWire, 1);
        appendBelief(w); nlogs += decodeLogs(w, g.id, g.players).length;
    }

    // Normalized step list — identical shape/construction to _wasm_multi_verify.
    const steps = gen.moves.map((m: any) => ({ seat: m.seat, type: m.type, cards: m.cards || [], atk: m.attack_cards }));

    const records: string[] = []; let octoN = 0, randN = 0;
    for (const s of steps) {
        const ply = nlogs;
        if (OCTO.has(s.seat)) {
            g.belief_log_bytes = belief;
            __ogExplainDump(true);
            wasmChooseMoveDirect(g, `p${s.seat}`, STRAT.octogen, { env });
            const dump = __ogExplainDump(true).trim();
            if (dump) { records.push(dump); octoN++; }
        } else {
            g.belief_log_bytes = belief;
            const legalMoves = calculateLegalMoves(g, `p${s.seat}`) as any[];
            const legal = legalMoves.map((m) => labelFrom(m.type, m.cards || [], m.attack_cards));
            const chosen = labelFrom(s.type, s.cards, s.atk);
            const opp_counts = g.players.map((p: any) => (p.hand ? p.hand.length : p.hand_length || 0));
            records.push(JSON.stringify({ kind: 'random', ply, seat: s.seat, legal, chosen, opp_counts, deck: g.deck ? g.deck.length : 0 }));
            randN++;
        }
        let aiMask = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; });
        const move: any = { kind: s.type };
        if (s.type !== 'pickup' && s.type !== 'good') move.cards = s.cards.map((c: any) => C(c.suit, c.value));
        if (s.atk) move.attack_cards = s.atk.map((c: any) => C(c.suit, c.value));
        const run: any = runPackedGameAction(g, s.seat, encodeAction(move), aiMask, []);
        if (!run || !run.ok) { process.stderr.write(`drive stopped (${s.type} p${s.seat}) reason=${run?.reason}\n`); break; }
        if (run.logsWire && run.logsWire.length > 2) {
            const w = logsFromKernelExport(run.logsWire, 1);
            appendBelief(w); nlogs += decodeLogs(w, g.id, g.players).length;
        }
        applyKernelStateToGame(g, run.post, `p${s.seat}`);
    }
    if (!records.length) throw new Error('no records — is the OG_EXPLAIN wasm swapped in? (make bots-wasm-explain)');
    writeFileSync(OUT, records.join('\n') + '\n');
    process.stderr.write(`exact drive: ${octoN} octogen + ${randN} random records -> ${OUT} (elim=${JSON.stringify(g.elimination_order)})\n`);
});
