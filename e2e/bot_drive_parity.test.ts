// Differential harness for the bot drive cycle (docs/C_CORE_CONSOLIDATION.md
// F2/A2): the server's TS cycle vs the kernel's, on seeded games, byte-comparing
// the products a commit actually takes.
//
// This is the de-risking step for porting bot_actions.ts:262-411 onto
// wasm_bot_drive. Three things change shape in that port, and each is a
// question this harness answers:
//
//   1. WHERE THE WORK HAPPENS. Today a bot turn is choose (chooseMoveDirect, in
//      bots.wasm) then apply (runPackedGameAction, a rules session). bot_drive
//      does both inside bots.wasm, in one call. Same move? Same bytes?
//   2. EVENT ACCUMULATION. Today events are captured per move; the drive lets
//      the hook snapshots accumulate across the whole bundled cycle. That is
//      what the broadcast wants, but it is a real semantic change.
//   3. LOG SLICING. The drive keeps the belief bots' session log resident
//      UNDER the records the cycle writes, so it exports from an offset. A bug
//      there would append the whole session to logs_packed a second time —
//      which is why the belief configs below matter most.
//
// Method: two games from the SAME pinned deal seed, played to the end in TWO
// SEPARATE PASSES — one through the kernel's cycle, one through the TS cycle —
// recording each cycle's products, then compared cycle by cycle. Separate
// passes, not interleaved ones, because the two share a wasm instance and the
// draw LCG carries across a decision: the Monte-Carlo bots SAMPLE from whatever
// value the previous action's refill left (robusta_strategy.c calls
// game_random; blackpowder/cordite/octogen save and restore it). Interleaving
// two games therefore feeds each other's leftovers into the other's search —
// which is exactly what this harness first "found", before it was the harness's
// own fault. One game per pass keeps each stream a clean function of its own
// history.
//
// The first mismatch fails the test, so a divergence is reported at the cycle
// it happens in; the states after it are meaningless and never asserted on.
//
// Two differences are deliberate and are held CONSTANT here rather than
// asserted away, so that everything else can be compared byte-for-byte:
//
//   * THE SHUFFLE. The TS cycle picks among simultaneously-eligible bots with
//     Math.random; the kernel derives the order from public state. They cannot
//     agree by construction — that is the point of the change (fairness that
//     replays). So the TS side is replayed over the seat order the kernel
//     chose. The shuffle has its own statistical, fails-if-removed test in
//     cnitro/tests/tests.c.
//   * BELIEF INSIDE A BUNDLE. The server hydrates the session log ONCE per
//     cycle, so a belief bot acting second in a bundle cannot see the `good`
//     the first bot just said. The kernel keeps the log resident across the
//     cycle, so it can. That is strictly MORE information, and only a public
//     fact the bot would have seen one cycle later anyway — an improvement that
//     rides with the port (like BUNDLED_PASSIVE = 0ms; see
//     docs/C_CORE_CONSOLIDATION.md F2). It was found by this harness: without
//     it, a blackpowder seat covered where the TS cycle had it pick up.
//     The oracle below is therefore given the same information the kernel has —
//     it refreshes belief between the actions of a cycle. With that held equal,
//     every product matches to the byte, which is the proof that intra-bundle
//     belief is the ONLY behavioral difference in the choose path.
//
// Pure kernel test — needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { processBotActionPacked } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { __setDealSeedOverride, PackedRunOk } from '../supabase/functions/_shared/wasm/engine.ts';
import {
  __ensureBots, __seedDrawRngFromState, wasmBotDrive, wasmBotEligibleMask,
  wasmBotPacingMs, BOT_PACE, BOT_STOP, BotDrivePref,
} from '../supabase/functions/_shared/wasm/bots.ts';
import { LegalMove } from '../supabase/functions/_shared/bot_interfaces.ts';
import { bytesToBareHex } from '../supabase/functions/_shared/wire/bytes.ts';
import { logsFromKernelExport } from '../supabase/functions/_shared/wire/logwire.ts';
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, GAME_MOVE_TYPE,
} from '../supabase/functions/_shared/types.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; }

