/* =============================================================================
 * Replay format v1 — ENCODE side (server-only; not copied to the client)
 * =============================================================================
 * Turns a finished game's log stream into the replay integer. Runs in the
 * edge functions at game end (utils.ts finalizeEndedGame): the resulting
 * base64 string is stored in game_snapshots and the game's packed session log
 * (games.logs_packed) is cleared — the snapshot replaces it.
 *
 * The rules projection runs in the C kernel (c/src/replay.c); this file
 * keeps the log-stream plumbing that is genuinely TS-shaped: slicing the
 * last session, mapping player_ids to seats, deriving the trump from the
 * logs, and synthesizing the round_end markers (a DISCARD directly preceded
 * by a GOOD is a good-transition; a clean-sweep cover's DISCARD is derived
 * by the decoder's cover cascade and carries no information).
 *
 * verifyRoundTrip() is the gate: it decodes the freshly encoded integer and
 * checks the result reproduces every information-bearing log. Only persist a
 * snapshot it returns; never wipe logs for a game it threw on.
 *
 * SCOPE NOTE (A4, docs/C_CORE_CONSOLIDATION.md F5): the "TS-shaped plumbing"
 * above is now the V5 story only. v6 in production is verifyRoundTripV6FromGame
 * — one kernel call that re-derives the deal from the seed and reads the actions
 * out of the session log itself, so none of the collect/marshal/derive-trump
 * code below runs for it. encodeReplayV6 / collectV6 / marshalInputV6 have no
 * production caller left; they are the frozen ORACLE the kernel is byte-compared
 * against (e2e/replay_v6_parity.test.ts). Keep them working, don't extend them.
 * ========================================================================== */

import { Card, Game, GameLog, LOG_TYPE } from "@api/core/types.ts";
import { ACE_VALUE } from "@api/core/constants.ts";
import {
  bytesToBigint,
  base32Encode,
  base64Encode,
  gameToUrl,
} from "./codec.ts";
import {
  INFO_TYPES,
  ReplayInput,
  ReplayLogEntry,
  EncodedReplay,
  DecodedReplay,
  cardId,
} from "./core.ts";
import { decodeReplay } from "./decode.ts";

// Encode-input byte format — see c/src/replay.h.
const ROUND_END = 0xff;
const CARD_NONE = 0xff;
const CARD_HIDDEN = 0xfe;

function wireOf(c: Card): number {
  if (c.suit < 0 || c.value < 1) return CARD_HIDDEN;
  return cardId(c);
}

type Action =
  | { kind: "log"; log: ReplayLogEntry; seat: number }
  | { kind: "round_end" };

function collectActions(input: ReplayInput): {
  actions: Action[];
  firstAttacker: number;
} {
  // keep only the last session
  let logs = input.logs;
  for (let i = logs.length - 1; i >= 0; i--) {
    if (logs[i].log_type === LOG_TYPE.GAME_START) {
      logs = logs.slice(i);
      break;
    }
  }

  const seatOf = (pid: string | null): number => {
    const s = input.playerIds.indexOf(pid ?? "");
    if (s < 0) throw new Error(`unknown player_id in logs: ${pid}`);
    return s;
  };

  const actions: Action[] = [];
  for (let i = 0; i < logs.length; i++) {
    const l = logs[i];
    if (INFO_TYPES.includes(l.log_type)) {
      actions.push({ kind: "log", log: l, seat: seatOf(l.player_id) });
    } else if (
      l.log_type === LOG_TYPE.DISCARD &&
      i > 0 &&
      logs[i - 1].log_type === LOG_TYPE.GOOD
    ) {
      actions.push({ kind: "round_end" });
    }
  }
  if (actions.length === 0) throw new Error("no game actions to encode");

  const firstAtk = actions.find(
    (a) => a.kind === "log" && a.log.log_type === LOG_TYPE.ATTACK,
  );
  if (!firstAtk || firstAtk.kind !== "log")
    throw new Error("no attack in logs");
  return { actions, firstAttacker: firstAtk.seat };
}


