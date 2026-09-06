/* =============================================================================
 * foolish.cards - whole-game replay format: shared types & wire constants
 * =============================================================================
 * The replay RULES ENGINE - the deterministic replayer that both encode and
 * decode drive - lives in the C kernel (c/src/replay.c), the same codebase as
 * the production game rules it mirrors (game.c / card.h). TS holds only
 * marshaling (encode.ts / decode.ts via sdk/ts/wasm/engine.ts) and these
 * shared types.
 *
 * e2e/replay_codec.test.ts drives the kernel over real engine-played games and
 * checks every code decodes back to the game that was played. Never change the
 * wire format in one place: bump the version in replay.h AND decide what
 * happens to every code already cut.
 * ========================================================================== */

import { Card, LOG_TYPE, LogType, LogCardPair } from "@api/core/types.ts";

// Wire-format constants. The kernel (c/src/replay.h REPLAY_FORMAT_*)
// is authoritative; these mirrors exist for TS-side pre-checks and tests.
// The ONE format: inline reveals, hidden-state-lossless, partial-game
// (c/src/replay.h REPLAY_FORMAT_VERSION_V10,
// docs/REPLAY_FORMAT6_HIDDEN_STATE.md). Was 6, then 7 (pass-mode bit), then 8
// (forced-opening bit), and is now 10 for a reason that is not a wire change at
// all: the bytes did not move, the deal order under them did. The retrodiction
// line that ran alongside it (public DRAW logs, hands recovered by complement
// once the fool was known - 5, renumbered 9) is gone entirely; a code carrying
// any other version is refused, never re-read.
export const FORMAT_VERSION_V6 = 10;
export const VERSION_ALPHABET = 16; // room for 15 future versions before a re-think

/* ------------------------------- card ids -------------------------------- */
// id = suit*13 + (value-1), ascending = (suit, value) order. Values are 1..13
// (2..A) on the 52-card deck (6+ players) and 5..13 (6..A) on the 36-card
// deck. Identical to the kernel's 1-byte wire card (c/wasm/wire.h).

export function cardId(c: Card): number {
  return c.suit * 13 + (c.value - 1);
}
export function idToCard(id: number): Card {
  return { suit: Math.floor(id / 13), value: (id % 13) + 1 };
}

/* ------------------------------- io types -------------------------------- */

/** The slice of a GameLog the codec needs (ids/timestamps are irrelevant). */
export interface ReplayLogEntry {
  log_type: LogType;
  player_id: string | null;
  card_pairs: LogCardPair[];
  defender_index: number | null;
}

export interface ReplayInput {
  /** player_ids in seat order — game.players order, NOT elimination order */
  playerIds: string[];
  /** logs of the session, ascending in time (may include older sessions —
   *  everything before the last GAME_START is ignored) */
  logs: ReplayLogEntry[];
  /** game.flipped at encode time; null if the trump card was drawn (its
   *  identity is then recovered from the DRAW log that revealed it) */
  flipped: Card | null;
}

/** A reconstructed log entry; seats replace player ids (the integer is
 *  self-contained and carries no identity data). */
export interface SeatLog {
  log_type: LogType;
  seat: number | null;
  card_pairs: LogCardPair[];
  defender_index: number | null;
}

export interface DecodedReplay {
  formatVersion: number;
  playerCount: number;
  trumpCard: Card;
  powerSuit: number;
  firstAttacker: number;
  /** full reconstructed event stream, GAME_START through the final event,
   *  including all derived DISCARD/DRAW/DEFENDER_CHANGE/PLAYER_OUT entries */
  logs: SeatLog[];
  /** seats in the order they went out; the fool is not in this list */
  eliminationOrder: number[];
  fool: number;
  discardPileLength: number;
}

export interface EncodedReplay {
  x: bigint;
  bytes: Uint8Array;
  byteLength: number;
  base32: string;
  base64: string;
  url: string;
}

// GOOD is deliberately absent: good presses are implied (v4) — they carry
// no information beyond the single throw-in-vs-round-end decision.
export const INFO_TYPES: LogType[] = [
  LOG_TYPE.ATTACK,
  LOG_TYPE.COVER,
  LOG_TYPE.PASS,
  LOG_TYPE.PICKUP,
];
