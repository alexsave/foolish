// Regression guard for the resident-state marshal cache (engine.ts residentFor).
//
// wasmChooseMove marshals the game and marks it "resident" so the action that
// immediately follows can skip re-marshaling. marshalGame decides to skip on
// OBJECT IDENTITY. The bot loop reuses ONE game object across decisions and can
// mutate it out-of-band (state reload on a CAS conflict, round-transition
// refill, passive-action bundling), so a later choose used to skip the marshal
// and decide against STALE kernel state — most damagingly a since-emptied deck
// still reading as alive, which gates off the exact endgame solver and throws
// forced wins (see fix: readers always marshal fresh).
//
// Oracle: a state READ through a reused-then-mutated game object must produce
// the SAME move as the same state read through a fresh object. Both use the
// same wasm + same fixed seed, so any divergence is pure state-plumbing.
// This test FAILS on the pre-fix bridge and PASSES once readers marshal fresh.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { STRAT, wasmChooseMoveDirect, __setBotSeedSource } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { Card, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

// Deterministic PRNG so failures reproduce from the seed.
function lcg(seed: number) {
  let s = seed >>> 0;
  return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; };
}

const idCard = (id: number): Card => ({ suit: Math.floor(id / 13), value: (id % 13) + 1 });
const mkP = (i: number, hand: Card[], status: string, awaiting: boolean, strat: string) =>
  ({ player_id: `p${i}`, name: `p${i}`, status, is_ai: true, hand, awaiting_attack: awaiting,
     hand_length: hand.length, strategy_key: strat } as any);

// A random, structurally-valid state: acting seat is the first attacker on an
// empty table (so it always has legal attacks), plus a random deck / flipped
// trump / OUT players — exercising the marshalled deck & flip fields.
function randState(rnd: () => number, strat: string): any {
  const ri = (n: number) => Math.floor(rnd() * n);
  const np = 2 + ri(5);
  const large = np >= 6;
  const cards: number[] = [];
  for (let s = 0; s < 4; s++) for (let v = large ? 1 : 5; v <= 13; v++) cards.push(s * 13 + (v - 1));
  for (let i = cards.length - 1; i > 0; i--) { const j = ri(i + 1); [cards[i], cards[j]] = [cards[j], cards[i]]; }
  let k = 0;
  const players: any[] = [];
  const inSeats: number[] = [];
  for (let i = 0; i < np; i++) {
    const out = i > 0 && rnd() < 0.25;
    const hn = out ? 0 : 1 + ri(6);
    const hand = out ? [] : cards.slice(k, k + hn).map(idCard);
    k += hn;
    players.push(mkP(i, hand, out ? PLAYER_STATUS.OUT : PLAYER_STATUS.IN, false, strat));
    if (!out) inSeats.push(i);
  }
  if (inSeats.length < 2) return null;
  const attacker = inSeats[0];
  const defender = inSeats[1];
  players[attacker].awaiting_attack = true;
  const rest = cards.slice(k);
  const flipped = rnd() < 0.5 && rest.length ? idCard(rest.shift()!) : null;
  const deck = rest.slice(0, ri(Math.min(rest.length, 8) + 1)).map(idCard);
  return {
    id: 'g' + ri(1e9), status: GAME_STATUS.PLAYING, players, power_suit: ri(4),
    first_attacker: attacker, defender, discard_pile_length: ri(40),
    flipped, deck, good_players: [], good_timestamp: null, table_battles: [],
    elimination_order: [], logs: [],
  };
}

const env = (strat: number) => strat === STRAT.cordite
  ? { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' } : {};
const wantLogs = (strat: number) => strat === STRAT.cordite;

function choose(g: any, strat: number): any {
  const actor = g.players.find((p: any) => p.awaiting_attack)?.player_id;
  return wasmChooseMoveDirect(g, actor, strat, { env: env(strat), logs: wantLogs(strat) });
}
const clone = (g: any) => structuredClone(g);
const cid = (c: any) => (c == null ? 'x' : `${c.suit},${c.value}`);
const moveKey = (m: any) => (m == null ? 'NULL'
  : `${m.type}|${(m.cards || []).map(cid).sort().join(' ')}|${(m.attack_cards || []).map(cid).sort().join(' ')}`);

// Prime the shared wasm instance's resident cache with a DIFFERENT state on the
// SAME object reference, then mutate that object to `target` and choose — the
// exact bot-loop hazard (object reused + mutated out-of-band between decisions).
function chooseViaReusedObject(target: any, strat: number, rnd: () => number): any {
  const G = clone(randState(rnd, strat === STRAT.cordite ? 'cordite' : 'handwritten') || target);
  choose(G, strat);                 // sets residentFor = G, kernel holds G's (stale) state
  Object.assign(G, clone(target));  // out-of-band mutation to the target state
  return choose(G, strat);          // pre-fix: skips marshal -> decides on stale state
}

function runDifferential(name: string, strat: number, N: number, seed: number) {
  __setBotSeedSource(() => 0x9e3779b9); // fixed strategy RNG -> identical across fresh/reused
  const rnd = lcg(seed);
  let checked = 0;
  for (let i = 0; i < N; i++) {
    const st = randState(rnd, strat === STRAT.cordite ? 'cordite' : 'handwritten');
    if (!st) continue;
    const expected = choose(clone(st), strat);
    if (expected == null) continue;
    const actual = chooseViaReusedObject(st, strat, rnd);
    checked++;
    assert.equal(moveKey(actual), moveKey(expected),
      `${name}: reused-object choose diverged from fresh-object choose\n  state=${JSON.stringify(st)}`);
  }
  assert.ok(checked > N * 0.5, `${name}: too few comparable states (${checked}/${N})`);
}

test('marshal: reused game object does not serve stale state to handwritten', () => {
  runDifferential('handwritten', STRAT.handwritten, 1500, 12345);
});

test('marshal: reused game object does not serve stale state to cordite', () => {
  runDifferential('cordite', STRAT.cordite, 250, 6789);
});
