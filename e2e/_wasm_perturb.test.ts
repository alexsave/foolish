// At a target octogen decision, re-query with the OPPONENT's hand (and the deck)
// in several orders. octogen never SEES the opponent's hand, but its per-decision
// world-sampling seed is state_fnv() over the ORDERED hands+deck (wasm_api.c), so
// a different hidden order => a different seed => a different tie-break. If the
// pick flips across orderings, the reconstruction can't be exact from a public
// replay alone (the private hand ORDER isn't recorded) — it's not an engine bug.
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { encodeAction } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';

__ensureBots();

test('octogen pick sensitivity to hidden ordering', { skip: !process.env.RECON_SEED }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const TARGET = Number(process.env.PERTURB_LOG || '6');   // replay log index of the octogen decision
    const OG = 1;
    const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
    const C = (s: number, v: number) => ({ suit: s, value: v });
    const nm = (c: any) => `${c.value}.${c.suit}`;
    const cat = (a: Uint8Array, b: Uint8Array) => { const m = new Uint8Array(a.length + b.length); m.set(a); m.set(b, a.length); return m; };
    const kmove = (m: any) => `${m.type}|${(m.cards || []).map(nm).sort().join(',')}`;

    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
        deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], game_seed: HEX,
    };
    const startRun: any = start_game_packed(g);
    let belief = startRun?.logsWire?.length > 2 ? logsFromKernelExport(startRun.logsWire, 1) : new Uint8Array(0);

    const recordedMove = kmove({ type: rd.logs[TARGET].t, cards: (rd.logs[TARGET].cards || []).map((c: any) => C(c.p.suit, c.p.value)) });
    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        if (i === TARGET && l.seat === OG) {
            g.belief_log_bytes = belief;
            const oppHand = g.players[0].hand.slice();
            const deck = g.deck.slice();
            const rev = <T,>(a: T[]) => [...a].reverse();
            const sortc = (a: any[]) => [...a].sort((x, y) => (x.suit - y.suit) || (x.value - y.value));
            const variants: Array<[string, any[], any[]]> = [
                ['as-driven          ', oppHand, deck],
                ['opp reversed       ', rev(oppHand), deck],
                ['opp sorted         ', sortc(oppHand), deck],
                ['deck reversed      ', oppHand, rev(deck)],
                ['opp+deck reversed  ', rev(oppHand), rev(deck)],
            ];
            console.log(`\n=== log ${TARGET}: octogen hand [${g.players[1].hand.map((c: any) => nm(c)).join(' ')}]  deck=${deck.length}  opp holds ${oppHand.length} ===`);
            console.log(`recorded (server) move: ${recordedMove}`);
            const picks = new Set<string>();
            for (const [label, oh, dk] of variants) {
                g.players[0].hand = oh; g.deck = dk;
                const q: any = wasmChooseMoveDirect(g, `p${OG}`, STRAT.octogen, { env });
                const k = q ? kmove({ type: q.type, cards: q.cards }) : 'null';
                picks.add(k);
                console.log(`  ${label} -> ${k}${k === recordedMove ? '   <== matches recorded' : ''}`);
            }
            console.log(`distinct picks across ${variants.length} hidden orderings: ${picks.size}`);
            g.players[0].hand = oppHand; g.deck = deck;
            break;
        }
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        let aiMask = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) aiMask |= 1 << k; });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, []);
        if (!run || !run.ok) { console.log(`stuck at ${i}`); break; }
        belief = cat(belief, logsFromKernelExport(run.logsWire, 1));
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
    }
});
