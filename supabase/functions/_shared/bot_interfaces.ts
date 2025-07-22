import { Card, Game } from './types.ts';

// Legal moves that a bot can make
export interface LegalMove {
    type: 'attack' | 'cover' | 'pass' | 'pickup' | 'good' | 'wait';
    cards?: Card[];
    attack_cards?: Card[]; // For cover moves, which cards to cover
    done_attacking_this_round?: boolean; // For attack moves, whether to be done attacking this round
}

// Bot strategy interface
export interface BotStrategy {
    // Given the game state and bot's hand, choose a legal move
    chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove>;
    
    // Strategy identifier
    readonly name: string;
} 