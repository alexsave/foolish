import { Game } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';

// Nitro — built from scratch against the espresso curriculum.
// Baseline: return the last legal move.
export class NitroStrategy implements BotStrategy {
    readonly name = 'nitro';

    async chooseMove(_game: Game, _botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        return legalMoves[legalMoves.length - 1];
    }
}
