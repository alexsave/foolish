// The packed action pipeline (docs/PACKED_WIRE_CUTOVER.md): a human move as
// bytes end to end. The TS layer here does only what C cannot: HTTP auth is
// upstream (wrap400), this module maps the caller to a seat via the roster
// column, runs the CAS commit loop, keeps the JSONB public dual + log rows in
// sync, and fires the broadcast. The game itself — validation, application,
// win finalization, per-viewer masking, event synthesis — happens inside ONE
// synchronous kernel section (engine.ts runPackedAction). No TS Game object
// exists on this path except the single cold materialization for the DB dual.
import {
    broadcastPackedEventBuffers, commitGame, executeWithGameLock,
    finalizeEndedGame, supabaseClient,
} from './utils.ts';
import { GAME_STATUS } from '../core/types.ts';
import { verify_player_in_game } from '../common/common_utils.ts';
import { ACTION_STATUS, AwireMove, decodeAction, REJECT_STALE_ROUND } from '../../../../sdk/ts/wire/awire.ts';
import { getCachedGame, invalidateCachedGame } from './game_cache.ts';

export interface PackedActionOutcome {
    status: number;       // ACTION_STATUS.*
    rejectCode: number;   // ENGINE_REJECT_* (0 unless REJECTED)
    version: number;      // committed (or current) games.version
    gameStatus: string;   // the caller's run_bots gate
}

interface GamesRow {
    id: string;
    name: string;
    status: string;
    version: number | null;
    round_epoch: number | null;
    state: string | null;
    players: { player_id: string; name: string; is_ai: boolean; status: string }[];
    good_players: string[] | null;
    good_timestamp: number | null;
}

const MAX_ATTEMPTS = 5;