function marshalInput(
  n: number,
  trumpId: number,
  firstAttacker: number,
  actions: Action[],
  logInt: Map<string, number>,
): Uint8Array {
  // Explicit rejects instead of silent u16/u8 wrap-around: the kernel would
  // reject the garbled stream anyway, but the failure must be attributable.
  if (actions.length > 0xffff)
    throw new Error(`replay: too many actions to encode (${actions.length})`);
  for (const a of actions) {
    // REPLAY_MAX_PAIRS in c/src/replay.h: a real log's pairs are all
    // distinct cards, so 52 covers every stream the engine can produce.
    if (a.kind === "log" && a.log.card_pairs.length > 52)
      throw new Error(`replay: log with ${a.log.card_pairs.length} card pairs`);
  }
  let size = 5;
  for (const a of actions)
    size += 3 + (a.kind === "log" ? 2 * a.log.card_pairs.length : 0);
  const buf = new Uint8Array(size);
  buf[0] = n;
  buf[1] = trumpId;
  buf[2] = firstAttacker;
  buf[3] = actions.length & 0xff;
  buf[4] = (actions.length >> 8) & 0xff;
  let q = 5;
  for (const a of actions) {
    if (a.kind === "round_end") {
      buf[q++] = ROUND_END;
      buf[q++] = 0xff;
      buf[q++] = 0;
      continue;
    }
    buf[q++] = logInt.get(a.log.log_type)!;
    buf[q++] = a.seat;
    buf[q++] = a.log.card_pairs.length;
    for (const p of a.log.card_pairs) {
      buf[q++] = wireOf(p.primary);
      buf[q++] = p.target ? wireOf(p.target) : CARD_NONE;
    }
  }
  return buf;
}

function deriveTrump(input: ReplayInput): Card {
  if (input.flipped) return input.flipped;
  // the trump card surfaces in exactly one DRAW log (the final stock card)
  for (const l of input.logs) {
    if (l.log_type !== LOG_TYPE.DRAW) continue;
    for (const p of l.card_pairs) {
      if (p.primary.suit >= 0) return p.primary;
    }
  }
  throw new Error("cannot determine the trump card from game state or logs");
}

export async function encodeReplay(input: ReplayInput): Promise<EncodedReplay> {
  const n = input.playerIds.length;
  if (n < 2 || n > 8) throw new Error(`unsupported player count ${n}`);
  const trump = deriveTrump(input);
  if (trump.value === ACE_VALUE) throw new Error("trump card cannot be an ace");
  const { actions, firstAttacker } = collectActions(input);

  const eng = await import("@sdk/ts/wasm/engine.ts");
  await eng.ensureEngineAsync();

  const bytes = eng.kernelReplayEncode(
    marshalInput(n, cardId(trump), firstAttacker, actions, eng.__LOG_TYPE_TO_INT),
  );
  const x = bytesToBigint(bytes);
  return {
    x,
    bytes,
    byteLength: bytes.length,
    base32: base32Encode(bytes),
    base64: base64Encode(bytes),
    url: gameToUrl(x),
  };
}





// Shared info-action comparison for both verify paths.
function checkInfoActionsMatch(input: ReplayInput, decoded: DecodedReplay): void {
  let logs = input.logs;
  for (let i = logs.length - 1; i >= 0; i--) {
    if (logs[i].log_type === LOG_TYPE.GAME_START) { logs = logs.slice(i); break; }
  }
  const origInfo = logs.filter((l) => INFO_TYPES.includes(l.log_type));
  const decInfo = decoded.logs.filter((l) => INFO_TYPES.includes(l.log_type));
  if (origInfo.length !== decInfo.length)
    throw new Error(`verify failed: ${origInfo.length} actions in, ${decInfo.length} out`);
  for (let i = 0; i < origInfo.length; i++) {
    const a = origInfo[i];
    const b = decInfo[i];
    const seat = input.playerIds.indexOf(a.player_id ?? "");
    if (
      a.log_type !== b.log_type || seat !== b.seat ||
      a.card_pairs.length !== b.card_pairs.length ||
      !a.card_pairs.every((p, j) =>
        p.primary.suit === b.card_pairs[j].primary.suit &&
        p.primary.value === b.card_pairs[j].primary.value &&
        (p.target?.suit ?? null) === (b.card_pairs[j].target?.suit ?? null) &&
        (p.target?.value ?? null) === (b.card_pairs[j].target?.value ?? null))
    ) {
      throw new Error(`verify failed at action ${i} (${a.log_type})`);
    }
  }
}

