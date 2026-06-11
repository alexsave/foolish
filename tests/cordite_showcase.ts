/* =============================================================================
 * Cordite replay showcase — translator + scorer
 * =============================================================================
 * Reads all-cordite games dumped by `cnitro_showcase` (one JSON game per line),
 * translates each C game-log stream into the replay codec's ReplayInput, scores
 * each for "drama" (comebacks, see-saws, frontrunner collapses), and encodes
 * the wildest into a self-contained replay URL — names + per-move timing
 * included — after verifying the round-trip.
 *
 *   cnitro_showcase ... > games.jsonl
 *   tsx tests/cordite_showcase.ts games.jsonl
 * ========================================================================== */

import * as fs from "fs";
import {
  ReplayInput,
  ReplayLogEntry,
  DecodedReplay,
  INFO_TYPES,
} from "../supabase/functions/_shared/replay/core.ts";
import { verifyRoundTrip } from "../supabase/functions/_shared/replay/encode.ts";
import { LOG_TYPE, LogType } from "../supabase/functions/_shared/types.ts";
import { URL_PREFIX } from "../supabase/functions/_shared/replay/codec.ts";
import {
  encodeExtras,
  joinReplayCode,
} from "../supabase/functions/_shared/replay/extras.ts";
import { buildReplaySteps } from "../src/replay/view";

// silence the engine's chatter (the codec itself is quiet, but be safe)
const print = console.log.bind(console);
console.warn = () => {};
console.info = () => {};

// C LOG_* enum order (cnitro/src/game.h) -> TS LogType strings.
const C_LOG: LogType[] = [
  LOG_TYPE.GAME_START,
  LOG_TYPE.ATTACK,
  LOG_TYPE.COVER,
  LOG_TYPE.PASS,
  LOG_TYPE.PICKUP,
  LOG_TYPE.GOOD,
  LOG_TYPE.DISCARD,
  LOG_TYPE.DEFENDER_CHANGE,
  LOG_TYPE.PLAYER_OUT,
  LOG_TYPE.DRAW,
];

type CPair = [number, number, number, number]; // ps, pv, ts, tv (-1 = none)
type CLog = [number, number, number, CPair[]]; // type, pidx, didx, pairs
interface CGame {
  np: number;
  seed: number;
  flip: [number, number];
  logs: CLog[];
}

function toReplayInput(g: CGame): ReplayInput {
  const playerIds = Array.from({ length: g.np }, (_, i) => `p${i}`);
  const logs: ReplayLogEntry[] = g.logs.map(([t, pidx, didx, pairs]) => ({
    log_type: C_LOG[t],
    player_id: pidx < 0 ? null : `p${pidx}`,
    card_pairs: pairs.map(([ps, pv, ts, tv]) => ({
      primary: { suit: ps, value: pv },
      target: ts < 0 ? null : { suit: ts, value: tv },
    })),
    defender_index: didx < 0 ? null : didx,
  }));
  return { playerIds, logs, flipped: { suit: g.flip[0], value: g.flip[1] } };
}

interface Score {
  total: number;
  parts: Record<string, number>;
  peaks: number[];
  foolSeat: number;
}

// Drama model: deep escape (a buried survivor), frontrunner collapse, see-saw
// lead changes, and a photo finish (many late eliminations, long game).
function scoreGame(d: DecodedReplay): Score {
  const steps = buildReplaySteps(d as any);
  const n = d.playerCount;
  const hist: number[][] = steps.map((s: any) =>
    s.players.map((p: any) => p.hidden + p.known.length),
  );
  const T = hist.length;
  const foolSeat = d.fool;

  const peak = new Array(n).fill(0);
  for (const row of hist) for (let s = 0; s < n; s++) peak[s] = Math.max(peak[s], row[s]);

  let escapePeak = 0;
  for (let s = 0; s < n; s++) if (s !== foolSeat) escapePeak = Math.max(escapePeak, peak[s]);

  let foolLateMin = Infinity;
  for (let t = Math.floor(T / 2); t < T; t++) {
    const h = hist[t][foolSeat];
    if (h > 0) foolLateMin = Math.min(foolLateMin, h);
  }
  if (!isFinite(foolLateMin)) foolLateMin = 0;
  const collapse = Math.max(0, peak[foolSeat] - foolLateMin);

  let leadChanges = 0;
  let prevLeader = -1;
  for (const row of hist) {
    let lead = 0;
    for (let s = 1; s < n; s++) if (row[s] > row[lead]) lead = s;
    if (lead !== prevLeader && prevLeader !== -1) leadChanges++;
    prevLeader = lead;
  }

  const elimSpread = d.eliminationOrder.length;
  const parts = {
    escape: escapePeak * 3.0,
    collapse: collapse * 1.5,
    seesaw: leadChanges * 2.5,
    finish: elimSpread * 4.0,
    length: Math.min(T, 200) / 10,
  };
  const total = Object.values(parts).reduce((a, b) => a + b, 0);
  return { total, parts, peaks: peak, foolSeat };
}

