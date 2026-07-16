// Adversarial fuzz of the C/WASM kernel MARSHALING BOUNDARY.
//
// The kernel (cnitro, compiled to rules.wasm + bots.wasm) is freestanding C
// with no libc bounds checking: get_state() reads caller-supplied counts
// (num_players, hand_count, deck_count, num_battles, ...) as loop bounds into
// fixed arrays, the legal-move enumerator indexes value-keyed stack arrays,
// and the bot bitboards do `1 << card_id`. A malformed Game must NEVER crash
// the module, corrupt memory, or hang it (combinatorial move blow-up) —
// upstream TS validation is the gate that REJECTS bad moves, but the kernel
// is the single source of truth and defends itself regardless.
//
// This locks in the hardening from the adversarial-hardening pass: every
// bound in get_state is clamped to capacity, every card is range-sanitized,
// and the attack/pass enumerators bound k by defender/next capacity (so they
// can't explore ~2^n doomed subsets). Pure kernel test — no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { kernelLegalMoves, kernelGameDone, kernelShouldAct } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { wasmChooseMove, STRAT } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { Game, Card, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const card = (s: number, v: number): Card => ({ suit: s, value: v });
const mkP = (i: number, hand: Card[] = [], status = PLAYER_STATUS.IN) => ({
  player_id: `p${i}`, name: `P${i}`, status, is_ai: true,
  hand, awaiting_attack: false, hand_length: hand.length, strategy_key: 'random',
} as any);
const base = (np: number): any => ({
  players: Array.from({ length: Math.max(1, Math.min(np, 8)) }, (_, i) =>
    mkP(i, [card(0, 1 + (i % 13)), card(1, 2 + (i % 12)), card(2, 3 + (i % 11))])),
  deck: [], logs: [], id: 'fuzz', name: 'fuzz', status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 1, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
});

const CRASH = /table index|out of bounds|unreachable|memory access|RuntimeError/i;

// A thrown JS error is an acceptable rejection; a wasm trap or a >2s hang is not.
function rulesSafe(g: any): void {
  const t0 = Date.now();
  try {
    const moves = kernelLegalMoves(g, g.players[0]?.player_id ?? 'p0');
    kernelGameDone(g);
    for (const p of g.players) if (p?.player_id) kernelShouldAct(g, p.player_id);
    assert.ok(Array.isArray(moves), 'moves is an array');
    assert.ok(moves.length <= 70000, `move count bounded, got ${moves.length}`);
    for (const m of moves) if (m.cards) for (const c of m.cards)
      assert.ok(c.suit >= -1 && c.suit <= 3 && c.value >= -1 && c.value <= 13,
        `enumerated card in range s${c.suit} v${c.value}`);
  } catch (e: any) {
    assert.ok(!CRASH.test(String(e?.message ?? e)), `no wasm trap: ${String(e?.message).split('\n')[0]}`);
  }
  assert.ok(Date.now() - t0 < 2000, 'no DoS hang');
}

test('kernel rules path survives malformed states (overflow / OOB / DoS classes)', () => {
  const cases: [string, (g: any) => void][] = [
    ['num_players=200', g => { for (let i = 0; i < 200; i++) g.players.push(mkP(i)); }],
    ['hand_count=100 same value', g => { g.players[0].hand = Array.from({ length: 100 }, () => card(0, 1)); }],
    ['hand_count=300 same value', g => { g.players[0].hand = Array.from({ length: 300 }, () => card(0, 1)); }],
    ['deck=500', g => { g.deck = Array.from({ length: 500 }, () => card(0, 1)); }],
    ['num_battles=200', g => { g.table_battles = Array.from({ length: 200 }, () => ({ attack: card(0, 5), defense: null, has_defense: false })); }],
    ['out-of-range cards', g => { g.players[0].hand = [card(99, 99), card(-5, -5), card(0, 100), card(7, 14)]; }],
    ['power_suit=99', g => { g.power_suit = 99; }],
    ['defender=250 first_attacker=-1', g => { g.defender = 250; g.first_attacker = -1; }],
    ['cover-combo explosion', g => {
      g.players[0].hand = Array.from({ length: 40 }, (_, i) => card(i % 4, 1 + (i % 13)));
      g.table_battles = Array.from({ length: 20 }, () => ({ attack: card(0, 5), defense: null, has_defense: false }));
      g.defender = 0;
    }],
    ['first-attack 52-card hand', g => { g.players[0].hand = Array.from({ length: 52 }, (_, i) => card(Math.floor(i / 13), 1 + (i % 13))); }],
    ['NaN / fractional fields', g => { g.power_suit = NaN; g.players[0].hand = [card(1.5, 5.9), card(0, 5)]; }],
    ['null cards in hand', g => { g.players[0].hand = [null, undefined, card(0, 5)] as any; }],
  ];
  for (const [name, mutate] of cases) {
    const g = base(4);
    mutate(g);
    assert.doesNotThrow(() => rulesSafe(g), name);
  }
});