__ensureBots();

// Log records carry a wall-clock stamp the kernel never sees — the server
// decorates them at commit (logsFromKernelExport(.., Date.now())). Pin it so
// the comparison is of kernel bytes, not of when the test ran.
const FIXED_TS = 1_700_000_000_000;

const mkGame = (strats: string[], id: string): Game => ({
  players: strats.map((s, i): PrivatePlayer => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0, strategy_key: s,
  })),
  deck: [], logs: [], id, name: id, status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
} as unknown as Game);

const seedFrom = (n: number): Uint8Array =>
  Uint8Array.from({ length: 32 }, (_, i) => (n * 31 + i * 7 + 11) & 0xff);

// The products a commit takes, as comparable strings.
interface Products {
  stateHex: string;
  logsHex: string;
  nEvents: number;
  eventsHex: string;   // every viewer's stream, in a stable order
  ended: boolean;
}

const eventsToHex = (events: Map<number, Uint8Array>): string =>
  [...events.keys()].sort((a, b) => a - b)
    .map(v => `${v}:${bytesToBareHex(events.get(v)!)}`).join(' ');

const productsOf = (
  stateHex: string, logsHex: string, nEvents: number,
  events: Map<number, Uint8Array>, ended: boolean,
): Products => ({ stateHex, logsHex, nEvents, eventsHex: eventsToHex(events), ended });

const runLogsHex = (run: PackedRunOk): string =>
  bytesToBareHex(logsFromKernelExport(run.logsWire, FIXED_TS));

const concatBytes = (a: Uint8Array, b: Uint8Array): Uint8Array => {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0); out.set(b, a.length);
  return out;
};

// ---------------------------------------------------------------------------
// The TS cycle, as an oracle. A faithful transcription of the inner cycle of
// bot_actions.ts:262-411 — choose+apply per seat, bundle zero-event passives,
// stop on the first visible action — over a GIVEN seat order (see the header).
// The accumulation rules are the original's: logwire records concatenate, the
// state blob is cumulative so the last one wins, and at most one bundled move
// carries events.
//
// One deliberate deviation from the original, per the header: belief is
// refreshed BETWEEN the actions of a cycle, because the kernel's bots see the
// bundle's own records and this holds that difference equal.
// ---------------------------------------------------------------------------
// A move as a comparable string — TYPE AND CARDS. Comparing only the type
// would let "attacked with two cards" pass for "attacked with four".
const cardsOf = (cs?: { suit: number; value: number }[]): string =>
  (cs ?? []).map(c => `${c.suit}.${c.value}`).join('+');
const moveKey = (seat: number, m: LegalMove): string =>
  `${seat}:${m.type}(${cardsOf(m.cards)}${m.attack_cards ? `>${cardsOf(m.attack_cards)}` : ''})`;

interface TsCycle {
  products: Products | null;
  applied: { seat: number; key: string }[];
  session: Uint8Array | null;   // the session log as this cycle left it
}

async function tsCycle(game: Game, order: number[], belief: Uint8Array | null): Promise<TsCycle> {
  let stateHex: string | null = null;
  let logsHex = '';
  let ended = false;
  let nEvents = 0;
  let events: Map<number, Uint8Array> | null = null;
  const applied: { seat: number; key: string }[] = [];
  let session = belief;

  for (const seat of order) {
    if (session) game.belief_log_bytes = session;
    const res = await processBotActionPacked(game, game.players[seat] as PrivatePlayer);
    if (!res) continue;
    if (res.run) {
      const wire = logsFromKernelExport(res.run.logsWire, FIXED_TS);
      logsHex += bytesToBareHex(wire);
      stateHex = bytesToBareHex(res.run.stateBlob);
      ended = res.run.ended;
      if (res.run.nEvents > 0) { events = res.run.events; nEvents = res.run.nEvents; }
      if (session) session = concatBytes(session, wire);
    }
    applied.push({ seat, key: moveKey(seat, res.move) });
    const passive = res.moveType === GAME_MOVE_TYPE.GOOD || res.moveType === GAME_MOVE_TYPE.WAIT;
    if (passive && (res.run?.nEvents ?? 0) === 0) continue;   // bundle
    break;
  }
  const products = stateHex === null ? null
    : productsOf(stateHex, logsHex, nEvents, events ?? new Map(), ended);
  return { products, applied, session };
}

