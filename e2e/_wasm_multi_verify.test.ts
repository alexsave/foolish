// Verify octogen reproduction using the SHIPPED wasm (no OG_EXPLAIN): re-drive
// the recorded game and, at each octogen seat, compare wasmChooseMoveDirect's
// returned move to the recorded move. Isolates whether a "differ" in the X-ray
// is real (shipped disagrees too) or an artifact of the OG_EXPLAIN build.
//
//   RECON_SEED=<hex> RECON_RD=<rd.json> OGX_OCTO_SEATS=0,1,2,3 SC_RUN=1 \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test e2e/_wasm_multi_verify.test.ts
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/wasm/engine.ts';
import { encodeAction } from '../supabase/functions/_shared/wire/awire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../supabase/functions/_shared/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';

__ensureBots();
const VAL: Record<number, string> = { 1: '2', 2: '3', 3: '4', 4: '5', 5: '6', 6: '7', 7: '8', 8: '9', 9: '10', 10: 'J', 11: 'Q', 12: 'K', 13: 'A' };
const SU = ['S', 'H', 'C', 'D'];

test('shipped-wasm octogen reproduction', { skip: !process.env.SC_RUN }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const NP = Number(rd.playerCount || 2);
    const TRUMP = (rd.trumpCard || {}).suit ?? 0;
    const OCTO = new Set((process.env.OGX_OCTO_SEATS || '').split(',').filter((s) => s !== '').map(Number));
    const tok = (s: number, v: number) => (VAL[v] || '?') + (SU[s] || '?') + (s === TRUMP ? '*' : '');
    const lab = (kind: string, cards: any[], targets?: any[]) => {
        if (kind === 'pickup') return 'pickup';
        if (kind === 'good') return 'good';
        if (kind === 'cover' && targets) return 'cover ' + cards.map((c, i) => `${tok(c.suit, c.value)}->${tok(targets[i].suit, targets[i].value)}`).join(' ');
        return `${kind} ` + cards.map((c) => tok(c.suit, c.value)).join(' ');
    };
    const norm = (s: string) => { const p = s.replace(/,/g, ' ').trim().split(/\s+/); return p[0] + ' ' + p.slice(1).sort().join(' '); };

    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: Array.from({ length: NP }, (_, i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: OCTO.has(i) ? (STRATEGY_KEY as any).OCTOGEN : (STRATEGY_KEY as any).RANDOM })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    const startRun: any = start_game_packed(g);
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    const C = (s: number, v: number) => ({ suit: s, value: v });
    let belief = new Uint8Array(0);
    const appendBelief = (chunk: Uint8Array) => { const m = new Uint8Array(belief.length + chunk.length); m.set(belief); m.set(chunk, belief.length); belief = m; };
    delete g.belief_logs;
    if (startRun && startRun.logsWire && startRun.logsWire.length > 2) appendBelief(logsFromKernelExport(startRun.logsWire, 1));

    const diffs: any[] = []; let octo = 0;
    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        if (OCTO.has(l.seat)) {
            g.belief_log_bytes = belief;
            const q: any = wasmChooseMoveDirect(g, `p${l.seat}`, STRAT.octogen, { env });
            octo++;
            const picked = q ? lab(q.type, q.cards || [], q.attack_cards) : '(null)';
            const recorded = lab(l.t, l.cards.map((c: any) => c.p), l.t === 'cover' ? l.cards.map((c: any) => c.tg) : undefined);
            if (norm(picked) !== norm(recorded)) diffs.push({ ply: i, seat: l.seat, picked, recorded });
        }
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        let aiMask = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, []);
        if (!run || !run.ok) { process.stderr.write(`stopped at ${i}\n`); break; }
        appendBelief(logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
    }
    process.stderr.write(`SHIPPED-wasm octogen: ${octo} turns, ${diffs.length} differ: ${JSON.stringify(diffs)}\n`);
});
