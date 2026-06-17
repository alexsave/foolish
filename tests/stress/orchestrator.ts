// Faithful replica of executeWithGameLock + the fire-and-forget broadcast path
// from supabase/functions/_shared/utils.ts, instrumented so the harness can see
// exactly what each client would receive.
//
// Two production behaviours are reproduced precisely because they're the whole
// point of the experiment:
//   1. The CAS retry loop (load version V -> compute -> commit_game gated on V ->
//      reload+redo on conflict). The commit itself is the REAL plpgsql RPC.
//   2. Broadcast is launched AFTER the durable commit and is NOT awaited
//      (`broadcastAnimationEvents(...).catch(...)`). So overlapping calls (e.g. a
//      human move + the bot loop) each fire an independent broadcast whose
//      delivery can interleave. We model per-recipient delivery latency to expose
//      any ordering/duplication glitch a real client could see.

import { Game, AnimationEvent, GAME_STATUS, PLAYER_STATUS, PrivatePlayer } from '../../supabase/functions/_shared/types.ts';
import { game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { loadCompleteGame, commitGame } from './db.ts';

// --- check_win_sync: verbatim from utils.ts --------------------------------
export const checkWinSync = (game: Game): boolean => {
  const fool = game_done(game);
  if (fool === null) return false;
  game.status = GAME_STATUS.GAME_OVER;
  game.players.forEach((p) => {
    p.status = p.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
  });
  return true;
};

// --- Broadcast recorder -----------------------------------------------------
export interface Delivery {
  recipient: string;       // player_id the sequence was delivered to
  committedVersion: number; // games.version this broadcast was emitted at
  emitSeq: number;          // global emission order (monotone by construction)
  arriveSeq: number;        // global arrival order across all clients
  numEvents: number;
  reqId: string;
}

export class Recorder {
  emitCounter = 0;
  arriveCounter = 0;
  perClient = new Map<string, Delivery[]>();
  pending: Promise<void>[] = [];
  maxLatencyMs: number;

  constructor(maxLatencyMs = 8) { this.maxLatencyMs = maxLatencyMs; }

  // Mirror broadcastAnimationEvents: snapshot a personalized payload per human
  // player NOW (synchronously, from the just-committed game), then deliver after a
  // per-recipient random latency. Bots have no client, so they're skipped.
  broadcast(game: Game, events: AnimationEvent[], committedVersion: number, reqId: string): void {
    if (events.length === 0) return;
    const emitSeq = this.emitCounter++;
    const humans = game.players.filter((p: PrivatePlayer) => !p.is_ai);
    for (const player of humans) {
      const latency = Math.random() * this.maxLatencyMs;
      const rec = this;
      const p = new Promise<void>((resolve) => {
        setTimeout(() => {
          if (!rec.perClient.has(player.player_id)) rec.perClient.set(player.player_id, []);
          rec.perClient.get(player.player_id)!.push({
            recipient: player.player_id, committedVersion, emitSeq,
            arriveSeq: rec.arriveCounter++, numEvents: events.length, reqId,
          });
          resolve();
        }, latency);
      });
      this.pending.push(p);
    }
  }

  async drain(): Promise<void> {
    while (this.pending.length) {
      const batch = this.pending; this.pending = [];
      await Promise.all(batch);
    }
  }
}

export interface LockOpts {
  computeDelayMs?: number; // injected delay between load and commit (widens race window)
  maxAttempts?: number;
}

export type Operation = (game: Game) => Promise<{ game: Game; events: AnimationEvent[] }>;

export interface LockResult {
  game: Game; events: AnimationEvent[]; attempts: number; committedVersion: number;
}

// Faithful replica of executeWithGameLock.
export const executeWithGameLock = async (
  gameId: string, operation: Operation, recorder: Recorder,
  reqId = 'unknown', mootIfGameOver = false, opts: LockOpts = {},
): Promise<LockResult> => {
  const MAX_ATTEMPTS = opts.maxAttempts ?? 5;

  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    const loadedGame = await loadCompleteGame(gameId);
    const expectedVersion = loadedGame.version ?? 0;

    if (mootIfGameOver && loadedGame.status === GAME_STATUS.GAME_OVER) {
      return { game: loadedGame, events: [], attempts: attempt, committedVersion: expectedVersion };
    }

    const result = await operation(loadedGame);

    // Simulate the up-to-2s cordite compute window on free-tier: hold the
    // computed-but-uncommitted state so a concurrent actor can slip in and bump
    // the version under us, forcing the CAS conflict path.
    if (opts.computeDelayMs) await new Promise((r) => setTimeout(r, opts.computeDelayMs));

    checkWinSync(result.game);
    const commit = await commitGame(result.game, expectedVersion);

    if (commit.status === 'conflict') {
      if (attempt < MAX_ATTEMPTS) continue;
      throw new Error(`Could not commit game ${gameId} after ${MAX_ATTEMPTS} attempts — write contention`);
    }

    const committedVersion = commit.version!;
    // Broadcast AFTER the durable commit, fire-and-forget (NOT awaited) — exactly
    // like production.
    recorder.broadcast(result.game, result.events, committedVersion, reqId);

    return { game: result.game, events: result.events, attempts: attempt, committedVersion };
  }
  throw new Error(`Could not commit game ${gameId}`);
};