test('kernel bot path survives malformed states (1<<card_id / belief build)', () => {
  const strats: [string, number, any][] = [
    // Every entry must be a brain bots.wasm actually LINKS: an unlinked strat is
    // not fuzzing the bot path, it is fuzzing `random`, because wasm_choose_move
    // used to fall back to it. 'champion' and 'fulminate' sat here and are in
    // neither the module nor the roster, so this test has been quietly running
    // `random` twice for its whole life. The tiny CD_W* budgets are a deliberate
    // env override — env still beats the roster's knobs (bot_knobs.h), which is
    // what keeps a Monte-Carlo fuzz fast.
    ['random', STRAT.random, {}],
    ['firecracker', STRAT.firecracker, { logs: true }],
    ['cordite', STRAT.cordite, { env: { CD_BUDGET: 'prod', CD_W1: '4', CD_W2: '4', CD_W3: '4' }, logs: true }],
    ['octogen', STRAT.octogen, { env: { CD_BUDGET: 'prod', CD_W1: '4', CD_W2: '4', CD_W3: '4' }, logs: true }],
    // ⚠ FAILING ON PURPOSE — 'firecracker' above traps ("memory access out of
    // bounds") on the 40-battle mutation below, as the FIRST malformed board it
    // sees:
    //
    //   table_battles = 40 x { attack: card(88,88), defense: card(-3,-3) }
    //   wasmChooseMove(g, 'p0', STRAT.firecracker, { logs: true })
    //
    // Pre-existing and NOT the A1 roster fold: the committed pre-A1 bots.wasm,
    // which dispatched firecracker through the old switch, traps identically.
    // It hid behind the two unlinked names ('champion'/'fulminate') that used to
    // stand here, because an unlinked strat fell back to `random`. Firecracker
    // is a SEEDED ladder rung — humans play it — so this stays red rather than
    // being quietly dropped from the list.
    //
    // Order-dependent: a small malformed board first, and the 40-battle one then
    // passes. So it is persistent scratch, not the card-value tables (those were
    // a genuine OOB write and ARE fixed — card_mark_value/card_has_value in
    // card.h). Prime suspect: the shared MC slots in cordite_sim.c, where
    // world_scratch_game() is a log-CAPPED Game slot.
  ];
  const mutate: ((g: any) => void)[] = [
    g => { g.players[0].hand = [card(99, 99), card(-5, -5), card(7, 14)]; },
    g => { g.power_suit = -7; },
    g => { g.deck = Array.from({ length: 200 }, () => card(9, 30)); },
    g => { g.table_battles = Array.from({ length: 40 }, () => ({ attack: card(88, 88), defense: card(-3, -3), has_defense: true })); },
    g => { g.logs = Array.from({ length: 900 }, () => ({ log_type: 'attack', player_id: 'p0', card_pairs: [{ primary: card(99, 99), target: card(-9, -9) }] })); },
  ];
  for (const m of mutate) {
    const g = base(4); m(g);
    for (const [name, sid, opts] of strats) {
      const t0 = Date.now();
      try {
        const idx = wasmChooseMove(g, g.players[0].player_id, sid, opts);
        assert.ok(typeof idx === 'number' && Number.isFinite(idx), `${name} returns finite index`);
      } catch (e: any) {
        assert.ok(!CRASH.test(String(e?.message ?? e)), `${name} no wasm trap`);
      }
      assert.ok(Date.now() - t0 < 3000, `${name} no DoS hang`);
    }
  }
});

// Randomized: thousands of structurally-wild games through the rules path.
test('kernel rules path survives randomized malformed states', () => {
  let seed = 0xBADF00D >>> 0;
  const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
  const ri = (n: number) => Math.floor(rnd() * n);
  const wild = () => card([-5, -1, 0, 1, 2, 3, 7, 50, 127][ri(9)], [-9, -1, 0, 1, 6, 13, 14, 99, 127][ri(9)]);
  for (let iter = 0; iter < 1500; iter++) {
    const np = Math.max(1, Math.min([0, 1, 2, 4, 6, 8, 20][ri(7)], 8));
    const g: any = base(np);
    g.players = Array.from({ length: np }, (_, i) =>
      mkP(i, Array.from({ length: ri(80) }, wild), [PLAYER_STATUS.IN, PLAYER_STATUS.OUT][ri(2)]));
    g.deck = Array.from({ length: ri(120) }, wild);
    g.table_battles = Array.from({ length: ri(50) }, () => ({ attack: wild(), defense: wild(), has_defense: rnd() < 0.5 }));
    g.power_suit = [-1, 0, 1, 2, 3, 99][ri(6)];
    g.defender = ri(300) - 50;
    g.first_attacker = ri(300) - 50;
    g.flipped = rnd() < 0.5 ? wild() : null;
    rulesSafe(g);
  }
});