/** v6 the way production makes it (docs/C_CORE_CONSOLIDATION.md F5/A4): ONE
 *  kernel call. The kernel re-derives the true deal from the game's deal seed
 *  and reads the actions out of the session log it is handed, so this side never
 *  assembles a reveal stream, never marshals an action, and never has to know
 *  that v6 has a header — all of which used to live here (collectV6 /
 *  marshalInputV6 / deriveTrump) and in game_lifecycle's reconstructSeededDeal.
 *
 *  The byte-equality that made this a PORT and not a rewrite is now asserted
 *  where the codec lives: c/tests/replay_v6_test.c holds
 *  replay_encode_v6_from_game against the marshalled producer on real engine
 *  games. The TS oracle it used to be compared against (verifyRoundTripV6 +
 *  reconstructSeededDeal) is deleted — a second implementation kept alive to
 *  agree with the first can only ever say "the copy agrees" (A9).
 *
 *  The round-trip gate stays exactly as it was: encode, decode, confirm every
 *  info action survived. It is what stands between an encoder bug and logs that
 *  have already been retired — so it is checked against `logs`, the session as
 *  the SERVER decoded it, not against anything the kernel handed back.
 *
 *  @param game       the finished game (roster in seat order + final state)
 *  @param seed       its 32-byte deal seed (games.game_seed)
 *  @param logs       the decoded session log — the verification reference
 *  @param packedLogs the same session as packed bytes (games.logs_packed), fed
 *                    to the kernel with no JS objects in between; omit and the
 *                    kernel is handed game.logs instead. */
export async function verifyRoundTripV6FromGame(
  game: Game,
  seed: Uint8Array,
  logs: GameLog[],
  packedLogs?: Uint8Array,
): Promise<{ encoded: EncodedReplay; decoded: DecodedReplay }> {
  const { kernelReplayEncodeV6FromGame } = await import("@sdk/ts/wasm/bots.ts");
  const bytes = kernelReplayEncodeV6FromGame(game, seed, packedLogs);
  const x = bytesToBigint(bytes);
  const encoded: EncodedReplay = {
    x, bytes, byteLength: bytes.length,
    base32: base32Encode(bytes),
    base64: base64Encode(bytes),
    url: gameToUrl(x),
  };
  const decoded = await decodeReplay(x);
  checkInfoActionsMatch(
    { playerIds: game.players.map((p) => p.player_id), logs, flipped: game.flipped },
    decoded,
  );
  return { encoded, decoded };
}

/** Encode, decode, and check the decoded stream reproduces every
 *  information-bearing log. Persist only what this returns — it catches any
 *  engine/model drift on the actual game at hand before data is destroyed. */
export async function verifyRoundTrip(input: ReplayInput): Promise<{
  encoded: EncodedReplay;
  decoded: DecodedReplay;
}> {
  const encoded = await encodeReplay(input);
  const decoded = await decodeReplay(encoded.x);

  let logs = input.logs;
  for (let i = logs.length - 1; i >= 0; i--) {
    if (logs[i].log_type === LOG_TYPE.GAME_START) {
      logs = logs.slice(i);
      break;
    }
  }
  const origInfo = logs.filter((l) => INFO_TYPES.includes(l.log_type));
  const decInfo = decoded.logs.filter((l) => INFO_TYPES.includes(l.log_type));
  if (origInfo.length !== decInfo.length)
    throw new Error(
      `verify failed: ${origInfo.length} actions in, ${decInfo.length} out`,
    );
  for (let i = 0; i < origInfo.length; i++) {
    const a = origInfo[i];
    const b = decInfo[i];
    const seat = input.playerIds.indexOf(a.player_id ?? "");
    if (
      a.log_type !== b.log_type ||
      seat !== b.seat ||
      a.card_pairs.length !== b.card_pairs.length ||
      !a.card_pairs.every(
        (p, j) =>
          p.primary.suit === b.card_pairs[j].primary.suit &&
          p.primary.value === b.card_pairs[j].primary.value &&
          (p.target?.suit ?? null) === (b.card_pairs[j].target?.suit ?? null) &&
          (p.target?.value ?? null) === (b.card_pairs[j].target?.value ?? null),
      )
    ) {
      throw new Error(`verify failed at action ${i} (${a.log_type})`);
    }
  }
  return { encoded, decoded };
}