// ---------------------------------------------------------------------------

const CONFIGS: { name: string; strats: string[]; logs: boolean }[] = [
  // Beliefless and RNG-free: any divergence is the cycle itself.
  { name: '2p simple_heuristic', strats: ['simple_heuristic', 'simple_heuristic'], logs: false },
  // handwritten_prod CONSUMES the strategy LCG — the case that proves the
  // kernel re-seeds per DECISION and not once per cycle (bot_drive_pre_action_hook).
  { name: '4p handwritten', strats: Array(4).fill('handwritten'), logs: false },
  // Mixed, with a stream-consumer acting before a stream-reader in a bundle.
  { name: '4p mixed rng', strats: ['handwritten', 'random', 'simple_heuristic', 'handwritten'], logs: false },
  // Belief bots: exercises the resident session log and the export offset.
  { name: '3p blackpowder', strats: Array(3).fill('blackpowder'), logs: true },
  // Six seats: the bundling case — several silent goods per cycle.
  { name: '6p mixed belief', strats: ['blackpowder', 'handwritten', 'firecracker', 'simple_heuristic', 'blackpowder', 'handwritten'], logs: true },
];

// One cycle of a recorded pass.
interface Cycle { keys: string[]; products: Products; pacing: number[] }

// Pass 1: the kernel drives the whole game. Records what it did, so the TS pass
// can be replayed over the same seat order.
function kernelPass(cfg: { strats: string[]; logs: boolean }, seed: Uint8Array, id: string): Cycle[] {
  __setDealSeedOverride(seed);
  const game = mkGame(cfg.strats, id);
  start_game_packed(game);
  // Both passes must open the same draw stream: the deal is ChaCha and never
  // touches this LCG, so a fresh game would otherwise inherit whatever the
  // previous game in this module instance left — and a bot that SAMPLES from it
  // (firecracker) would then choose differently for reasons that are not the
  // cycle. See __seedDrawRngFromState.
  __seedDrawRngFromState();
  const aiMask = (1 << cfg.strats.length) - 1;
  // The session log the belief bots read, carried across cycles exactly as the
  // server's bots-only loop carries it (residentBelief: hydrate once, then
  // append each cycle's committed records rather than re-reading).
  let session: Uint8Array | null = cfg.logs ? new Uint8Array(0) : null;

  const out: Cycle[] = [];
  let guard = 0;
  while (game_done(game) === null && ++guard < 500) {
    if (session) game.belief_log_bytes = session;
    // human_mask 0: every seat is a bot.
    const r = wasmBotDrive(game, { humanMask: 0, aiMask, humanSeats: [], logs: cfg.logs });
    if (r.actions.length === 0) {
      assert.equal(r.run, null, 'a cycle that applied nothing has no products');
      break;
    }
    out.push({
      keys: r.actions.map(a => moveKey(a.seat, a.move)),
      products: productsOf(bytesToBareHex(r.run!.stateBlob), runLogsHex(r.run!),
        r.run!.nEvents, r.run!.events, r.run!.ended),
      pacing: r.actions.map(a => a.pacingClass),
    });
    if (session) session = concatBytes(session, logsFromKernelExport(r.run!.logsWire, FIXED_TS));
  }
  assert.ok(guard < 500, 'kernel pass did not terminate');
  return out;
}

// Pass 2: the TS cycle plays the same game, over the seat order the kernel chose.
async function tsPass(
  cfg: { strats: string[]; logs: boolean }, seed: Uint8Array, id: string, orders: number[][],
): Promise<Cycle[]> {
  __setDealSeedOverride(seed);
  const game = mkGame(cfg.strats, id);
  start_game_packed(game);
  __seedDrawRngFromState();   // same opening stream as the kernel pass
  let session: Uint8Array | null = cfg.logs ? new Uint8Array(0) : null;

  const out: Cycle[] = [];
  for (const order of orders) {
    if (game_done(game) !== null) break;
    const r = await tsCycle(game, order, session);
    if (!r.products) break;
    out.push({ keys: r.applied.map(a => a.key), products: r.products, pacing: [] });
    session = r.session;
  }
  return out;
}

