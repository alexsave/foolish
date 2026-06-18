/* =============================================================================
 * Replay format v1 — DECODE side (shared: client replay screen + server verify)
 * =============================================================================
 * Reconstructs the full game event stream from the replay integer alone. No
 * auth, no database: the URL/base64 string IS the game.
 * ========================================================================== */

import { Coder, bytesToBigint, base64Decode, urlToGame } from "./codec.ts";
import {
  FORMAT_VERSION,
  VERSION_ALPHABET,
  DecodedReplay,
  idToCard,
  trumpAlphabet,
  runReplay,
  inCount,
} from "./core.ts";

export function decodeReplay(x: bigint): DecodedReplay {
  const coder = Coder.forDecode(x);
  const version = coder.codeUniform(VERSION_ALPHABET);
  if (version !== FORMAT_VERSION)
    throw new Error(`unsupported replay format version ${version}`);
  const n = coder.codeUniform(7) + 2;
  const alpha = trumpAlphabet(n);
  const trumpId = alpha[coder.codeUniform(alpha.length)];
  const firstAttacker = coder.codeUniform(n);

  const m = runReplay(coder, n, trumpId, firstAttacker, null);
  if (!coder.residueIsZero())
    throw new Error("invalid replay: leftover data after game end");
  if (inCount(m) !== 1) throw new Error("invalid replay: no single fool");

  let fool = -1;
  for (let s = 0; s < n; s++) if (m.status[s]) fool = s;
  return {
    formatVersion: version,
    playerCount: n,
    trumpCard: idToCard(trumpId),
    powerSuit: Math.floor(trumpId / 13),
    firstAttacker,
    logs: m.out,
    eliminationOrder: m.eliminationOrder,
    fool,
    discardPileLength: m.discard,
  };
}


