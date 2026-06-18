// Client reconciliation simulation.
//
// Q1: does the client reconcile after a temporary WS disconnect (lost broadcast
//     packets), or do cards from different bouts end up on the table together?
// Q2: do out-of-order / dropped deliveries make optimistic state + the merge
//     glitch (covered cards un-covering, phantom cards)?
//
// Method: run a REAL multi-bout game (real handlers + real commit) to capture the
// exact per-broadcast animation snapshots the focus player's client would
// receive, plus the server's authoritative live-card set at every version. Then
// replay that stream through the client's REAL merge logic (client_merge.ts,
// ported verbatim from ServerContext.tsx) under three delivery regimes and audit
// the resulting client state.
//
//   npx tsx tests/stress/client_sim.ts [--bouts=N] [--drop=K] [--trials=T] [--blatency=ms]

import { randomUUID } from 'crypto';
import { Game } from '../../supabase/functions/_shared/types.ts';
import { start_game } from '../../supabase/functions/_shared/common_utils.ts';
import { resetDb, seedGame, loadCompleteGame, commitGame, SeedPlayer } from './db.ts';
import { checkWinSync } from './orchestrator.ts';
import { legalMoves } from './moves.ts';
import { applyMove } from './apply.ts';
import { mergeGameData } from './client_merge.ts';

const args = process.argv.slice(2);
const flag = (n: string, d: number) => {
  const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? Number(a.split('=')[1]) : d;
};
const MAX_VERSIONS = flag('versions', 220);
const TRIALS = flag('trials', 200);
const BLATENCY = flag('blatency', 120);

const key = (c: { suit: number; value: number }) => `${c.suit}:${c.value}`;
const tableKeys = (battles: any[]): string[] => {
  const out: string[] = [];
  for (const b of battles) { out.push(key(b.attack)); if (b.defense) out.push(key(b.defense)); }
  return out;
};
const liveKeysOf = (g: Game): Set<string> => {
  const s = new Set<string>();
  for (const c of g.deck) s.add(key(c));
  if (g.flipped) s.add(key(g.flipped));
  for (const p of g.players) for (const c of p.hand) s.add(key(c));
  for (const b of g.table_battles) { s.add(key(b.attack)); if (b.defense) s.add(key(b.defense)); }
  return s;
};

// One captured broadcast as the focus client would receive it: a series of event
// snapshots (we only need the public table state for card-placement correctness)
// plus the final game, each tagged with the committed version.
interface Broadcast {
  version: number;
  // bout index (increments every server table-clear) the FINAL state belongs to
  bout: number;
  eventTables: any[][];   // table_battles after each event in the sequence
  finalTable: any[];      // final message.game.table_battles
  serverLive: Set<string>; // server authoritative live-card keys at this version
}

interface Stream { initialTable: any[]; broadcasts: Broadcast[]; finalLive: Set<string>; finalTable: any[]; }