for (const cfg of CONFIGS) {
  test(`bot drive parity: ${cfg.name}`, async () => {
    let cycles = 0, actionsSeen = 0, bundled = 0;

    for (let g = 0; g < 3; g++) {
      const seed = seedFrom(g + 1);
      const id = `drive-${cfg.name}-${g}`;

      // Each pass plays its own game to the end before the other starts: the
      // module is a singleton and the draw LCG carries across a decision (see
      // the header), so interleaving would cross the two games' streams.
      const kp = kernelPass(cfg, seed, id);
      const tp = await tsPass(cfg, seed, id, kp.map(c => c.keys.map(k => Number(k.split(':')[0]))));

      for (let i = 0; i < kp.length; i++) {
        const where = `${cfg.name} game ${g} cycle ${i + 1}`;
        assert.ok(tp[i], `${where}: the TS cycle stopped early (kernel drove ${kp.length} cycles)`);
        assert.deepEqual(tp[i].keys, kp[i].keys, `${where}: the two cycles applied different moves`);
        assert.equal(kp[i].products.stateHex, tp[i].products.stateHex, `${where}: state blob differs`);
        assert.equal(kp[i].products.logsHex, tp[i].products.logsHex, `${where}: log records differ`);
        assert.equal(kp[i].products.nEvents, tp[i].products.nEvents, `${where}: event count differs`);
        assert.equal(kp[i].products.eventsHex, tp[i].products.eventsHex, `${where}: event streams differ`);
        assert.equal(kp[i].products.ended, tp[i].products.ended, `${where}: ended differs`);

        // Only the last action of a cycle may be visible; the rest bundled.
        kp[i].pacing.slice(0, -1).forEach((p, j) =>
          assert.equal(p, BOT_PACE.BUNDLED_PASSIVE, `${where}: bundled action ${j} is not silent`));

        cycles++;
        actionsSeen += kp[i].keys.length;
        if (kp[i].keys.length > 1) bundled++;
      }
      assert.equal(tp.length, kp.length, `${cfg.name} game ${g}: the passes drove a different number of cycles`);
    }

    assert.ok(cycles > 20, `${cfg.name}: too few cycles compared (${cycles})`);
    assert.ok(actionsSeen >= cycles, 'every cycle applied at least one action');
    if (cfg.strats.length > 2) {
      assert.ok(bundled > 0, `${cfg.name}: no cycle ever bundled — bundling went untested`);
    }
  });
}

// The CAS-retry path: a move offered back is replayed when still legal, and the
// products are the same as if the seat had searched for it (the kernel re-checks
// legality against the current menu — see BotDrivePref).
test('bot drive parity: a preferred move reproduces the searched one', async () => {
  const strats = ['handwritten', 'handwritten', 'blackpowder'];
  const aiMask = (1 << strats.length) - 1;
  let replayed = 0;

  for (let g = 0; g < 3; g++) {
    const seed = seedFrom(g + 40);

    __setDealSeedOverride(seed);
    const ga = mkGame(strats, `pref-a-${g}`);
    start_game_packed(ga);

    __setDealSeedOverride(seed);
    const gb = mkGame(strats, `pref-b-${g}`);
    start_game_packed(gb);

    let guard = 0;
    while (game_done(ga) === null && ++guard < 300) {
      // A: search normally.
      const ra = wasmBotDrive(ga, { humanMask: 0, aiMask, humanSeats: [], logs: true });
      if (ra.actions.length === 0) break;

      // B: hand back exactly what A chose. Same state, so every pref is still
      // legal and must be replayed rather than re-searched.
      const prefs: BotDrivePref[] = ra.actions.map(a => ({ seat: a.seat, move: a.move }));
      const rb = wasmBotDrive(gb, { humanMask: 0, aiMask, humanSeats: [], logs: true, prefs });

      assert.deepEqual(rb.actions.map(a => moveKey(a.seat, a.move)),
        ra.actions.map(a => moveKey(a.seat, a.move)),
        `pref game ${g} cycle ${guard}: replayed moves differ from the searched ones`);
      assert.equal(bytesToBareHex(rb.run!.stateBlob), bytesToBareHex(ra.run!.stateBlob),
        `pref game ${g} cycle ${guard}: state blob differs`);
      assert.equal(runLogsHex(rb.run!), runLogsHex(ra.run!),
        `pref game ${g} cycle ${guard}: log records differ`);
      replayed += ra.actions.length;
    }
  }
  assert.ok(replayed > 30, `too few preferred moves exercised (${replayed})`);
});

