// Ad-hoc rules.wasm cover-enumeration stack canary (R4,
// docs/RULES_GUARDS_WASM_MEMORY_PLAN.md; NOT part of the suite — no .test.ts).
// The rules twin of e2e/stack_canary.mts: paints the rules shadow stack, drives
// worst-case cover + attack-combination enumeration through the production
// kernelLegalMoves marshal, and reports the high-water. Never loads bots
// (engine() stays rules.wasm). Run:
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/rules_stack_canary.mts
// Last measured worst: 14.3 KiB (cover nb=8) — 32 KiB stack is 2.23x that.
// Set STACK to the shipped -z stack-size before re-measuring.
import { kernelLegalMoves } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, Card, Battle } from '../supabase/functions/_shared/core/types.ts';

const STACK = 32768;
const seen: WebAssembly.Memory[] = [];
const RealInstance = WebAssembly.Instance;
(WebAssembly as any).Instance = function (mod: WebAssembly.Module, imports?: WebAssembly.Imports) {
  const inst = new RealInstance(mod, imports);
  const m = (inst.exports as any).memory;
  if (m instanceof WebAssembly.Memory) seen.push(m);
  return inst;
} as any;
(WebAssembly as any).Instance.prototype = RealInstance.prototype;

const card = (suit: number, value: number): Card => ({ suit, value });
const mkPlayer = (i: number, hand: Card[]): PrivatePlayer => ({
  player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.IN, is_ai: false,
  hand, awaiting_attack: false, hand_length: hand.length, strategy_key: 'human' as any,
});

// Build a state: `nb` uncovered battles (attacks all value 6, distinct-ish),
// defender holds a big hand of high cards + trumps that can cover many.
function heavyGame(nb: number, defHand: number): Game {
  const battles: Battle[] = [];
  for (let i = 0; i < nb; i++) battles.push({ attack: card(i % 4, 6), defense: null });
  // Defender hand: lots of 7..13 across suits (cover the 6s same-suit) + trumps.
  const hand: Card[] = [];
  for (let v = 7; v <= 13 && hand.length < defHand; v++)
    for (let s = 0; s < 4 && hand.length < defHand; s++) hand.push(card(s, v));
  // pad with trump (suit 0) aces if room
  while (hand.length < defHand) hand.push(card(0, 14 - (hand.length % 3)));
  const players = [
    mkPlayer(0, [card(1, 6), card(2, 6), card(3, 6)]), // attacker
    mkPlayer(1, hand),                                   // defender (seat 1)
  ];
  return {
    players, deck: [], logs: [], id: 'h', name: 'h', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: card(0, 5), power_suit: 0,
    first_attacker: 0, defender: 1, table_battles: battles, elimination_order: [],
    good_timestamp: null, good_players: [],
  } as Game;
}

// Force instantiation + capture memory.
kernelLegalMoves(heavyGame(1, 6), 'p1');
const mem = seen.sort((a, b) => b.buffer.byteLength - a.buffer.byteLength)[0];
if (!mem) { console.error('no rules memory captured'); process.exit(1); }
console.log('rules mem pages:', mem.buffer.byteLength / 65536);

const paint = () => new Uint8Array(mem.buffer).fill(0xA5, 64, STACK - 64);
const scan = () => {
  const u = new Uint8Array(mem.buffer);
  let low = 64;
  while (low < STACK - 64 && u[low] === 0xA5) low++;
  return STACK - low; // high-water bytes
};

// First-attack / attack-continuation enumeration (combinations_attack): a big
// same-value attacker hand drives the deepest attack recursion.
function attackGame(handSize: number, nbCovered: number): Game {
  const battles: Battle[] = [];
  for (let i = 0; i < nbCovered; i++) battles.push({ attack: card(0, 9), defense: card(1, 9) });
  const hand: Card[] = [];
  for (let i = 0; i < handSize; i++) hand.push(card(i % 4, 9)); // all value 9 → wide combos
  const players = [
    mkPlayer(0, hand),                                   // attacker (seat 0)
    mkPlayer(1, [card(0, 10), card(1, 10), card(2, 10)]),
  ];
  return {
    players, deck: [], logs: [], id: 'a', name: 'a', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: card(0, 5), power_suit: 0,
    first_attacker: 0, defender: 1, table_battles: battles, elimination_order: [],
    good_timestamp: null, good_players: [],
  } as Game;
}

let worst = 0, worstDesc = '';
paint();
// Cover sweep (durak rarely exceeds ~6 uncovered battles; go to 8 for margin).
for (const nb of [1, 2, 3, 4, 5, 6, 7, 8]) {
  for (const dh of [6, 12, 18, 24, 36, 48, 60]) {
    try { kernelLegalMoves(heavyGame(nb, dh), 'p1'); } catch { /* ignore */ }
  }
  const hw = scan();
  if (hw > worst) { worst = hw; worstDesc = `cover nb=${nb}`; }
}
// Attack-combination sweep (first attack + continuations, wide same-value hand).
for (const hs of [6, 12, 18, 24, 36, 48, 60]) {
  for (const nc of [0, 1, 2, 3, 4]) {
    try { kernelLegalMoves(attackGame(hs, nc), 'p0'); } catch { /* ignore */ }
  }
  const hw = scan();
  if (hw > worst) { worst = hw; worstDesc = `attack hs=${hs}`; }
}
console.log(`cover-enum stack high-water: ${worst} B (${(worst / 1024).toFixed(1)} KiB) at ${worstDesc}`);
console.log(`stack=${STACK} (${STACK / 1024} KiB); headroom = ${((STACK - worst) / 1024).toFixed(1)} KiB; ratio = ${(STACK / worst).toFixed(2)}x`);
console.log(`32KiB stack would be ${(32768 / worst).toFixed(2)}x the measured worst`);
