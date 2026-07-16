// Mint a shareable v6 replay URL from a dump_game.c moves JSON.
//   ./build/dump_game <seed> > moves.json
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx \
//     sdk/c/tools/endgame_retro/encode_replay.mjs moves.json
// Drives the recorded octogen moves through the TS kernel from the seed deal
// (so the logs are the engine's own, guaranteed valid) and encodes+verifies a
// Format-6 replay. Prints the foolish.cards URL.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
const R = process.env.FOOLISH_ROOT ||
  resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
process.env.TSX_TSCONFIG_PATH ||= `${R}/e2e/tsconfig.json`;

const { start_game_packed, reconstructSeededDeal } = await import(`${R}/supabase/functions/_shared/common/game_lifecycle.ts`);
const { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } = await import(`${R}/sdk/ts/wasm/engine.ts`);
const { encodeAction } = await import(`${R}/sdk/ts/wire/awire.ts`);
const { logsFromKernelExport, decodeLogs } = await import(`${R}/sdk/ts/wire/logwire.ts`);
const { verifyRoundTripV6 } = await import(`${R}/supabase/functions/_shared/common/replay/encode.ts`);
const { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } = await import(`${R}/supabase/functions/_shared/core/types.ts`);

const spec = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const SEED = spec.seed;
const seedBytes = Uint8Array.from(SEED.match(/../g).map((b) => parseInt(b, 16)));
const C = ([s, v]) => ({ suit: s, value: v });

const g = {
  players: Array.from({ length: 2 }, (_, i) => ({ player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: STRATEGY_KEY.OCTOGEN })),
  deck: [], logs: [], game_seed: SEED, id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0,
  flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [],
};
__setDealSeedOverride(seedBytes);
const startRun = start_game_packed(g);
__setDealSeedOverride(null);

const gameLogs = [];
let ts = 1;
const absorb = (w) => { if (!w || w.length <= 2) return; const d = logsFromKernelExport(w, ts); ts += 700; for (const l of decodeLogs(d, g.id, g.players)) gameLogs.push(l); };
absorb(startRun.logsWire);

const mkMove = (m) => { const mv = { kind: m.type }; if (m.type !== 'pickup' && m.type !== 'good') mv.cards = (m.cards || []).map(C); if (m.type === 'cover') mv.attack_cards = (m.attack || []).map(C); return mv; };
let applied = 0;
for (const m of spec.moves) {
  let aiMask = 0; g.players.forEach((p, k) => { if (p.is_ai) aiMask |= 1 << k; });
  const run = runPackedGameAction(g, m.seat, encodeAction(mkMove(m)), aiMask, []);
  if (!run || !run.ok) { console.error(`STOP move#${applied} ply=${m.ply} ${m.type}: ${run && run.reason}`); break; }
  absorb(run.logsWire);
  applyKernelStateToGame(g, run.post, `p${m.seat}`);
  applied++;
}
console.error(`applied ${applied}/${spec.moves.length}; status=${g.status}; elim=${JSON.stringify(g.elimination_order)}`);

const { initialHands, stock, flip } = reconstructSeededDeal(SEED, g.players.map((p) => ({ player_id: p.player_id })));
const { encoded, decoded } = await verifyRoundTripV6({ playerIds: ['p0', 'p1'], logs: gameLogs, flipped: flip, initialHands, stock });
const info = decoded.logs.filter((l) => ['attack', 'cover', 'pass', 'pickup', 'good'].includes(l.log_type)).length;
console.log('URL:', encoded.url);
console.log(`bytes: ${encoded.byteLength}  actions: ${info}  round-trip: OK`);
