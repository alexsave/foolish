import { Game, GAME_STATUS, PLAYER_STATUS, AnimationEvent } from './types.ts';
import { executeWithGameLock } from './utils.ts';
import { executeRoundTransition } from './actions/good.ts';
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

const CHECK_INTERVAL_MS = 5000; // Check every 5 seconds
const TIMEOUT_MS = 60000; // 60 seconds timeout

const acquireAutoDiscardLock = async (game_id: string): Promise<boolean> => {
    try {
        const lockId = crypto.randomUUID();

        const { error } = await supabaseClient
            .from('auto_discard_locks')
            .insert({ game_id, lock_id: lockId });

        if (error) {
            // Lock already exists for this game
            if (error.code !== '23505') {
                return false;
            }

            // Check if existing lock is stale (older than 2 minutes)
            const { data: existingLock } = await supabaseClient
                .from('auto_discard_locks')
                .select('acquired_at')
                .eq('game_id', game_id)
                .single();

            if (!existingLock) {
                return false;
            }

            const lockAge = Date.now() - new Date(existingLock.acquired_at).getTime();
            if (lockAge <= 120000) { // 2 minutes
                return false;
            }

            // Delete stale lock and try again
            await supabaseClient
                .from('auto_discard_locks')
                .delete()
                .eq('game_id', game_id);

            const { error: retryError } = await supabaseClient
                .from('auto_discard_locks')
                .insert({ game_id, lock_id: lockId });

            if (retryError) {
                return false;
            }
        }

        // Verify we got the lock
        const { data, error: selectError } = await supabaseClient
            .from('auto_discard_locks')
            .select('lock_id')
            .eq('game_id', game_id)
            .single();

        if (selectError || !data || data.lock_id !== lockId) {
            return false;
        }

        return true;
    } catch (error) {
        return false;
    }
};

const releaseAutoDiscardLock = async (game_id: string): Promise<void> => {
    try {
        await supabaseClient
            .from('auto_discard_locks')
            .delete()
            .eq('game_id', game_id);
    } catch (error) {
        console.error(`Error releasing auto-discard lock for game ${game_id}:`, error);
    }
};

// Main auto-discard monitoring loop
export const lockedAutoDiscardLoop = async (game_id: string): Promise<void> => {
    // Try to acquire lock - if another loop is already running, exit
    if (!(await acquireAutoDiscardLock(game_id))) {
        console.log(`[AUTO-DISCARD] Loop already running for game ${game_id}, skipping`);
        return;
    }

    console.log(`[AUTO-DISCARD] Starting monitoring loop for game ${game_id}`);

    try {
        await monitorAutoDiscard(game_id);
    } finally {
        await releaseAutoDiscardLock(game_id);
        console.log(`[AUTO-DISCARD] Stopped monitoring loop for game ${game_id}`);
    }
};

// Monitor game state and trigger auto-discard when appropriate
async function monitorAutoDiscard(game_id: string, iteration: number = 0): Promise<void> {
    if (iteration > 100) {
        console.log(`[AUTO-DISCARD] Max iterations reached for game ${game_id}, stopping`);
        return;
    }

    console.log(`[AUTO-DISCARD][${iteration}] Checking game ${game_id}`);

    let shouldContinue = false;
    let shouldAutoDiscard = false;

    try {
        const { game } = await executeWithGameLock(game_id, async (game) => {
            // Check if we should still be monitoring
            if (game.status !== GAME_STATUS.PLAYING) {
                console.log(`[AUTO-DISCARD][${iteration}] Game ${game_id} is not playing, stopping`);
                return { game, events: [] };
            }

            // Check if there are attacks on the table
            if (game.table_battles.length === 0) {
                console.log(`[AUTO-DISCARD][${iteration}] No attacks on table, stopping`);
                return { game, events: [] };
            }

            // Check if all attacks are covered
            const allAttacksCovered = game.table_battles.every(battle => battle.defense !== null);
            if (!allAttacksCovered) {
                console.log(`[AUTO-DISCARD][${iteration}] Not all attacks covered, stopping`);
                return { game, events: [] };
            }

            // Check if good_timestamp is set
            if (!game.good_timestamp) {
                console.log(`[AUTO-DISCARD][${iteration}] No good_timestamp set, stopping`);
                return { game, events: [] };
            }

            // Get all attackers
            const allAttackers = game.players.filter((p, index) =>
                index !== game.defender &&
                p.status === PLAYER_STATUS.IN
            );

            // Check if all attackers have pressed good
            const allAttackersGood = allAttackers.every(attacker =>
                game.good_players && game.good_players.includes(attacker.player_id)
            );

            // Check if timeout has been reached
            const timeElapsed = Date.now() - game.good_timestamp;
            const timeoutReached = timeElapsed >= TIMEOUT_MS;

            if (allAttackersGood) {
                console.log(`[AUTO-DISCARD][${iteration}] All ${allAttackers.length} attackers pressed good, triggering auto-discard`);
                shouldAutoDiscard = true;
            } else if (timeoutReached) {
                console.log(`[AUTO-DISCARD][${iteration}] Timeout reached (${timeElapsed}ms), triggering auto-discard`);
                shouldAutoDiscard = true;
            } else {
                const goodCount = game.good_players ? game.good_players.length : 0;
                const timeRemaining = TIMEOUT_MS - timeElapsed;
                console.log(`[AUTO-DISCARD][${iteration}] Waiting... ${goodCount}/${allAttackers.length} ready, ${timeRemaining}ms remaining`);
                shouldContinue = true;
            }

            // Execute auto-discard if conditions are met
            if (shouldAutoDiscard) {
                const transitionReason = allAttackersGood
                    ? `All ${allAttackers.length} attackers said good`
                    : `60-second timeout reached`;

                console.log(`[AUTO-DISCARD][${iteration}] Executing auto-discard: ${transitionReason}`);

                // Use shared round transition logic instead of faking a player action
                const transitionEvents = await executeRoundTransition(game, transitionReason);

                return { game, events: transitionEvents };
            }

            return { game, events: [] };
        });

        // Continue monitoring if needed
        if (shouldContinue) {
            await new Promise(resolve => setTimeout(resolve, CHECK_INTERVAL_MS));
            return await monitorAutoDiscard(game_id, iteration + 1);
        }

    } catch (error) {
        console.error(`[AUTO-DISCARD][${iteration}] Error monitoring game ${game_id}:`, error);
    }
}

