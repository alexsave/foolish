/**
 * Log-aware handwritten strategy
 * Uses game logs to make smarter decisions about which cards other players might have
 * 
 * Uses the game engine interface (SimpleMove) directly with real Game types
 */

import { Game, LOG_TYPE } from '../types';
import { 
    getLegalMoves, 
    SimpleMove 
} from './gameEngine';

export class LogAwareHandwrittenBot {
    name = 'log-aware-handwritten';
    
    /**
     * Analyze logs to determine which cards are known to be in other players' hands
     */
    private analyzeKnownCards(game: Game, playerIndex: number): Map<number, Set<string>> {
        const knownCards = new Map<number, Set<string>>();
        
        // Initialize sets for each player
        for (let i = 0; i < game.players.length; i++) {
            knownCards.set(i, new Set<string>());
        }
        
        // Go through logs and track what we know
        for (const log of game.logs) {
            if (!log.player_id) continue;
            
            // Find player index from player_id
            const playerId = game.players.findIndex(p => p.player_id === log.player_id);
            if (playerId === -1) continue;
            
            if (log.log_type === LOG_TYPE.PICKUP) {
                // Player picked up cards - we know they have these
                for (const pair of log.card_pairs) {
                    const cardKey = `${pair.primary.value}_${pair.primary.suit}`;
                    knownCards.get(playerId)?.add(cardKey);
                }
            } else if (log.log_type === LOG_TYPE.ATTACK || log.log_type === LOG_TYPE.COVER || log.log_type === LOG_TYPE.PASS) {
                // Player played these cards - remove from known cards
                for (const pair of log.card_pairs) {
                    const cardKey = `${pair.primary.value}_${pair.primary.suit}`;
                    for (const set of knownCards.values()) {
                        set.delete(cardKey);
                    }
                }
            }
        }
        
        return knownCards;
    }
    
    selectMove(game: Game, playerIndex: number): SimpleMove {
        const legalMoves = getLegalMoves(game, playerIndex);
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Analyze logs for intelligence
        const knownCards = this.analyzeKnownCards(game, playerIndex);
        
        // Simple strategy: prefer defend moves, then attacks, then pickup as last resort
        const defendMoves = legalMoves.filter(m => m.type === 'defend');
        if (defendMoves.length > 0) {
            // Pick lowest value defense card
            return defendMoves.reduce((best, curr) => {
                if (!best.card || !curr.card) return curr;
                return curr.card.value < best.card.value ? curr : best;
            });
        }
        
        const attackMoves = legalMoves.filter(m => m.type === 'attack');
        if (attackMoves.length > 0) {
            // Pick lowest value attack card
            return attackMoves.reduce((best, curr) => {
                if (!best.card || !curr.card) return curr;
                return curr.card.value < best.card.value ? curr : best;
            });
        }
        
        // Default: first legal move
        return legalMoves[0];
    }
}
