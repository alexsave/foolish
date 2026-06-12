/* =============================================================================
 * Tutorial game generator
 * =============================================================================
 * Plays full 3-player games with the REAL server engine, encodes each to a
 * replay code, and scores it for teaching value: seat 0 ("You") should perform
 * a wide variety of moves, and the whole game must contain every gameplay
 * element we want to teach (lead attack, cover, trump cover, throw-in,
 * perevod/pass, pickup, round-end discard, refills, the deck running out, a
 * player going out, and the fool). Prints the best candidate's moves code so it
 * can be embedded in the client tutorial (src/components/Tutorial.tsx).
 *
 *   npx tsx tests/gen_tutorial_game.ts [attempts]
 * ========================================================================== */

import {
  start_game,
  game_done,
} from "../supabase/functions/_shared/common_utils.ts";
import {
  Game,
  GAME_STATUS,
  PLAYER_STATUS,
  PrivatePlayer,
  StrategyKey,
} from "../supabase/functions/_shared/types.ts";
import {
  shouldBotActCore,
  processBotAction,
} from "../supabase/functions/_shared/pure_bot_actions.ts";
import { calculateLegalMoves } from "../supabase/functions/_shared/bot_strategy.ts";
import { ReplayInput } from "../supabase/functions/_shared/replay/core.ts";
import { encodeReplay } from "../supabase/functions/_shared/replay/encode.ts";
import { decodeReplay } from "../supabase/functions/_shared/replay/decode.ts";
import { buildReplaySteps, ReplayStep } from "../src/replay/view";
import { LOG_TYPE } from "../src/common/types";
import { writeFileSync } from "node:fs";

const saved = console.log.bind(console);
console.log = () => {};
console.warn = () => {};
console.error = () => {};
console.info = () => {};
const print = saved;

const ATTEMPTS = Number(process.argv[2] ?? 4000);
const NP = 3;
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

