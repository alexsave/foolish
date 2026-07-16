import { test } from 'node:test';
import { readFileSync, writeFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, appendLogs, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { wasmChooseMoveDirect, __ensureBots, __ogExplainDump, STRAT } from '../sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/core/types.ts';

__ensureBots();

// Analysis harness: needs RECON_SEED + RECON_RD and a bots.wasm built with
// -DOG_EXPLAIN_BUILD (make bots-wasm-explain) swapped in. Skipped in CI.
test('exact wasm reconstruction of the new game', { skip: !process.env.RECON_SEED }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    start_game_packed(g);
    console.log(`deal: trump=${g.power_suit} flip=${g.flipped?.suit}.${g.flipped?.value} first=${g.first_attacker}`);

    const C = (s: number, v: number) => ({ suit: s, value: v });
    const key = (t: string, cards?: any[], atk?: any[]) => {
        const cs = (cards || []).map((c) => `${c.suit}.${c.value}`).sort().join(',');
        const as = (atk || []).map((c) => `${c.suit}.${c.value}`).sort().join(',');
        return `${t}|${cs}|${as}`;
    };
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    // The decoded replay IS the public log stream octogen sees (hidden draws etc).
    // Feed logs[0..i) as belief_logs at each decision; drive the ACTION moves
    // through the engine to keep the STATE (hands/table/deck) exact.
    const cvt = (l: any) => ({
        log_type: l.t, player_id: l.seat != null ? `p${l.seat}` : null,
        defender_index: l.def ?? -1,
        card_pairs: (l.cards || []).map((c: any) => ({ primary: c.p, target: c.tg })),
    });
    const beliefUpTo = (i: number) => rd.logs.slice(0, i).map(cvt);
    let dec = 0, match = 0, applied = 0;
    const dumps: any[] = [];
    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        if (l.seat === 1) {
            g.belief_logs = beliefUpTo(i);   // octogen's public deduction input
            const realHand = g.players[1].hand.map((c: any) => `${c.value}.${c.suit}`).join(' ');
            __ogExplainDump(true);
            const pick = wasmChooseMoveDirect(g, 'p1', STRAT.octogen, { env });
            const dump = __ogExplainDump(true);
            dec++;
            if (pick) {
                const rk = key(l.t, l.t === 'pickup' || l.t === 'good' ? [] : cards, atk);
                const pk = key(pick.type, (pick as any).cards, (pick as any).attack_cards);
                if (rk === pk) match++;
                let rec = null; try { rec = JSON.parse((dump.trim().split('\n').pop()) || '{}'); } catch { /* */ }
                dumps.push({ rk, pk, agree: rk === pk, realHand, delib: rec });
            }
        }
        let aiMask = 0; const humanSeats: number[] = [];
        g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; else humanSeats.push(k); });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, humanSeats);
        if (!run || !run.ok) { console.log(`APPLY FAILED at move#${applied + 1} ${l.t} p${l.seat} reject=${run?.reason}`); break; }
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
        applied++;
    }
    console.log(`DEPLOYED octogen match: ${match}/${dec}  (drove ${applied} moves, elim=${JSON.stringify(g.elimination_order)})`);
    writeFileSync('/tmp/wasm_delib.json', JSON.stringify(dumps));
    console.log('wrote /tmp/wasm_delib.json (' + dumps.length + ' decisions)');
});