// A stale pref must be ignored, not played: legality is re-checked against the
// CURRENT menu, so an impossible offer costs a re-choose and nothing else.
test('bot drive: a stale preferred move is re-chosen, not trusted', () => {
  const strats = ['handwritten', 'handwritten'];
  __setDealSeedOverride(seedFrom(99));
  const game = mkGame(strats, 'pref-stale');
  start_game_packed(game);

  // An attack with a card no one can hold in a fresh deal is never on the menu.
  const bogus: BotDrivePref[] = [0, 1].map(seat => ({
    seat, move: { type: 'attack' as const, cards: [{ suit: 0, value: 2 }] },
  }));
  const withBogus = wasmBotDrive(game, {
    humanMask: 0, aiMask: 0x3, humanSeats: [], prefs: bogus,
  });

  __setDealSeedOverride(seedFrom(99));
  const clean = mkGame(strats, 'pref-stale');
  start_game_packed(clean);
  const withNone = wasmBotDrive(clean, { humanMask: 0, aiMask: 0x3, humanSeats: [] });

  assert.deepEqual(withBogus.actions.map(a => a.move.type), withNone.actions.map(a => a.move.type),
    'a stale pref changed the move that was played');
  assert.equal(bytesToBareHex(withBogus.run!.stateBlob), bytesToBareHex(withNone.run!.stateBlob),
    'a stale pref changed the committed state');
});

// The kernel must never drive a seat the host owns, however eligible it is.
test('bot drive: human_mask seats are never driven', () => {
  const strats = ['handwritten', 'handwritten', 'handwritten'];
  __setDealSeedOverride(seedFrom(7));
  const game = mkGame(strats, 'human-mask');
  start_game_packed(game);

  let guard = 0;
  while (game_done(game) === null && ++guard < 200) {
    const mask = wasmBotEligibleMask(game, 0x1);
    assert.equal(mask & 0x1, 0, 'a masked seat is never reported eligible');
    const r = wasmBotDrive(game, { humanMask: 0x1, aiMask: 0x7, humanSeats: [0] });
    if (r.actions.length === 0) {
      assert.equal(r.stop, game_done(game) === null ? BOT_STOP.NO_ELIGIBLE : BOT_STOP.ENDED);
      break;   // seat 0 is owed a move and no bot can act
    }
    assert.ok(r.actions.every(a => a.seat !== 0), 'the kernel drove a masked seat');
  }
});

// The pacing table is the kernel's, reached through the bridge — never mirrored
// in TS (docs/C_CORE_CONSOLIDATION.md F3).
test('bot pacing comes from the kernel table', () => {
  assert.equal(wasmBotPacingMs(BOT_PACE.MOVE, true), 3000);
  assert.equal(wasmBotPacingMs(BOT_PACE.MOVE, false), 300);
  assert.equal(wasmBotPacingMs(BOT_PACE.ROUND_TRANSITION, true), 3000);
  // The deliberate change that rides with the port: a cycle where nothing
  // became visible costs nothing, where the server used to pause the full
  // 3000ms for it (docs/C_CORE_CONSOLIDATION.md F3).
  assert.equal(wasmBotPacingMs(BOT_PACE.BUNDLED_PASSIVE, true), 0);
  assert.equal(wasmBotPacingMs(BOT_PACE.NONE, true), 0);
});
