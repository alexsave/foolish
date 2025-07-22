import { Game } from './types.ts';
import { BotStrategy, LegalMove } from './bot_interfaces.ts';

// One card per attack strategy - only puts down one card per attack round
export class OneCardBotStrategy implements BotStrategy {
    readonly name = 'one_card';
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        // Filter to only attack moves that use exactly one card AND are done attacking this round
        const singleCardDoneAttackMoves = legalMoves.filter(move => 
            move.type === 'attack' && 
            move.cards && 
            move.cards.length === 1 && 
            move.done_attacking_this_round === true
        );
        
        // If we have single card "done attacking" moves, prefer those
        if (singleCardDoneAttackMoves.length > 0) {
            const randomIndex = Math.floor(Math.random() * singleCardDoneAttackMoves.length);
            return singleCardDoneAttackMoves[randomIndex];
        }
        
        // Prefer wait over other non-attack moves (smart defensive play)
        const waitMoves = legalMoves.filter(move => move.type === 'wait');
        if (waitMoves.length > 0) {
            return waitMoves[0];
        }
        
        // Otherwise, for non-attack moves, choose randomly
        const nonAttackMoves = legalMoves.filter(move => move.type !== 'attack');
        if (nonAttackMoves.length > 0) {
            const randomIndex = Math.floor(Math.random() * nonAttackMoves.length);
            return nonAttackMoves[randomIndex];
        }
        
        // If only other attack moves available, prefer "done attacking" versions
        const doneAttackMoves = legalMoves.filter(move => 
            move.type === 'attack' && move.done_attacking_this_round === true
        );
        if (doneAttackMoves.length > 0) {
            // Sort by number of cards, pick the one with fewest cards
            doneAttackMoves.sort((a, b) => (a.cards?.length || 0) - (b.cards?.length || 0));
            return doneAttackMoves[0];
        }
        
        // Fallback to random move
        const randomIndex = Math.floor(Math.random() * legalMoves.length);
        return legalMoves[randomIndex];
    }
} 