// Synthesize plausible per-move "thinking" times for a bot game: a fixed
// start epoch + varied few-second gaps (deterministic in the move index).
function synthMoveTimes(moveCount: number): number[] {
  const t0 = 1749600000; // a fixed mid-2026 epoch, for reproducible URLs
  const times = [t0];
  let t = t0;
  for (let i = 0; i < moveCount; i++) {
    const gap = 1.2 + ((i * 2654435761) % 1000) / 250; // ~1.2..5.2s, varied
    t += gap;
    times.push(t);
  }
  return times;
}

(function main() {
  const path = process.argv[2];
  if (!path) {
    print("usage: tsx tests/cordite_showcase.ts <games.jsonl>");
    process.exit(2);
  }
  const lines = fs
    .readFileSync(path, "utf8")
    .split("\n")
    .map((l) => l.trim())
    .filter(Boolean);
  print(`read ${lines.length} games from ${path}`);

  let best:
    | { g: CGame; d: DecodedReplay; enc: any; score: Score }
    | null = null;
  let fails = 0;

  for (const line of lines) {
    let g: CGame;
    try {
      g = JSON.parse(line) as CGame;
    } catch {
      continue;
    }
    const input = toReplayInput(g);
    let res;
    try {
      res = verifyRoundTrip(input); // throws on any desync
    } catch (e: any) {
      fails++;
      if (fails <= 3) print(`  skip np=${g.np} seed=${g.seed}: ${e.message}`);
      continue;
    }
    const score = scoreGame(res.decoded);
    if (!best || score.total > best.score.total) {
      best = { g, d: res.decoded, enc: res.encoded, score };
      print(
        `  new best np=${g.np} seed=${g.seed} score=${score.total.toFixed(1)} ` +
          `${res.encoded.byteLength}B peaks=[${score.peaks.join(",")}] ` +
          `fool=seat${score.foolSeat} [${Object.entries(score.parts)
            .map(([k, v]) => `${k}=${(v as number).toFixed(0)}`)
            .join(" ")}]`,
      );
    }
  }

  if (!best) {
    print(`no encodable game (${fails} failed round-trip)`);
    process.exit(1);
  }

  // names + per-move timing extras
  const names = Array.from({ length: best.g.np }, (_, i) => `Cordite ${i + 1}`);
  const moveCount = best.d.logs.filter((l) => INFO_TYPES.includes(l.log_type))
    .length;
  const extras = encodeExtras(names, synthMoveTimes(moveCount));
  const url = URL_PREFIX + joinReplayCode(best.enc.base32, extras);

  print("\n========================================================");
  print(
    `WILDEST: ${best.g.np} cordite bots  score=${best.score.total.toFixed(1)}  ` +
      `(${lines.length - fails} encodable, ${fails} failed)`,
  );
  print(`  drama: ${JSON.stringify(best.score.parts)}`);
  print(`  peak hand sizes by seat: ${best.score.peaks.join(", ")}`);
  print(
    `  elimination order (seats): ${best.d.eliminationOrder.join(" -> ")}  fool=seat ${best.d.fool}`,
  );
  print(`  moves: ${moveCount}   replay size: ${best.enc.byteLength} bytes`);
  print("\n  URL:");
  print(`  ${url}`);
  print("========================================================");
})();
