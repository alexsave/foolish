import { Game } from '../../supabase/functions/_shared/types';
import { BotStrategy, LegalMove } from '../../supabase/functions/_shared/bot_interfaces';
import { FeatureExtractor } from '../../supabase/functions/_shared/durakai/featureExtractor';

// @ts-ignore - neataptic doesn't have types
import * as neataptic from 'neataptic';

/**
 * NEAT-based bot strategy that uses neural network to evaluate moves
 * Learns through neuroevolution
 */
export class NEATBotStrategy implements BotStrategy {
    readonly name = 'neat';
    private network: any; // neataptic network
    private includePolynomialFeatures: boolean;
    
    constructor(network?: any, options?: { includePolynomialFeatures?: boolean }) {
        this.includePolynomialFeatures = options?.includePolynomialFeatures ?? false;
        if (network) {
            this.network = network;
        } else {
            // Create a simple feedforward network if none provided
            const inputCount = this.includePolynomialFeatures
                ? FeatureExtractor.getFeatureCount()
                : FeatureExtractor.getBaseFeatureCount();
            const outputCount = 1; // Score for this move
            
            this.network = new neataptic.Architect.Perceptron(
                inputCount,
                Math.floor(inputCount / 2),
                Math.floor(inputCount / 4),
                outputCount
            );
        }
    }
    
    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        if (legalMoves.length === 1) {
            return legalMoves[0];
        }
        
        // Extract features and evaluate each move
        const extractor = new FeatureExtractor(game, botPlayerId, {
            includePolynomial: this.includePolynomialFeatures
        });
        const moveScores: { move: LegalMove; score: number }[] = [];
        
        for (const move of legalMoves) {
            const features = extractor.extractFeaturesForMove(move);
            const output = this.network.activate(features);
            const score = Array.isArray(output) ? output[0] : output;
            
            moveScores.push({ move, score });
        }
        
        // Sort by score (highest first)
        moveScores.sort((a, b) => b.score - a.score);
        
        // Return the highest scored move
        return moveScores[0].move;
    }
    
    /**
     * Get the underlying network for evolution
     */
    getNetwork(): any {
        return this.network;
    }
    
    /**
     * Create a new NEATBotStrategy from a network
     */
    static fromNetwork(network: any, options?: { includePolynomialFeatures?: boolean }): NEATBotStrategy {
        return new NEATBotStrategy(network, options);
    }
    
    /**
     * Export network to JSON
     */
    toJSON(): string {
        return JSON.stringify(this.network.toJSON());
    }
    
    /**
     * Import network from JSON
     */
    static fromJSON(json: string): NEATBotStrategy {
        const networkData = JSON.parse(json);
        const network = neataptic.Network.fromJSON(networkData);
        return new NEATBotStrategy(network);
    }
}

