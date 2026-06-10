/* =============================================================================
 * Replay format v1 — ENCODE side (server-only; not copied to the client)
 * =============================================================================
 * Turns a finished game's log stream into the replay integer. Runs in the
 * edge functions at game end (utils.ts check_win_async): the resulting base64
 * string is appended to games.snapshots and the game's game_logs rows are
 * wiped — the snapshot replaces them.
 *
 * verifyRoundTrip() is the gate: it decodes the freshly encoded integer and
 * checks the result reproduces every information-bearing log. Only persist a
 * snapshot it returns; never wipe logs for a game it threw on.
 * ========================================================================== */

import { Card, LOG_TYPE } from "../types.ts";
import { ACE_VALUE } from "../constants.ts";
import {
  Coder,
  bigintToBytes,
  base32Encode,
  base64Encode,
  gameToUrl,
} from "./codec.ts";
import {
  FORMAT_VERSION,
  VERSION_ALPHABET,
  INFO_TYPES,
  InfoSource,
  ReplayLogEntry,
  ReplayInput,
  EncodedReplay,
  DecodedReplay,
  cardId,
  trumpAlphabet,
  runReplay,
} from "./core.ts";
import { decodeReplay } from "./decode.ts";

function makeSource(input: ReplayInput): {
  src: InfoSource;
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
  const info = logs.filter((l) => INFO_TYPES.includes(l.log_type));
  if (info.length === 0) throw new Error("no game actions to encode");

  const seatOf = (pid: string | null): number => {
    const s = input.playerIds.indexOf(pid ?? "");
    if (s < 0) throw new Error(`unknown player_id in logs: ${pid}`);
    return s;
  };

  const firstAtk = info.find((l) => l.log_type === LOG_TYPE.ATTACK);
  if (!firstAtk) throw new Error("no attack in logs");
  const firstAttacker = seatOf(firstAtk.player_id);

  let i = 0;
  const src: InfoSource = {
    peek: () => {
      if (i >= info.length) throw new Error("log source exhausted");
      return info[i];
    },
    advance: () => {
      i++;
    },
    exhausted: () => i >= info.length,
    seatOf,
  };
  return { src, firstAttacker };
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

function encodeHeader(
  coder: Coder,
  n: number,
  trumpId: number,
  firstAttacker: number,
): void {
  coder.codeUniform(VERSION_ALPHABET, FORMAT_VERSION);
  coder.codeUniform(7, n - 2);
  const alpha = trumpAlphabet(n);
  const t = alpha.indexOf(trumpId);
  if (t < 0) throw new Error("trump not in alphabet");
  coder.codeUniform(alpha.length, t);
  coder.codeUniform(n, firstAttacker);
}

export function encodeReplay(input: ReplayInput): EncodedReplay {
  const n = input.playerIds.length;
  if (n < 2 || n > 8) throw new Error(`unsupported player count ${n}`);
  const trump = deriveTrump(input);
  if (trump.value === ACE_VALUE) throw new Error("trump card cannot be an ace");
  const { src, firstAttacker } = makeSource(input);

  const coder = Coder.forEncode();
  encodeHeader(coder, n, cardId(trump), firstAttacker);
  runReplay(coder, n, cardId(trump), firstAttacker, src);

  const x = coder.finishEncode();
  const bytes = bigintToBytes(x);
  return {
    x,
    bytes,
    byteLength: bytes.length,
    base32: base32Encode(bytes),
    base64: base64Encode(bytes),
    url: gameToUrl(x),
  };
}

/** Encode, decode, and check the decoded stream reproduces every
 *  information-bearing log. Persist only what this returns — it catches any
 *  engine/model drift on the actual game at hand before data is destroyed. */
export function verifyRoundTrip(input: ReplayInput): {
  encoded: EncodedReplay;
  decoded: DecodedReplay;
} {
  const encoded = encodeReplay(input);
  const decoded = decodeReplay(encoded.x);

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
