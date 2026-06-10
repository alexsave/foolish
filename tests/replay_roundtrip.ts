/* =============================================================================
 * Replay codec property test (run with: npm run test:replay [-- games-per-pc])
 * =============================================================================
 * Plays random legal games with the REAL server engine (the same
 * handleAttack/handleCover/... the edge functions run), then:
 *
 *   game.logs -> encodeReplay -> bigint -> bytes -> base32/base64
 *            -> decodeReplay (public-state replayer, decode direction)
 *
 * and asserts the decoded stream reproduces the ENTIRE original log stream —
 * not just the coded actions but every derived DISCARD / DRAW /
 * DEFENDER_CHANGE / PLAYER_OUT, byte for byte. Any rules drift between the
 * server engine and _shared/replay/core.ts shows up here as a hard failure.
 * ========================================================================== */

import {
  start_game,
  game_done,
} from "../supabase/functions/_shared/common_utils.ts";
import {
  Game,
  GameLog,
  GAME_STATUS,
  PLAYER_STATUS,
  PrivatePlayer,
  StrategyKey,
  LOG_TYPE,
} from "../supabase/functions/_shared/types.ts";
import {
  shouldBotActCore,
  processBotAction,
} from "../supabase/functions/_shared/pure_bot_actions.ts";
import { calculateLegalMoves } from "../supabase/functions/_shared/bot_strategy.ts";
import {
  ReplayInput,
  SeatLog,
  DecodedReplay,
} from "../supabase/functions/_shared/replay/core.ts";
import {
  encodeReplay,
  verifyRoundTrip,
} from "../supabase/functions/_shared/replay/encode.ts";
import { decodeReplay } from "../supabase/functions/_shared/replay/decode.ts";
import {
  urlToGame,
  base64Decode,
  bytesToBigint,
} from "../supabase/functions/_shared/replay/codec.ts";
import { buildReplaySteps } from "../src/replay/view";
import {
  encodeExtras,
  decodeExtras,
  splitReplayCode,
  joinReplayCode,
  quantizeGap,
  dequantizeGap,
  moveTimesFromLogs,
} from "../supabase/functions/_shared/replay/extras.ts";
import { INFO_TYPES } from "../supabase/functions/_shared/replay/core.ts";

// Silence the very chatty engine
const saved = console.log.bind(console);
console.log = () => {};
console.warn = () => {};
console.error = () => {};
console.info = () => {};
const print = saved;

const GAMES_PER_PC = Number(process.argv[2] ?? 150);
const MAX_ACTIONS = 100000;

const mkPlayer = (i: number, strategy: StrategyKey): PrivatePlayer => ({
  player_id: `bot_${i}`,
  name: `Bot ${i}`,
  status: PLAYER_STATUS.READY,
  is_ai: true,
  hand: [],
  awaiting_attack: false,
  hand_length: 0,
  strategy_key: strategy,
});

const mkGame = (np: number, strategy: StrategyKey): Game => ({
  players: Array.from({ length: np }, (_, i) => mkPlayer(i, strategy)),
  deck: [],
  logs: [],
  id: "g",
  name: "g",
  status: GAME_STATUS.PLAYING,
  deck_length: 0,
  discard_pile_length: 0,
  flipped: null,
  power_suit: 0,
  first_attacker: 0,
  defender: 0,
  table_battles: [],
  elimination_order: [],
  good_timestamp: null,
  good_players: [],
});

async function playRandomGame(
  np: number,
  strategy: StrategyKey,
): Promise<Game | null> {
  const game = mkGame(np, strategy);
  start_game(game);
  let actions = 0;
  while (game_done(game) === null) {
    if (++actions > MAX_ACTIONS) return null; // stalled, skip (harness issue)
    const eligible: PrivatePlayer[] = [];
    for (let i = 0; i < game.players.length; i++) {
      const p = game.players[i];
      if (
        shouldBotActCore(game, p, i) &&
        calculateLegalMoves(game, p.player_id).length > 0
      ) {
        eligible.push(p);
      }
    }
    if (eligible.length === 0) return null;
    const order = [...eligible];
    for (let i = order.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [order[i], order[j]] = [order[j], order[i]];
    }
    let acted = false;
    for (const p of order) {
      if (await processBotAction(game, p)) {
        acted = true;
        break;
      }
    }
    if (!acted) return null;
  }
  return game;
}

