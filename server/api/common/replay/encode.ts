/* =============================================================================
 * Replay ENCODE side (server-only; not copied to the client)
 * =============================================================================
 * Turns a finished game into the replay integer. Runs in the edge functions at
 * game end (utils.ts finalizeEndedGame): the resulting base64 string is stored
 * in game_snapshots and the game's packed session log (games.logs_packed) is
 * cleared - the snapshot replaces it.
 *
 * ONE KERNEL CALL (docs/C_CORE_CONSOLIDATION.md F5/A4). Everything a producer
 * used to do on this side - slice the last session, map player_ids to seats,
 * derive the trump from the logs, synthesize the round_end markers, re-deal
 * from the seed, assemble a reveal stream, hand-marshal a header - is inside
 * replay_encode_v6_from_game, which is also exactly what the phone calls. What
 * is left here is the round-trip GATE.
 *
 * verifyRoundTripV6FromGame() is that gate: it decodes the freshly encoded
 * integer and checks the result reproduces every information-bearing log. Only
 * persist a snapshot it returns; never wipe logs for a game it threw on.
 *
 * There is no second encoder. The retrodiction format that used to live here
 * (encodeReplay / verifyRoundTrip, and the collect/marshal/derive-trump
 * plumbing they needed) hid the deal, so its replays retrodicted hands the
 * Oracle then had to guess at, and the only thing it bought was covering games
 * the seeded producer refuses. It has been removed with the format.
 * ========================================================================== */

import { Game, GameLog, LOG_TYPE } from "@api/core/types.ts";
import {
  bytesToBigint,
  base64Encode,
} from "./codec.ts";
import {
  INFO_TYPES,
  ReplayInput,
  EncodedReplay,
  DecodedReplay,
} from "./core.ts";
import { decodeReplay } from "./decode.ts";

// A lazy import that resolves ONCE. The deferral is deliberate (a cold start must
// not pull the rules-wasm embed it never uses); re-RESOLVING the specifier on
// every call was not - see the note on `lazy` in
// server/impls/supabase/functions/_shared/adapter/utils.ts.
const lazy = <T>(load: () => Promise<T>): (() => Promise<T>) => {
    let mod: Promise<T> | undefined;
    return () => (mod ??= load());
};
const botsMod = lazy(() => import("@sdk/ts/wasm/bots.ts"));


// Does the decoded stream reproduce every information-bearing log of the
// session that was played? This is what stands between an encoder bug and logs
// that have already been retired.
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
  const { kernelReplayEncodeV6FromGame, kernelB32Encode, kernelReplayLink } = await botsMod();
  const bytes = kernelReplayEncodeV6FromGame(game, seed, packedLogs);
  const x = bytesToBigint(bytes);
  const code = kernelB32Encode(bytes);
  const encoded: EncodedReplay = {
    x, bytes, byteLength: bytes.length,
    base32: code,
    base64: base64Encode(bytes),
    url: kernelReplayLink(code, []),
  };
  const decoded = await decodeReplay(x);
  checkInfoActionsMatch(
    { playerIds: game.players.map((p) => p.player_id), logs, flipped: game.flipped },
    decoded,
  );
  return { encoded, decoded };
}