export async function executePackedAction(
    gameId: string, userId: string, wire: Uint8Array, reqId: string = 'packed',
    intentVersion?: number,
): Promise<PackedActionOutcome> {
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        // The load: this isolate usually committed this game's previous state
        // (the last human move, or the bot loop it scheduled), so the
        // CAS-fenced cache skips the round-trip on the hot path — a stale
        // entry surfaces as a commit conflict and the retry reloads fresh.
        // The fresh load selects ONLY what this path reads; games.logs_packed
        // in particular grows all session and must never ride along.
        let row: GamesRow;
        const cached = attempt === 1 ? getCachedGame(gameId) : undefined;
        if (cached) {
            row = {
                id: gameId, name: cached.name, status: cached.status,
                version: cached.version, round_epoch: cached.roundEpoch, state: cached.stateHex,
                players: cached.players, good_players: cached.good_players,
                good_timestamp: cached.good_timestamp,
            };
        } else {
            const { data, error } = await supabaseClient
                .from('games')
                .select('id, name, status, version, round_epoch, state, players, good_players, good_timestamp')
                .eq('id', gameId).single();
            if (error || !data) throw new Error(`Game ${gameId} not found`);
            row = data as GamesRow;
        }
        const expectedVersion = row.version ?? 0;

        // End-game race: same moot rule as executeWithGameLock — a move that
        // lost the race to a game-ending commit is a no-op, not an error.
        // Checked BEFORE the round guard: a move against a finished game is a
        // clean MOOT (the client resolves it as a no-op success), not a
        // stale-round reject that would pop a toast at the win screen.
        if (row.status === GAME_STATUS.GAME_OVER) {
            console.log(`[${reqId}][PACKED] game ${gameId} already over — move is a no-op`);
            return { status: ACTION_STATUS.MOOT, rejectCode: 0, version: expectedVersion, gameStatus: row.status };
        }

        // Round-boundary guard (docs/WEB_RACE_BUG_HANDOFF.md). The client stamps
        // its move with intentVersion — the games.version it composed the move
        // against. round_epoch is the version the CURRENT round began at, bumped
        // whenever a pickup/discard closes a round. intentVersion < round_epoch
        // means a round closed AFTER the client composed this move: the move was
        // aimed at a battle that no longer exists, and letting the kernel
        // re-validate it against the fresh round is exactly the "revert, then it
        // plays anyway" ghost. Reject it as stale intent instead. This is
        // round-scoped, not version-scoped: same-round throw-ins (cross-version
        // but same round_epoch) still validate purely by kernel legality.
        //   - Old clients (v1 envelope) send no intentVersion => not guarded.
        //   - A reject is only authoritative against FRESH state: a stale cache
        //     entry always pairs a version with ITS epoch (both written by one
        //     commit), so it can lag but never mislead — if it trips here on the
        //     cached hot path, drop it and re-check against the DB before
        //     rejecting. The inverse (cache too old to trip) is caught by the CAS
        //     version fence on commit, which reloads fresh and re-runs this.
        const roundEpoch = row.round_epoch ?? 0;
        if (intentVersion !== undefined && intentVersion < roundEpoch) {
            if (cached) { invalidateCachedGame(gameId); continue; }
            console.log(`[${reqId}][PACKED] stale-round reject: intent v${intentVersion} < round_epoch v${roundEpoch} (game ${gameId})`);
            return { status: ACTION_STATUS.REJECTED, rejectCode: REJECT_STALE_ROUND, version: expectedVersion, gameStatus: row.status };
        }

        // Legacy row without a blob (committed before games.state existed):
        // decode the wire into a JS move and run the JSON pipeline once —
        // its commit produces the blob and the game joins the packed path.
        if (!row.state) return await legacyFallback(gameId, userId, wire, reqId);

        // The caller's auth identity IS the player id; the seat index is the
        // kernel's name for them. Keep the legacy error priority: playing-
        // state guard outranks membership.
        const seat = row.players.findIndex(p => p.player_id === userId);
        if (seat < 0) {
            if (row.status !== GAME_STATUS.PLAYING) throw new Error(`Game ${gameId} is not in playing state`);
            throw new Error(`Player ${userId} not in game ${gameId}`);
        }

        let aiMask = 0;
        const humanSeats: number[] = [];
        row.players.forEach((p, i) => { if (p.is_ai) aiMask |= 1 << i; else humanSeats.push(i); });

        // ONE synchronous kernel section: load blob -> apply wire -> finalize
        // win -> serialize state + logs + every recipient's masked event
        // stream. Lazy import keeps the wasm embed off lobby-only cold starts.
        const { hexToBytes, bytesToHex } = await import('../common/replay/codec.ts');
        const { runPackedAction, materializeKernelGame } = await import('../../../../sdk/ts/wasm/engine.ts');
        const { logsFromKernelExport } = await import('../../../../sdk/ts/wire/logwire.ts');
        const { bytesToBareHex } = await import('../../../../sdk/ts/wire/bytes.ts');
        const run = runPackedAction(hexToBytes(row.state), seat, wire, aiMask, humanSeats);

        if (!run.ok) {
            // A rejection is only authoritative against FRESH state: an apply
            // from a stale cache self-corrects through the CAS conflict, but
            // a reject never reaches the CAS — so re-run once from the DB.
            if (cached) { invalidateCachedGame(gameId); continue; }
            return { status: ACTION_STATUS.REJECTED, rejectCode: run.reason, version: expectedVersion, gameStatus: row.status };
        }

        // The single JS materialization: the commit's JSONB public dual (the
        // roster/battles columns the heartbeat scan and lobby reads consume).
        // Bot strategy keys are only needed by the end-of-game finalize;
        // patched there, cold path.
        const game = materializeKernelGame(run.post, {
            id: row.id,
            name: row.name,
            version: expectedVersion,
            deck_length: 0,
            players: row.players.map(p => ({
                player_id: p.player_id, name: p.name, is_ai: p.is_ai,
                strategy_key: p.is_ai ? 'bot' : 'human',
            })),
            good_players: row.good_players || [],
            good_timestamp: row.good_timestamp || null,
        }, userId);

        // This move's log records, kernel-masked, straight to the packed
        // session-log column — the timestamp is the only thing TS adds.
        const logsHex = run.logsWire.length > 2
            ? bytesToBareHex(logsFromKernelExport(run.logsWire, Date.now()))
            : null;
        const commit = await commitGame(game, expectedVersion, bytesToHex(run.stateBlob), logsHex);
        if (commit.status === 'conflict') {
            // Someone else committed (another isolate, or a JS-path writer):
            // whatever we believed about this game is stale.
            invalidateCachedGame(gameId);
            if (attempt < MAX_ATTEMPTS) continue;
            throw new Error(`Could not commit game ${gameId} after ${MAX_ATTEMPTS} attempts — write contention`);
        }

        if (run.ended) {
            // ELO + replay snapshot + log wipe, exactly once (only the
            // winning commit reaches here). Real strategy keys for the
            // finalize consumers.
            const botIds = row.players.filter(p => p.is_ai).map(p => p.player_id);
            if (botIds.length > 0) {
                const { data: botRows } = await supabaseClient.from('bots').select('id, strategy_key').in('id', botIds);
                const strat = new Map<string, string>((botRows ?? []).map((b: { id: string; strategy_key: string }) => [b.id, b.strategy_key]));
                for (const p of game.players) {
                    if (p.is_ai) p.strategy_key = strat.get(p.player_id) ?? p.strategy_key;
                }
            }
            await finalizeEndedGame(game);
        }

        // Broadcast the kernel's own per-viewer streams AFTER the durable
        // commit, fire-and-forget — a plain `good` (zero events, not ended)
        // broadcasts nothing, exactly like the JSON path.
        if (run.nEvents > 0) {
            broadcastPackedEventBuffers(game, run.events, reqId).catch(err =>
                console.error(`[${reqId}] Error broadcasting packed events:`, err));
        }

        return { status: ACTION_STATUS.APPLIED, rejectCode: 0, version: game.version ?? 0, gameStatus: game.status };
    }
    throw new Error(`Could not commit game ${gameId}`);
}

