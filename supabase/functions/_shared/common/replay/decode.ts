/* =============================================================================
 * Replay format v1 — DECODE side (shared: client replay screen + server verify)
 * =============================================================================
 * Reconstructs the full game event stream from the replay integer alone. No
 * auth, no database: the URL/base64 string IS the game.
 *
 * The rules projection runs in the C kernel (cnitro/src/replay.c) — the
 * same codebase as the production game rules — via one wasm call; this file
 * only moves bytes and rebuilds the TS shapes. Async because the browser
 * must compile the wasm module asynchronously (the engine bridge and its
 * embedded module are also dynamic-imported here so the replay screen chunk,
 * not every page, carries them).
 * ========================================================================== */

import { bigintToBytes } from "./codec.ts";
import { DecodedReplay, SeatLog, idToCard } from "./core.ts";
import { LogCardPair } from "../../core/types.ts";

// Byte layout: REPLAY_DEC_HDR + per-log records — see cnitro/src/replay.h.
const DEC_HDR = 20;
const CARD_NONE = 0xff;

export async function decodeReplay(x: bigint): Promise<DecodedReplay> {
  const eng = await import("../../sdk/ts/wasm/engine.ts");
  await eng.ensureEngineAsync();
  const out = eng.kernelReplayDecode(bigintToBytes(x));

  const n = out[1];
  const trumpId = out[2];
  const eliminationOrder: number[] = [];
  for (let i = 0; i < out[7]; i++) eliminationOrder.push(out[8 + i]);
  const nLogs = out[16] | (out[17] << 8) | (out[18] << 16) | (out[19] << 24);

  const logs: SeatLog[] = new Array(nLogs);
  let q = DEC_HDR;
  for (let i = 0; i < nLogs; i++) {
    const log_type = eng.__LOG_TYPE_FROM_INT[out[q]];
    const seat = out[q + 1] === 0xff ? null : out[q + 1];
    const defender_index = out[q + 2] === 0xff ? null : out[q + 2];
    const nPairs = out[q + 3];
    q += 4;
    const card_pairs: LogCardPair[] = new Array(nPairs);
    for (let j = 0; j < nPairs; j++) {
      const target = out[q + 1];
      card_pairs[j] = {
        primary: eng.__cardFromWire(out[q]),
        target: target === CARD_NONE ? null : eng.__cardFromWire(target),
      };
      q += 2;
    }
    logs[i] = { log_type, seat, card_pairs, defender_index };
  }

  return {
    formatVersion: out[0],
    playerCount: n,
    trumpCard: idToCard(trumpId),
    powerSuit: Math.floor(trumpId / 13),
    firstAttacker: out[3],
    logs,
    eliminationOrder,
    fool: out[4],
    discardPileLength: out[5] | (out[6] << 8),
  };
}