/* normalize both streams to a comparable shape */
function normOriginal(game: Game): SeatLog[] {
  const seatOf = (pid: string | null) =>
    pid === null ? null : game.players.findIndex((p) => p.player_id === pid);
  return game.logs.map((l: GameLog) => ({
    log_type: l.log_type,
    seat: seatOf(l.player_id),
    card_pairs: l.card_pairs.map((p) => ({
      primary: { suit: p.primary.suit, value: p.primary.value },
      target: p.target
        ? { suit: p.target.suit, value: p.target.value }
        : null,
    })),
    defender_index: l.defender_index ?? null,
  }));
}

function normDecoded(d: DecodedReplay): SeatLog[] {
  return d.logs.map((l) => ({
    log_type: l.log_type,
    seat: l.seat,
    card_pairs: l.card_pairs.map((p) => ({
      primary: { suit: p.primary.suit, value: p.primary.value },
      target: p.target
        ? { suit: p.target.suit, value: p.target.value }
        : null,
    })),
    defender_index: l.defender_index ?? null,
  }));
}

function diffStreams(a: SeatLog[], b: SeatLog[]): string | null {
  const ja = a.map((l) => JSON.stringify(l));
  const jb = b.map((l) => JSON.stringify(l));
  const len = Math.max(ja.length, jb.length);
  for (let i = 0; i < len; i++) {
    if (ja[i] !== jb[i]) {
      return `entry ${i}:\n  original: ${ja[i] ?? "<end>"}\n  decoded:  ${jb[i] ?? "<end>"}`;
    }
  }
  return null;
}