// Pre-blob rows only: run the move through the legacy JSON pipeline (its
// commit writes the blob, so this fires at most once per legacy game). The
// binary response contract still holds: a handler rejection maps to a
// REJECTED envelope (code 0 = unspecified — the legacy path throws message
// strings, not codes) and the end-game race maps to MOOT, exactly like the
// kernel path.
async function legacyFallback(
    gameId: string, userId: string, wire: Uint8Array, reqId: string,
): Promise<PackedActionOutcome> {
    const move = decodeAction(wire);
    if (!move) throw new Error('malformed action wire');
    const { handleAttack } = await import('../common/actions/attack.ts');
    const { handleCover } = await import('../common/actions/cover.ts');
    const { handlePass } = await import('../common/actions/pass.ts');
    const { handlePickup } = await import('../common/actions/pickup.ts');
    const { handleGood } = await import('../common/actions/good.ts');
    let rejected = false;
    const result = await executeWithGameLock(gameId, async (game) => {
        rejected = false; // reset per CAS attempt — the op re-runs on conflict
        verify_player_in_game(game, userId);
        try {
            const events = dispatchLegacy(move, game, userId,
                { handleAttack, handleCover, handlePass, handlePickup, handleGood });
            return { game, events };
        } catch (e) {
            // A validation rejection must not abort the lock as an HTTP
            // error — surface it as the packed REJECTED status.
            console.log(`[${reqId}][PACKED] legacy-path rejection:`, (e as Error).message);
            rejected = true;
            return { game, events: [] };
        }
    }, reqId, true);
    if (rejected) {
        return {
            status: ACTION_STATUS.REJECTED, rejectCode: 0,
            version: result.game.version ?? 0, gameStatus: result.game.status,
        };
    }
    const moot = result.game.status === GAME_STATUS.GAME_OVER && result.events.length === 0;
    return {
        status: moot ? ACTION_STATUS.MOOT : ACTION_STATUS.APPLIED, rejectCode: 0,
        version: result.game.version ?? 0, gameStatus: result.game.status,
    };
}

// deno-lint-ignore no-explicit-any
function dispatchLegacy(move: AwireMove, game: any, userId: string, h: any) {
    switch (move.kind) {
        case 'attack': return h.handleAttack(game, userId, move.cards);
        case 'cover': return h.handleCover(game, userId, move.cards, move.attack_cards);
        case 'pass': return h.handlePass(game, userId, move.cards);
        case 'pickup': return h.handlePickup(game, userId);
        case 'good': return h.handleGood(game, userId);
    }
}