const mkGame = (strategy: StrategyKey): Game => ({
  players: Array.from({ length: NP }, (_, i) => mkPlayer(i, strategy)),
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

async function playGame(strategy: StrategyKey): Promise<Game | null> {
  const game = mkGame(strategy);
  start_game(game);
  let actions = 0;
  while (game_done(game) === null) {
    if (++actions > MAX_ACTIONS) return null;
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

interface Concepts {
  lead: number[]; // step indices, by any seat
  cover: number[];
  trumpCover: number[];
  throwIn: number[];
  pass: number[];
  pickup: number[];
  discard: number[];
  draw: number[];
  deckEmpty: number[];
  out: number[];
  end: number[];
  // which concepts seat 0 personally performs
  seat0: Set<string>;
}

function analyze(steps: ReplayStep[], powerSuit: number): Concepts {
  const c: Concepts = {
    lead: [], cover: [], trumpCover: [], throwIn: [], pass: [], pickup: [],
    discard: [], draw: [], deckEmpty: [], out: [], end: [], seat0: new Set(),
  };
  for (let i = 0; i < steps.length; i++) {
    const s = steps[i];
    const prev = steps[i - 1];
    const is0 = s.seat === 0;
    switch (s.kind) {
      case LOG_TYPE.ATTACK:
        if (prev && prev.battles.length > 0) {
          c.throwIn.push(i);
          if (is0) c.seat0.add("throwIn");
        } else {
          c.lead.push(i);
          if (is0) c.seat0.add("lead");
        }
        break;
      case LOG_TYPE.COVER: {
        c.cover.push(i);
        if (is0) c.seat0.add("cover");
        const cov = s.cards[0];
        const tgt = s.target;
        if (cov && tgt && cov.suit === powerSuit && tgt.suit !== powerSuit) {
          c.trumpCover.push(i);
          if (is0) c.seat0.add("trumpCover");
        }
        break;
      }
      case LOG_TYPE.PASS:
        c.pass.push(i);
        if (is0) c.seat0.add("pass");
        break;
      case LOG_TYPE.PICKUP:
        c.pickup.push(i);
        if (is0) c.seat0.add("pickup");
        break;
      case LOG_TYPE.DISCARD:
        c.discard.push(i);
        break;
      case LOG_TYPE.DRAW:
        c.draw.push(i);
        break;
      case LOG_TYPE.PLAYER_OUT:
        c.out.push(i);
        break;
      case "end":
        c.end.push(i);
        break;
    }
    if (s.deckCount === 0 && s.flipped === null) c.deckEmpty.push(i);
  }
  return c;
}

const REQUIRED: (keyof Concepts)[] = [
  "lead", "cover", "trumpCover", "throwIn", "pass", "pickup",
  "discard", "draw", "deckEmpty", "out", "end",
];

const SEAT0_WANTED = ["lead", "cover", "throwIn", "pass", "pickup"];

(async () => {
  let best: {
    score: number;
    code: string;
    steps: number;
    firstAttacker: number;
    fool: number;
    concepts: Concepts;
    powerSuit: number;
  } | null = null;

  const strategies: StrategyKey[] = [
    "handwritten",
    "simple_heuristic",
  ] as StrategyKey[];

  let tried = 0;
  for (let a = 0; a < ATTEMPTS; a++) {
    const strategy = strategies[a % strategies.length];
    const game = await playGame(strategy);
    if (!game) continue;
    tried++;
    let enc, dec, steps: ReplayStep[];
    try {
      const input: ReplayInput = {
        playerIds: game.players.map((p) => p.player_id),
        logs: game.logs,
        flipped: game.flipped,
      };
      enc = encodeReplay(input);
      dec = decodeReplay(enc.x);
      steps = buildReplaySteps(dec as any);
    } catch {
      continue;
    }

    const concepts = analyze(steps, dec.powerSuit);
    // hard requirement: every teachable concept must appear at least once
    const hasAll = REQUIRED.every((k) => (concepts[k] as number[]).length > 0);
    if (!hasAll) continue;

    // length window: long enough to be a real game, short enough to sit through
    const total = steps.length;
    if (total < 38 || total > 78) continue;

    // scoring: favor seat-0 variety, first-attacker == 0 (you lead), and a
    // tidy length near ~55 steps.
    let score = 0;
    for (const w of SEAT0_WANTED) if (concepts.seat0.has(w)) score += 10;
    if (concepts.seat0.has("trumpCover")) score += 6;
    if (dec.firstAttacker === 0) score += 8;
    // a pass performed BY seat 0 is the rarest/most-valuable demo
    if (concepts.seat0.has("pass")) score += 6;
    // seat 0 should not be the fool (more encouraging to win/place)
    if (dec.fool !== 0) score += 4;
    score -= Math.abs(total - 55) * 0.2;

    if (!best || score > best.score) {
      best = {
        score, code: enc.base32, steps: total,
        firstAttacker: dec.firstAttacker, fool: dec.fool, concepts,
        powerSuit: dec.powerSuit,
      };
      // persist as we go so partial progress survives a timeout
      writeFileSync(
        "/tmp/tutorial_best.json",
        JSON.stringify(
          {
            score, code: enc.base32, steps: total,
            firstAttacker: dec.firstAttacker, fool: dec.fool,
            powerSuit: dec.powerSuit, seat0: [...concepts.seat0],
          },
          null, 2,
        ),
      );
      print(`[new best a=${a}] score=${score.toFixed(1)} strat=${strategy} steps=${total} fa=${dec.firstAttacker} fool=${dec.fool} seat0={${[...concepts.seat0].join(",")}}`);
    }
  }

  if (!best) {
    print("no suitable game found");
    return;
  }

  const c = best.concepts;
  const first = (xs: number[]) => (xs.length ? xs[0] : -1);
  print("\n================ BEST TUTORIAL GAME ================");
  print(`games played: ${tried}`);
  print(`score        : ${best.score.toFixed(1)}`);
  print(`steps        : ${best.steps}`);
  print(`power_suit   : ${best.powerSuit}`);
  print(`firstAttacker: ${best.firstAttacker}`);
  print(`fool seat    : ${best.fool}`);
  print(`seat0 does   : ${[...c.seat0].join(", ")}`);
  print("first-occurrence step indices:");
  print(`  lead      : ${first(c.lead)}`);
  print(`  cover     : ${first(c.cover)}`);
  print(`  trumpCover: ${first(c.trumpCover)}`);
  print(`  throwIn   : ${first(c.throwIn)}`);
  print(`  pass      : ${first(c.pass)}`);
  print(`  pickup    : ${first(c.pickup)}`);
  print(`  discard   : ${first(c.discard)}`);
  print(`  draw      : ${first(c.draw)}`);
  print(`  deckEmpty : ${first(c.deckEmpty)}`);
  print(`  out       : ${first(c.out)}`);
  print(`  end       : ${first(c.end)}`);
  print("\nMOVES CODE (base32):");
  print(best.code);
  print("===================================================");
})();