(async () => {
  let totalGames = 0;
  let failures = 0;
  let stalled = 0;

  for (let np = 2; np <= 8; np++) {
    const sizes: number[] = [];
    const byStrategy: Record<string, number[]> = {};
    let pcFails = 0;
    for (let g = 0; g < GAMES_PER_PC; g++) {
      // alternate strategies for distribution diversity: random explores the
      // legal-move space (correctness), handwritten/cordite produce realistic
      // game shapes (what the size weights are tuned for)
      const strategy = (
        g % 3 === 0 ? "random" : g % 3 === 1 ? "handwritten" : "cordite"
      ) as StrategyKey;
      const game = await playRandomGame(np, strategy);
      if (!game) {
        stalled++;
        continue;
      }
      totalGames++;
      try {
        const input: ReplayInput = {
          playerIds: game.players.map((p) => p.player_id),
          logs: game.logs,
          flipped: game.flipped,
        };
        const enc = encodeReplay(input);
        sizes.push(enc.byteLength);
        (byStrategy[strategy] ??= []).push(enc.byteLength);

        // decode through every serialization layer
        const xUrl = urlToGame(enc.url);
        const xB64 = bytesToBigint(base64Decode(enc.base64));
        if (xUrl !== enc.x || xB64 !== enc.x)
          throw new Error("serialization round-trip mismatch");

        const dec = decodeReplay(enc.x);
        const diff = diffStreams(normOriginal(game), normDecoded(dec));
        if (diff) throw new Error(`stream mismatch at ${diff}`);

        // elimination order / fool must match too
        const elim = game.elimination_order.map((pid) =>
          game.players.findIndex((p) => p.player_id === pid),
        );
        if (JSON.stringify(elim) !== JSON.stringify(dec.eliminationOrder))
          throw new Error(
            `elimination mismatch ${JSON.stringify(elim)} vs ${JSON.stringify(dec.eliminationOrder)}`,
          );
        const fool = game.players.findIndex(
          (p) => p.player_id === game_done(game),
        );
        if (fool !== dec.fool) throw new Error("fool mismatch");
        if (game.discard_pile_length !== dec.discardPileLength)
          throw new Error("discard pile mismatch");

        // and the public verifier used by the UI must agree
        verifyRoundTrip(input);

        // the replay-screen view builder must fold the stream without any
        // desync (it throws on hand/deck underflow or card-count drift)
        const steps = buildReplaySteps(dec as any);
        if (steps.length !== dec.logs.length + 1)
          throw new Error("view steps mismatch");
        // extras (names + per-move timing) round-trip: arbitrary-Unicode
        // names exactly; gaps within the log-quantizer's ~7% relative error
        {
          const unicodeNames = [
            "ВАСЯ \u{1F0CF}",
            "\uD55C\uAD6D\uC774\uB984",
            "ÉMILIE",
            "P4",
            "X".repeat(60), // over the byte cap, must trim cleanly
            "P6",
            "P7",
            "P8",
          ].slice(0, np);
          // synthesize plausible created_at times: ms flurries to multi-day gaps
          let t = 1750000000;
          game.logs.forEach((l, idx) => {
            t += [0.05, 0.4, 2, 9, 45, 600, 90000][idx % 7];
            l.created_at = new Date(t * 1000).toISOString();
          });
          const moveTimes = moveTimesFromLogs(game.logs);
          const extras = encodeExtras(unicodeNames, moveTimes);
          const full = joinReplayCode(enc.base32, extras);
          const { moves: m2, extras: x2 } = splitReplayCode(full);
          if (m2 !== enc.base32 || x2 !== extras)
            throw new Error("extras container split mismatch");
          if (urlToGame("WWW.FOOLISH.CARDS/" + full) !== enc.x)
            throw new Error("urlToGame must ignore the extras section");

          const moveCount = dec.logs.filter((l) =>
            INFO_TYPES.includes(l.log_type),
          ).length;
          if (moveCount !== moveTimes.length - 1)
            throw new Error("extras: move count vs times mismatch");
          const back = decodeExtras(extras, np, moveCount);
          const wantNames = unicodeNames.map((nm) =>
            Array.from(nm).length > 48 ? nm.slice(0, 48) : nm,
          );
          back.names!.forEach((nm, i) => {
            const orig = unicodeNames[i];
            if (nm !== orig && !orig.startsWith(nm))
              throw new Error(`name mismatch: ${nm} vs ${orig}`);
          });
          if (Math.abs(back.startTime! - Math.floor(moveTimes[0])) > 1)
            throw new Error("start time mismatch");
          back.moveGaps!.forEach((g, i) => {
            const want = moveTimes[i + 1] - moveTimes[i];
            const err = Math.abs(g - want);
            if (err > Math.max(0.08, want * 0.08))
              throw new Error(`gap ${i}: got ${g}, want ${want}`);
          });
        }

        // the derived bout leader must be the seat that actually opens each
        // bout (only the first attacker may attack an empty table)
        for (let s = 1; s < steps.length; s++) {
          if (
            steps[s].kind === LOG_TYPE.ATTACK &&
            steps[s - 1].battles.length === 0 &&
            steps[s].seat !== steps[s - 1].firstAttacker
          ) {
            throw new Error(
              `view firstAttacker drift at step ${s}: P${steps[s].seat} opened, derived P${steps[s - 1].firstAttacker}`,
            );
          }
        }
      } catch (e: any) {
        pcFails++;
        failures++;
        if (pcFails <= 3) {
          print(`\n[np=${np} game=${g} ${strategy}] FAIL: ${e.message}`);
        }
      }
    }
    if (sizes.length > 0) {
      const avg = sizes.reduce((s, v) => s + v, 0) / sizes.length;
      const min = Math.min(...sizes);
      const max = Math.max(...sizes);
      const per = Object.entries(byStrategy)
        .map(([k, v]) => {
          const a = v.reduce((s, x) => s + x, 0) / v.length;
          return `${k}=${a.toFixed(1)}`;
        })
        .join(" ");
      print(
        `np=${np}  games=${sizes.length}  fails=${pcFails}  bytes avg=${avg.toFixed(1)} min=${min} max=${max}  [${per}]`,
      );
    } else {
      print(`np=${np}  no completed games (stalled=${stalled})`);
    }
  }

  print(
    `\nTOTAL: ${totalGames} games, ${failures} failures, ${stalled} stalled-skipped`,
  );
  if (failures > 0) {
    print("RESULT: FAIL");
    process.exit(1);
  }
  print("RESULT: ALL CLEAN");
})();
