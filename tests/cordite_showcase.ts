/* =============================================================================
 * Cordite replay showcase — finalist encoder
 * =============================================================================
 * Reads the top-K most dramatic all-cordite games dumped by `cnitro_showcase`
 * (already scored + ranked in C, highest first), and encodes the first one
 * that round-trips through the replay codec into a self-contained URL — player
 * names + per-move timing included.
 *
 * Scoring already happened in C during generation, so we only pay the codec
 * round-trip cost on these few finalists, not on every game played.
 *
 *   cnitro_showcase --games=300 --pcs=4,6 --top=10 > finalists.jsonl
 *   tsx tests/cordite_showcase.ts finalists.jsonl
 * ========================================================================== */

import * as fs from "fs";
import {
  ReplayInput,
  ReplayLogEntry,
  INFO_TYPES,
} from "../supabase/functions/_shared/replay/core.ts";
import { verifyRoundTrip } from "../supabase/functions/_shared/replay/encode.ts";
import { LOG_TYPE, LogType } from "../supabase/functions/_shared/types.ts";
import { URL_PREFIX } from "../supabase/functions/_shared/replay/codec.ts";
import {
  encodeExtras,
  joinReplayCode,
} from "../supabase/functions/_shared/replay/extras.ts";

const print = console.log.bind(console);

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
  score: number;
  fool: number;
  flip: [number, number];
  peaks: number[];
  elim: number[];
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
    print("usage: tsx tests/cordite_showcase.ts <finalists.jsonl>");
    process.exit(2);
  }
  const finalists: CGame[] = fs
    .readFileSync(path, "utf8")
    .split("\n")
    .map((l) => l.trim())
    .filter(Boolean)
    .map((l) => JSON.parse(l) as CGame);
  print(`read ${finalists.length} finalists from ${path} (pre-ranked in C)`);

  // finalists arrive highest-score-first; take the first that round-trips.
  for (let rank = 0; rank < finalists.length; rank++) {
    const g = finalists[rank];
    const input = toReplayInput(g);
    let enc, dec;
    try {
      const rt = verifyRoundTrip(input);
      enc = rt.encoded;
      dec = rt.decoded;
    } catch (e: any) {
      print(
        `  #${rank + 1} np=${g.np} seed=${g.seed} score=${g.score} ` +
          `FAILED round-trip (${e.message}) — trying next`,
      );
      continue;
    }

    const namePrefix = process.argv[3] || "Cordite";
    const names = Array.from({ length: g.np }, (_, i) => `${namePrefix} ${i + 1}`);
    const moves = dec.logs.filter((l) => INFO_TYPES.includes(l.log_type)).length;
    const extras = encodeExtras(names, synthMoveTimes(moves));
    const url = URL_PREFIX + joinReplayCode(enc.base32, extras);

    print("\n========================================================");
    print(
      `WILDEST (finalist #${rank + 1} that round-trips): ` +
        `${g.np} cordite bots  score=${g.score}`,
    );
    print(`  peak hand sizes by seat: ${g.peaks.join(", ")}`);
    print(`  elimination order (seats): ${g.elim.join(" -> ")}  fool=seat ${g.fool}`);
    print(`  moves: ${moves}   replay size: ${enc.byteLength} bytes`);
    print("\n  URL:");
    print(`  ${url}`);
    print("========================================================");
    return;
  }

  print("no finalist round-tripped");
  process.exit(1);
})();