// ---- Phase 1: capture a real broadcast stream from a multi-bout game --------
async function capture(): Promise<Stream> {
  const gameId = `cs_${randomUUID().slice(0, 6)}`;
  const players: SeedPlayer[] = [
    { id: randomUUID(), name: 'Hero', is_ai: false, strategy_key: 'human' },
    { id: randomUUID(), name: 'BotA', is_ai: true, strategy_key: 'random' },
    { id: randomUUID(), name: 'BotB', is_ai: true, strategy_key: 'random' },
  ];
  await seedGame(gameId, players);

  const broadcasts: Broadcast[] = [];
  let bout = 0;

  const commit = async (events: any[]) => {
    const g = await loadCompleteGame(gameId);
    const expected = g.version ?? 0;
    return { g, expected };
  };

  // start
  {
    const g = await loadCompleteGame(gameId);
    const expected = g.version ?? 0;
    const events = start_game(g) as any[];
    checkWinSync(g);
    const res = await commitGame(g, expected);
    broadcasts.push({
      version: res.version!, bout,
      eventTables: events.map((e) => (e.game_state?.table_battles ?? []).map(cloneBattle)),
      finalTable: g.table_battles.map(cloneBattle), serverLive: liveKeysOf(g),
    });
  }

  const initialTable = (broadcasts[0].finalTable ?? []);

  let steps = 0;
  while (broadcasts.length < MAX_VERSIONS && steps < MAX_VERSIONS * 4) {
    steps++;
    const g = await loadCompleteGame(gameId);
    if (g.status !== 'playing') break;
    const tableBefore = g.table_battles.length;
    const moves = legalMoves(g);
    if (moves.length === 0) break;
    const m = moves[Math.floor(Math.random() * moves.length)];
    const expected = g.version ?? 0;
    let events: any[];
    try { events = applyMove(g, m).events as any[]; } catch { continue; }
    checkWinSync(g);
    const res = await commitGame(g, expected);
    if (res.status !== 'ok') continue;
    // a server-side table clear (round transition) bumps the bout counter
    if (tableBefore > 0 && g.table_battles.length === 0) bout++;
    broadcasts.push({
      version: res.version!, bout,
      eventTables: events.map((e) => (e.game_state?.table_battles ?? []).map(cloneBattle)),
      finalTable: g.table_battles.map(cloneBattle), serverLive: liveKeysOf(g),
    });
  }

  const last = await loadCompleteGame(gameId);
  return { initialTable, broadcasts, finalLive: liveKeysOf(last), finalTable: last.table_battles.map(cloneBattle) };
}

const cloneBattle = (b: any) => ({ attack: { ...b.attack }, defense: b.defense ? { ...b.defense } : null });

// ---- Phase 2: replay the stream through the client merge --------------------
interface Glitch {
  phantomPeak: number;     // most cards on the client table that the server had already retired
  crossBoutPeak: number;   // most distinct bouts represented on the client table at once
  uncoverEvents: number;   // a battle that was covered became uncovered in the client view
  finalMismatch: boolean;  // client final table != server final table
}

type Regime = 'in-order' | 'reordered' | 'disconnect';

function buildDelivery(stream: Stream, regime: Regime): Broadcast[] {
  const bs = stream.broadcasts;
  if (regime === 'in-order') return [...bs];
  if (regime === 'disconnect') {
    // Drop exactly the round-transition (table-clear) broadcast — a brief WS
    // outage that happens to span a bout change. This is the single packet whose
    // loss the client cannot recover from (no refetch on reconnect).
    const clears = bs.map((b, i) => ({ b, i })).filter(({ b, i }) => i > 0 && bs[i - 1].finalTable.length > 0 && b.finalTable.length === 0);
    if (clears.length === 0) return [...bs];
    const c = clears[Math.floor(Math.random() * clears.length)].i;
    return bs.filter((_, i) => i !== c);
  }
  // reordered: assign each broadcast a random arrival time (emit order + jitter)
  return bs
    .map((b, i) => ({ b, t: i * 2 + Math.random() * BLATENCY }))
    .sort((a, z) => a.t - z.t)
    .map((x) => x.b);
}

