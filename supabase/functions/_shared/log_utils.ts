import { GameLog, LOG_TYPE, Game, UnsavedGameLog } from './types.ts';
import type { SupabaseClient } from 'jsr:@supabase/supabase-js';

// =============================================================================
// GAME LOG UTILITIES
// =============================================================================

// Internal function to save logs to database
// Should only be called from saveCompleteGame to ensure atomicity
export const saveGameLogs = async (supabaseClient: SupabaseClient, game_id: string, logs: GameLog[]): Promise<void> => {
    if (logs.length === 0) {
        return;
    }

    try {
        // Filter to only new logs (created within the last 10 minutes)
        // Due to Supabase function runtime limits, we can't be saving logs older than 10 minutes
        // This means logs older than 10 minutes must have been loaded from the database
        const tenMinutesAgo = new Date(Date.now() - 10 * 60 * 1000);
        const newLogs = logs.filter(log => new Date(log.created_at) > tenMinutesAgo);

        if (newLogs.length === 0) {
            console.log(`[LOG] No new logs to save for game ${game_id} (all logs older than 10 minutes)`);
            return;
        }

        const logsToUpsert = newLogs.map(log => ({
            id: log.id,
            game_id: log.game_id,
            log_type: log.log_type,
            player_id: log.player_id,
            card_pairs: log.card_pairs,
            defender_index: log.defender_index,
            created_at: log.created_at
        }));

        // Use upsert with onConflict to ignore existing logs
        const { error } = await supabaseClient
            .from('game_logs')
            .upsert(logsToUpsert, { onConflict: 'id', ignoreDuplicates: true });

        if (error) {
            console.error('Error saving game logs:', error);
            throw error; // Propagate error to fail the entire save operation
        }

        console.log(`[LOG] Successfully saved ${newLogs.length} new log(s) for game ${game_id}`);
    } catch (error) {
        console.error('Error in saveGameLogs:', error);
        throw error; // Ensure transaction-like behavior
    }
};

// Load all logs for the current game session
// Finds the most recent GAME_START and returns all logs after it
export const loadCurrentSessionLogs = async (supabaseClient: SupabaseClient, game_id: string): Promise<GameLog[]> => {
    try {
        // Get all logs for this game ordered by creation time
        const { data: allLogs, error } = await supabaseClient
            .from('game_logs')
            .select('*')
            .eq('game_id', game_id)
            .order('created_at', { ascending: true });

        if (error) {
            console.error('Error loading game logs:', error);
            return [];
        }

        if (!allLogs || allLogs.length === 0) {
            return [];
        }

        // Find the index of the most recent GAME_START
        let gameStartIndex = -1;
        for (let i = allLogs.length - 1; i >= 0; i--) {
            if (allLogs[i].log_type === LOG_TYPE.GAME_START) {
                gameStartIndex = i;
                break;
            }
        }

        // If no GAME_START found, return empty (shouldn't happen in normal operation)
        if (gameStartIndex === -1) {
            console.log(`[LOG] No GAME_START found for game ${game_id}, returning empty logs`);
            return [];
        }

        // Return logs from the current session (including GAME_START)
        const currentSessionLogs = allLogs.slice(gameStartIndex);
        console.log(`[LOG] Loaded ${currentSessionLogs.length} log(s) for current session of game ${game_id}`);
        
        return currentSessionLogs;
    } catch (error) {
        console.error('Error in loadCurrentSessionLogs:', error);
        return [];
    }
};

// Get the most recent game_start timestamp for a specific game
// This identifies the current play session
const getMostRecentGameStart = async (supabaseClient: SupabaseClient, game_id: string): Promise<Date | null> => {
    try {
        const { data, error } = await supabaseClient
            .from('game_logs')
            .select('created_at')
            .eq('game_id', game_id)
            .eq('log_type', LOG_TYPE.GAME_START)
            .order('created_at', { ascending: false })
            .limit(1)
            .single();

        if (error || !data) {
            return null;
        }

        return new Date(data.created_at);
    } catch (error) {
        console.error('Error getting most recent game start:', error);
        return null;
    }
};

// Clean up old game logs (older than 2 weeks, excluding current game session)
// Called when a game finishes
export const cleanupOldGameLogs = async (supabaseClient: SupabaseClient, game_id: string): Promise<void> => {
    try {
        console.log(`[CLEANUP] Starting cleanup of old game logs for game ${game_id}`);
        
        // Get the most recent game_start for this game
        const currentGameStart = await getMostRecentGameStart(supabaseClient, game_id);
        
        if (!currentGameStart) {
            console.log(`[CLEANUP] No game_start found for game ${game_id}, skipping cleanup`);
            return;
        }

        // Calculate cutoff date (2 weeks ago)
        const twoWeeksAgo = new Date();
        twoWeeksAgo.setDate(twoWeeksAgo.getDate() - 14);

        console.log(`[CLEANUP] Current game started at: ${currentGameStart.toISOString()}`);
        console.log(`[CLEANUP] Deleting logs older than: ${twoWeeksAgo.toISOString()}`);

        const cutoffDate = Math.min(twoWeeksAgo.getTime(), currentGameStart.getTime());

        // Delete logs that are:
        // 1. For this game_id
        // 2. Older than 2 weeks
        // 3. Created before the current game session started
        const { data, error } = await supabaseClient
            .from('game_logs')
            .delete()
            .eq('game_id', game_id)
            .lt('created_at', new Date(cutoffDate).toISOString())
            .select('id');

        if (error) {
            console.error('[CLEANUP] Error cleaning up old game logs:', error);
            return;
        }

        const deletedCount = data?.length || 0;
        if (deletedCount > 0) {
            console.log(`[CLEANUP] Deleted ${deletedCount} old log(s) for game ${game_id}`);
        } else {
            console.log(`[CLEANUP] No old logs to delete for game ${game_id}`);
        }
    } catch (error) {
        console.error('[CLEANUP] Error in cleanupOldGameLogs:', error);
        // Don't throw to prevent breaking game flow
    }
};
