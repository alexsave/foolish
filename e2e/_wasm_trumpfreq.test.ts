import { test } from 'node:test';
import { readFileSync, writeFileSync } from 'node:fs';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { encodeAction } from '../supabase/functions/_shared/sdk/ts/wire/awire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../supabase/functions/_shared/types.ts';

__ensureBots();

// At each seat-1 ATTACK decision while the deck is still alive, sample the
// deployed octogen's chosen move across many world-sample seeds (game_seed) and
// tally how often it LEADS with a trump. This measures octogen's actual
// propensity to "dump trump early" at real board states from the recorded game.
// Analysis harness: set RECON_SEED + RECON_RD to run. Skipped in CI (no env).
test('trump-lead frequency across world seeds', { skip: !process.env.RECON_SEED }, () => {
    const HEX = process.env.RECON_SEED!;
    const rd = JSON.parse(readFileSync(process.env.RECON_RD!, 'utf8'));
    const N = Number(process.env.RECON_N || '128');
    __setDealSeedOverride(Uint8Array.from(HEX.match(/../g)!.map((b) => parseInt(b, 16))));
    const g: any = {
        players: [0, 1].map((i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: (STRATEGY_KEY as any).OCTOGEN })),
        deck: [], logs: [], belief_logs: [], game_seed: HEX, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
    };
    start_game_packed(g);
    const out: string[] = [];
    const say = (s: string) => { out.push(s); };
    const deckLen = () => (g.deck && g.deck.length) || g.deck_length || 0;
    say(`deal trump=${g.power_suit} deck_length=${g.deck_length} deck_count=${g.deck_count} deck.len=${g.deck?.length}`);
    const TRUMP = g.power_suit;
    const env: Record<string, string> = { CD_BUDGET: process.env.RECON_BUDGET || 'prod', CD_RACE: '1', CD_RACE_C: process.env.RECON_RACE_C || '75' };
    for (const k of Object.keys(process.env)) if (k.startsWith('OG_')) env[k] = process.env[k]!;
    say(`env ${JSON.stringify(env)}`);
    const C = (s: number, v: number) => ({ suit: s, value: v });
    const cvt = (l: any) => ({ log_type: l.t, player_id: l.seat != null ? `p${l.seat}` : null, defender_index: l.def ?? -1, card_pairs: (l.cards || []).map((c: any) => ({ primary: c.p, target: c.tg })) });
    // engine small-deck values: 5..13 == 6,7,8,9,10,J,Q,K,A
    const FACE = ['?', '?', '?', '?', '?', '6', '7', '8', '9', '10', 'J', 'Q', 'K', 'A'];
    const nm = (c: any) => `${FACE[c.value] ?? c.value}${['S', 'H', 'C', 'D'][c.suit]}${c.suit === TRUMP ? '*' : ''}`;
    // 64-hex seed strings derived by flipping a byte — varies the world sample.
    const seedFor = (k: number) => (k.toString(16).padStart(2, '0') + HEX.slice(2));

    for (let i = 0; i < rd.logs.length; i++) {
        const l = rd.logs[i];
        if (['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t) && l.seat === 1 && l.t === 'attack' && deckLen() > 0) {
            const hand = g.players[1].hand.slice();
            const trumps = hand.filter((c: any) => c.suit === TRUMP).length;
            const allTrump = trumps === hand.length;
            g.belief_logs = rd.logs.slice(0, i).map(cvt);
            let leadTrump = 0, ok = 0;
            const distinct = new Map<string, number>();
            for (let k = 0; k < N; k++) {
                g.game_seed = seedFor(k);
                const pick: any = wasmChooseMoveDirect(g, 'p1', STRAT.octogen, { env });
                if (!pick || pick.type !== 'attack') continue;
                ok++;
                const label = pick.cards.map(nm).sort().join(',');
                distinct.set(label, (distinct.get(label) || 0) + 1);
                if (pick.cards.some((c: any) => c.suit === TRUMP)) leadTrump++;
            }
            const recMove = l.cards.map((c: any) => nm(C(c.p.suit, c.p.value))).sort().join(',');
            const recTrump = l.cards.some((c: any) => c.p.suit === TRUMP);
            const top = [...distinct.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4).map(([m, c]) => `${m}:${c}`).join('  ');
            say(`[attack ply, deck=${deckLen()}] hand=[${hand.map(nm).join(' ')}]  trumps=${trumps}${allTrump ? ' (ALL TRUMP-forced)' : ''}`);
            say(`   recorded=${recMove}${recTrump ? ' (TRUMP)' : ''}   P(lead trump over ${ok} seeds)=${(100 * leadTrump / Math.max(1, ok)).toFixed(1)}%`);
            say(`   distribution: ${top}`);
        }
        // drive recorded move (only player actions; engine generates draw/discard/etc.)
        if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
        const cards = l.cards.map((c: any) => C(c.p.suit, c.p.value));
        const atk = l.t === 'cover' ? l.cards.map((c: any) => C(c.tg.suit, c.tg.value)) : undefined;
        let aiMask = 0; const humanSeats: number[] = [];
        g.players.forEach((p: any, kk: number) => { if (p.is_ai) aiMask |= 1 << kk; else humanSeats.push(kk); });
        const move: any = { kind: l.t };
        if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
        if (atk) move.attack_cards = atk;
        const run: any = runPackedGameAction(g, l.seat, encodeAction(move), aiMask, humanSeats);
        if (!run || !run.ok) { say(`APPLY FAILED ${l.t} p${l.seat}`); break; }
        applyKernelStateToGame(g, run.post, `p${l.seat}`);
    }
    writeFileSync('/tmp/trumpfreq.txt', out.join('\n'));
});
