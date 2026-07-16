// Reconstruct a recorded game through the SAME wasm binary the live server runs,
// so octogen's decisions match the deployed game exactly (the native og_explain
// build differs from the shipped wasm: no CD_TT_* / different WORLD_LOG_CAP /
// overlay constants, so its solver — and thus a few co-optimal picks — diverge).
// Drives the recorded public moves and, at each octogen turn, queries the wasm
// octogen and compares to the recorded move.
//   node --import tsx wasm_recon.mts <replay_decoded.json> <64-hex-seed> [seat]
//
// WIP / KNOWN LIMITATION: this deals from the seed via start_game_packed +
// __setDealSeedOverride, but that deal does NOT reproduce the recorded game's
// deal (its p1 doesn't hold the card p1 attacks with on move 1) — the harness's
// seed-deal path diverges from both the native og_explain deal and the deployed
// deal. So the recorded moves fail to apply. The fix is to drive the wasm
// engine's REPLAY-decode path (which reproduces the game correctly) instead of
// re-dealing from the seed; until then this only demonstrates the query wiring.
import { readFileSync } from 'node:fs';
import { start_game_packed } from '../../../supabase/functions/_shared/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../../../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../../../sdk/ts/wire/awire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../../../sdk/ts/wasm/bots.ts';
import { game_done } from '../../../supabase/functions/_shared/common/common_utils.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../../../supabase/functions/_shared/core/types.ts';

__ensureBots();
const rd = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const seedHex = process.argv[3];
const OG = process.argv[4] ? +process.argv[4] : 1;   // octogen seat (driver default 1)
const seed = Uint8Array.from(seedHex.match(/../g)!.map((b) => parseInt(b, 16)));

const mk = (): any => ({
  players: [0, 1].map((i) => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: (STRATEGY_KEY as any).OCTOGEN,
  })),
  deck: [], logs: [], id: 'recon', name: 'recon', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
});

const key = (type: string, cards?: any[], atk?: any[]) => {
  const cs = (cards || []).map((c) => `${c.suit}.${c.value}`).sort().join(',');
  const as = (atk || []).map((c) => `${c.suit}.${c.value}`).sort().join(',');
  return `${type}|${cs}|${as}`;
};

__setDealSeedOverride(seed);
const g = mk();
start_game_packed(g);
console.error(`deal: trump=${g.power_suit} flip=${g.flipped?.suit}.${g.flipped?.value} first=${g.first_attacker} deck=${g.deck_length}`);

const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
let dec = 0, match = 0, applied = 0;
const mismatches: string[] = [];
for (const l of rd.logs) {
  if (!['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.t)) continue;
  const seat = l.seat;
  const cards = l.cards.map((c: any) => ({ suit: c.p.suit, value: c.p.value }));
  const atk = l.t === 'cover' ? l.cards.map((c: any) => ({ suit: c.tg.suit, value: c.tg.value })) : undefined;

  if (seat === OG) {
    const pick = wasmChooseMoveDirect(g, `p${seat}`, STRAT.octogen, { env });
    dec++;
    if (pick) {
      const rk = key(l.t, l.t === 'pickup' || l.t === 'good' ? [] : cards, atk);
      const pk = key(pick.type, (pick as any).cards, (pick as any).attack_cards);
      if (rk === pk) match++;
      else mismatches.push(`  log${l.i ?? applied}: rec=${rk}  wasm=${pk}`);
    }
  }

  let aiMask = 0; const humanSeats: number[] = [];
  g.players.forEach((p: any, i: number) => { if (p.is_ai) aiMask |= 1 << i; else humanSeats.push(i); });
  const move: any = { kind: l.t };
  if (l.t !== 'pickup' && l.t !== 'good') move.cards = cards;
  if (atk) move.attack_cards = atk;
  const wire = encodeAction(move);
  const run: any = runPackedGameAction(g, seat, wire, aiMask, humanSeats);
  if (!run || !run.ok) { console.error(`APPLY FAILED at log ${l.i}: ${l.t} seat${seat}`); break; }
  applyKernelStateToGame(g, run.post, `p${seat}`);
  applied++;
  if (game_done(g) !== null) { /* keep draining recorded moves */ }
}
console.log(`\nWASM reconstruction: ${match}/${dec} octogen picks match the recorded game  (drove ${applied} moves, elim=${g.elimination_order})`);
if (mismatches.length) { console.log('mismatches:'); mismatches.forEach((m) => console.log(m)); }