function replay(stream: Stream, regime: Regime): Glitch {
  // client starts from the dealt state (as loadGame would give it)
  let client: any = { table_battles: stream.initialTable.map(cloneBattle), self: { hand: [] } };
  // server's authoritative table at each version, to compare the client against
  const serverTableByVersion = new Map<number, any[]>();
  for (const b of stream.broadcasts) serverTableByVersion.set(b.version, b.finalTable);

  let maxVer = -1;
  let phantomPeak = 0; let crossBoutPeak = 0; let uncoverEvents = 0;
  // per-attack covered state in the CURRENT client table, to detect a battle
  // that was shown covered then shown uncovered while still on the table.
  const coverState = new Map<string, string | null>(); // attackKey -> defenseKey|null

  const delivery = buildDelivery(stream, regime);
  for (const bc of delivery) {
    // apply each event snapshot, then the final — exactly the client's per-event
    // commit + final commit.
    const snapshots = [...bc.eventTables, bc.finalTable];
    for (const tb of snapshots) {
      client = mergeGameData(client, { table_battles: tb.map(cloneBattle) });
    }
    maxVer = Math.max(maxVer, bc.version);

    // --- audits on the resulting client table, vs the server's authoritative
    //     table at the newest version the client has applied ---
    const ct = client.table_battles as any[];
    const expected = serverTableByVersion.get(maxVer) ?? [];
    const expectedByAttack = new Map(expected.map((b) => [key(b.attack), b]));

    let phantoms = 0; let legit = 0;
    const presentAttacks = new Set<string>();
    for (const battle of ct) {
      const ak = key(battle.attack);
      presentAttacks.add(ak);
      const exp = expectedByAttack.get(ak);
      // EXTRA card the server's current table doesn't have (stale-bout battle), or
      // a defense the server's current battle doesn't have (stale cover/uncover).
      const extra = !exp || (!!battle.defense && (!exp.defense || key(exp.defense) !== key(battle.defense)));
      if (extra) phantoms++; else legit++;

      // un-cover: a battle shown covered in the client's CURRENT table, now shown
      // uncovered while still present (a covering card visibly disappears).
      const prev = coverState.get(ak);
      if (prev != null && battle.defense == null) uncoverEvents++;
      coverState.set(ak, battle.defense ? key(battle.defense) : null);
    }
    for (const k of [...coverState.keys()]) if (!presentAttacks.has(k)) coverState.delete(k);

    phantomPeak = Math.max(phantomPeak, phantoms);
    // cross-bout: a stale card coexisting with the current bout's real cards
    if (phantoms > 0 && legit > 0) crossBoutPeak = Math.max(crossBoutPeak, phantoms + legit);
  }

  // final reconciliation vs server authoritative table
  const cKeys = tableKeys(client.table_battles).sort();
  const sKeys = tableKeys(stream.finalTable).sort();
  const finalMismatch = JSON.stringify(cKeys) !== JSON.stringify(sKeys);
  return { phantomPeak, crossBoutPeak, uncoverEvents, finalMismatch };
}

async function main(): Promise<void> {
  console.log('=== Foolish client reconciliation sim ===');
  await resetDb();

  const agg: Record<Regime, { phantom: number; crossBout: number; uncover: number; mismatch: number; trials: number }> = {
    'in-order': { phantom: 0, crossBout: 0, uncover: 0, mismatch: 0, trials: 0 },
    'reordered': { phantom: 0, crossBout: 0, uncover: 0, mismatch: 0, trials: 0 },
    'disconnect': { phantom: 0, crossBout: 0, uncover: 0, mismatch: 0, trials: 0 },
  };
  let multiBout = 0;

  for (let t = 0; t < TRIALS; t++) {
    process.stdout.write(`trial ${t + 1}/${TRIALS}...\r`);
    await resetDb();
    const stream = await capture();
    if (stream.broadcasts[stream.broadcasts.length - 1].bout > 0) multiBout++;
    for (const regime of ['in-order', 'reordered', 'disconnect'] as Regime[]) {
      const g = replay(stream, regime);
      const a = agg[regime];
      a.trials++;
      if (g.phantomPeak > 0) a.phantom++;
      if (g.crossBoutPeak > 1) a.crossBout++;
      if (g.uncoverEvents > 0) a.uncover++;
      if (g.finalMismatch) a.mismatch++;
    }
  }

  console.log(`\n=== results over ${TRIALS} real games (${multiBout} reached a 2nd bout) ===`);
  console.log(`regime       | trials w/ phantom-card | w/ cross-bout table | w/ un-cover | w/ final-table mismatch`);
  for (const regime of ['in-order', 'reordered', 'disconnect'] as Regime[]) {
    const a = agg[regime];
    console.log(
      `${regime.padEnd(12)} | ${String(a.phantom).padStart(20)} | ${String(a.crossBout).padStart(19)} | ${String(a.uncover).padStart(11)} | ${String(a.mismatch).padStart(22)}`);
  }
  process.exit(0);
}

main().catch((e) => { console.error(e); process.exit(1); });
