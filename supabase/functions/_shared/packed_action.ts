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
import { GAME_STATUS } from './types.ts';
import { verify_player_in_game } from './common_utils.ts';
import { ACTION_STATUS, AwireMove, decodeAction } from './wire/awire.ts';

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
    state: string | null;
    players: { player_id: string; name: string; is_ai: boolean; status: string }[];
    good_players: string[] | null;
    good_timestamp: number | null;
}

const MAX_ATTEMPTS = 5;

export async function executePackedAction(
    gameId: string, userId: string, wire: Uint8Array, reqId: string = 'packed',
): Promise<PackedActionOutcome> {
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        const { data, error } = await supabaseClient
            .from('games').select('*').eq('id', gameId).single();
        if (error || !data) throw new Error(`Game ${gameId} not found`);
        const row = data as GamesRow;
        const expectedVersion = row.version ?? 0;

        // End-game race: same moot rule as executeWithGameLock — a move that
        // lost the race to a game-ending commit is a no-op, not an error.
        if (row.status === GAME_STATUS.GAME_OVER) {
            console.log(`[${reqId}][PACKED] game ${gameId} already over — move is a no-op`);
            return { status: ACTION_STATUS.MOOT, rejectCode: 0, version: expectedVersion, gameStatus: row.status };
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
        const { hexToBytes, bytesToHex } = await import('./replay/codec.ts');
        const { runPackedAction, materializeKernelGame } = await import('./wasm/engine.ts');
        const { logsFromKernelExport } = await import('./wire/logwire.ts');
        const { bytesToBareHex } = await import('./wire/bytes.ts');
        const run = runPackedAction(hexToBytes(row.state), seat, wire, aiMask, humanSeats);

        if (!run.ok) {
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
    const { handleAttack } = await import('./actions/attack.ts');
    const { handleCover } = await import('./actions/cover.ts');
    const { handlePass } = await import('./actions/pass.ts');
    const { handlePickup } = await import('./actions/pickup.ts');
    const { handleGood } = await import('./actions/good.ts');
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
