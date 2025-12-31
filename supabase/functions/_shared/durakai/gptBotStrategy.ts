import { BotStrategy } from '../bot_interfaces';
import { Game, Card, LOG_TYPE } from '../types';
import { LegalMove } from '../bot_interfaces';
import { CardTracker } from './cardTracker';

type JsonCard = { suit: number; value: number };
type JsonMovePair = { primary: JsonCard; target?: JsonCard | null };
type JsonMove = {
    type: LegalMove['type'];
    pairs?: JsonMovePair[];
    reasoning?: string;
};

/**
 * Bot strategy that uses an OpenAI model to make decisions.
 * Uses the Responses API with strict JSON schema output for reliable parsing.
 */
export class GPTBotStrategy implements BotStrategy {
    public readonly name: string = 'gpt';
    private apiKey: string;
    private model: string;
    private verbose: boolean;
    private baseUrl: string;
    
    constructor(apiKey?: string, model: string = 'gpt-5.2', verbose: boolean = false) {
        this.apiKey = apiKey || process.env.OPENAI_API_KEY || '';
        this.model = model;
        this.verbose = verbose || process.env.OPENAI_VERBOSE === 'true';
        this.baseUrl = (process.env.OPENAI_BASE_URL || 'https://api.openai.com').replace(/\/+$/, '');
        
        if (!this.apiKey) {
            throw new Error('OpenAI API key required. Set OPENAI_API_KEY environment variable.');
        }
    }
    
    async chooseMove(game: Game, playerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) {
            throw new Error('No legal moves available');
        }
        
        if (legalMoves.length === 1) {
            return legalMoves[0];
        }
        
        // Format game state for GPT
        const prompt = this.formatGameState(game, playerId, legalMoves);
        
        try {
            // Call OpenAI Responses API with strict JSON schema output
            const controller = new AbortController();
            const timeoutMs = Number(process.env.OPENAI_TIMEOUT_MS || 30_000);
            const timeout = setTimeout(() => controller.abort(), timeoutMs);

            const response = await fetch(`${this.baseUrl}/v1/responses`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Authorization': `Bearer ${this.apiKey}`
                },
                body: JSON.stringify({
                    model: this.model,
                    input: [
                        {
                            role: 'system',
                            content: [
                                {
                                    type: 'text',
                                    text:
                                        'You are an expert Durak (Russian card game) player. ' +
                                        'Choose the best move from the provided legal moves. ' +
                                        'Return ONLY a JSON object that matches the provided schema.'
                                }
                            ]
                        },
                        {
                            role: 'user',
                            content: [{ type: 'text', text: prompt }]
                        }
                    ],
                    response_format: {
                        type: 'json_schema',
                        json_schema: {
                            name: 'durak_move',
                            schema: {
                                type: 'object',
                                additionalProperties: false,
                                properties: {
                                    type: {
                                        type: 'string',
                                        description: 'Move type',
                                        enum: ['attack', 'cover', 'pass', 'pickup', 'good', 'wait']
                                    },
                                    pairs: {
                                        type: 'array',
                                        description:
                                            'Array of card pairs. For attack/pass: target can be null/omitted. For cover: target is the attacked card being covered.',
                                        items: {
                                            type: 'object',
                                            additionalProperties: false,
                                            properties: {
                                                primary: {
                                                    type: 'object',
                                                    additionalProperties: false,
                                                    properties: {
                                                        suit: { type: 'integer', minimum: 0, maximum: 3 },
                                                        value: { type: 'integer', minimum: 2, maximum: 14 }
                                                    },
                                                    required: ['suit', 'value']
                                                },
                                                target: {
                                                    anyOf: [
                                                        { type: 'null' },
                                                        {
                                                            type: 'object',
                                                            additionalProperties: false,
                                                            properties: {
                                                                suit: { type: 'integer', minimum: 0, maximum: 3 },
                                                                value: { type: 'integer', minimum: 2, maximum: 14 }
                                                            },
                                                            required: ['suit', 'value']
                                                        }
                                                    ]
                                                }
                                            },
                                            required: ['primary']
                                        }
                                    },
                                    reasoning: {
                                        type: 'string',
                                        description: 'Brief explanation (optional but helpful).'
                                    }
                                },
                                required: ['type']
                            }
                        }
                    },
                    temperature: 0.4,
                    max_output_tokens: 200
                }),
                signal: controller.signal
            });
            clearTimeout(timeout);
            
            if (!response.ok) {
                const errorText = await response.text();
                throw new Error(`OpenAI API error: ${response.statusText} - ${errorText}`);
            }
            
            const data = await response.json();
            const content = this.extractResponseText(data)?.trim();
            
            if (!content) {
                throw new Error('No content in GPT response');
            }
            
            // Parse JSON response
            let result: any;
            try {
                result = JSON.parse(content);
            } catch (parseError) {
                console.warn(`Failed to parse GPT JSON response: ${content}`);
                throw new Error(`Invalid JSON from GPT: ${parseError}`);
            }
            
            const jsonMove = this.coerceJsonMove(result);

            if (this.verbose && jsonMove.reasoning) {
                console.log(`GPT reasoning: ${jsonMove.reasoning}`);
            }

            const matched = this.matchJsonMoveToLegalMove(jsonMove, legalMoves);
            if (!matched) {
                console.warn('GPT returned a move that does not match any legal move. Falling back to first legal move.');
                return legalMoves[0];
            }

            if (this.verbose) {
                console.log(`GPT chose: ${this.formatMove(matched)}`);
            }

            return matched;
            
        } catch (error) {
            console.error('Error calling GPT:', error);
            console.error('Falling back to first legal move');
            // Fallback to first move on error
            return legalMoves[0];
        }
    }

    private extractResponseText(data: any): string | null {
        // Some environments/SDKs provide `output_text`
        if (typeof data?.output_text === 'string' && data.output_text.length > 0) {
            return data.output_text;
        }

        // Standard Responses API shape: output[].content[].text
        const output = data?.output;
        if (!Array.isArray(output)) return null;

        const chunks: string[] = [];
        for (const item of output) {
            const content = item?.content;
            if (!Array.isArray(content)) continue;
            for (const part of content) {
                const text = part?.text;
                if (typeof text === 'string') chunks.push(text);
            }
        }

        return chunks.length ? chunks.join('') : null;
    }

    private coerceJsonMove(raw: any): JsonMove {
        const type = raw?.type as JsonMove['type'];
        if (
            type !== 'attack' &&
            type !== 'cover' &&
            type !== 'pass' &&
            type !== 'pickup' &&
            type !== 'good' &&
            type !== 'wait'
        ) {
            throw new Error(`Invalid or missing move.type from GPT: ${String(type)}`);
        }

        const pairs = Array.isArray(raw?.pairs) ? raw.pairs : undefined;
        const reasoning = typeof raw?.reasoning === 'string' ? raw.reasoning : undefined;

        return { type, pairs, reasoning };
    }

    private legalMoveToJsonMove(move: LegalMove): JsonMove {
        if (move.type === 'pickup' || move.type === 'good' || move.type === 'wait') {
            return { type: move.type };
        }

        const cards = move.cards || [];
        const targets = move.attack_cards || [];
        const pairs: JsonMovePair[] = [];

        for (let i = 0; i < cards.length; i++) {
            const primary = this.toJsonCard(cards[i]);
            const target = move.type === 'cover' && targets[i] ? this.toJsonCard(targets[i]) : null;
            pairs.push({ primary, target });
        }

        return { type: move.type, pairs };
    }

    private matchJsonMoveToLegalMove(chosen: JsonMove, legalMoves: LegalMove[]): LegalMove | null {
        const candidates = legalMoves.filter(m => m.type === chosen.type);
        if (candidates.length === 0) return null;

        // No-card moves
        if (chosen.type === 'pickup' || chosen.type === 'good' || chosen.type === 'wait') {
            return candidates[0] || null;
        }

        const chosenPrimaries = (chosen.pairs || [])
            .map(p => p?.primary)
            .filter((c: any): c is JsonCard => !!c && typeof c.suit === 'number' && typeof c.value === 'number');

        if (chosenPrimaries.length === 0) return null;

        const chosenKey = this.cardSetKey(chosenPrimaries);
        for (const m of candidates) {
            const mCards = (m.cards || []).map(c => this.toJsonCard(c));
            if (mCards.length === 0) continue;
            if (this.cardSetKey(mCards) === chosenKey) return m;
        }

        return null;
    }

    private toJsonCard(card: Card): JsonCard {
        return { suit: card.suit, value: card.value };
    }

    private cardSetKey(cards: JsonCard[]): string {
        return cards
            .map(c => `${c.suit}-${c.value}`)
            .sort()
            .join('|');
    }
    
    private formatGameState(game: Game, myPlayerId: string, legalMoves: LegalMove[]): string {
        const myPlayer = game.players.find(p => p.player_id === myPlayerId)!;
        const opponentIds = game.players.filter(p => p.player_id !== myPlayerId).map(p => p.player_id);
        const tracker = new CardTracker(game, myPlayerId);
        
        let prompt = '=== DURAK GAME STATE ===\n\n';
        
        // Game info
        prompt += `Trump suit: ${this.suitName(game.power_suit)}\n`;
        prompt += `Deck cards remaining: ${game.deck.length}\n`;
        if (game.flipped) {
            prompt += `Flipped card: ${this.cardName(game.flipped)}\n`;
        }
        prompt += `Discard pile: ${game.discard_pile_length} cards\n\n`;
        
        // My hand
        prompt += `YOUR HAND (${myPlayer.hand.length} cards):\n`;
        for (const card of myPlayer.hand) {
            prompt += `  ${this.cardName(card)}\n`;
        }
        prompt += '\n';
        
        // Opponents
        for (const oppId of opponentIds) {
            const opp = game.players.find(p => p.player_id === oppId)!;
            prompt += `OPPONENT: ${opp.hand.length} cards in hand\n`;
        }
        prompt += '\n';
        
        // Current player roles
        const defenderIdx = game.defender;
        const defenderId = game.players[defenderIdx]?.player_id;
        
        prompt += `Defender: ${defenderId === myPlayerId ? 'YOU' : 'Opponent'}\n`;
        prompt += `You are ${defenderId === myPlayerId ? 'defending' : 'attacking or passing'}\n\n`;
        
        // Table battles
        if (game.table_battles.length > 0) {
            prompt += `CURRENT ATTACKS ON TABLE:\n`;
            for (const battle of game.table_battles) {
                if (battle.defense) {
                    prompt += `  ${this.cardName(battle.attack)} → COVERED BY ${this.cardName(battle.defense)}\n`;
                } else {
                    prompt += `  ${this.cardName(battle.attack)} → UNCOVERED\n`;
                }
            }
            prompt += '\n';
        }

        // Card knowledge summary (derived from event logs)
        prompt += this.formatCardKnowledgeSummary(game, myPlayerId, tracker);
        
        // Legal moves
        prompt += `YOUR LEGAL MOVES:\n`;
        for (const move of legalMoves) {
            prompt += `  - ${this.formatMove(move)}\n`;
            prompt += `    JSON: ${JSON.stringify(this.legalMoveToJsonMove(move))}\n`;
        }
        prompt += '\n';

        prompt += 'Analyze the situation and choose the best move.\n';
        prompt += 'Return JSON describing the move (type + pairs). Use the provided JSON lines exactly.\n';
        
        return prompt;
    }

    private formatCardKnowledgeSummary(game: Game, myPlayerId: string, tracker: CardTracker): string {
        let s = '';
        s += `CARD KNOWLEDGE (from logs; partial information):\n`;

        // Discard info
        const discarded = tracker.getCardsInDiscard();
        s += `  Known discarded cards: ${discarded.length}\n`;
        if (discarded.length > 0) {
            const shown = discarded.slice(0, 16).map(c => this.cardName(c)).join(', ');
            s += `    ${shown}${discarded.length > 16 ? ' ...' : ''}\n`;
        }

        // Known opponent cards
        s += `  Known opponent cards (total): ${tracker.getKnownOpponentCardCount()}\n`;
        for (const p of game.players) {
            if (p.player_id === myPlayerId) continue;
            const knownSet = tracker.knownCardsByPlayer.get(p.player_id);
            const knownKeys = knownSet ? Array.from(knownSet) : [];
            s += `    Opponent (${p.hand.length} cards): known ${knownKeys.length}\n`;
            if (knownKeys.length > 0) {
                const cards = knownKeys
                    .slice(0, 12)
                    .map((k) => {
                        const [suitStr, valueStr] = k.split('-');
                        const suit = Number(suitStr);
                        const value = Number(valueStr);
                        return this.cardName({ suit, value } as Card);
                    })
                    .join(', ');
                s += `      ${cards}${knownKeys.length > 12 ? ' ...' : ''}\n`;
            }
        }

        // Unknown pool
        const unknownCount = tracker.getUnknownCardCount();
        s += `  Unknown cards remaining (not accounted for): ${unknownCount}\n`;
        s += `  Avg unknown card value: ${tracker.getAverageUnknownCardValue().toFixed(2)}\n`;

        // Table-aware probabilities
        if (game.table_battles.length > 0) {
            const uncovered = game.table_battles.filter(b => !b.defense).map(b => b.attack);
            if (uncovered.length > 0) {
                s += `  Defender cover probability (for uncovered attacks):\n`;
                for (const attack of uncovered.slice(0, 6)) {
                    const p = tracker.getProbabilityCanCover(attack);
                    s += `    ${this.cardName(attack)}: ${(p * 100).toFixed(0)}%\n`;
                }
                if (uncovered.length > 6) s += `    ...\n`;
            }

            const valuesOnTable = Array.from(new Set(game.table_battles.map(b => b.attack.value)));
            if (valuesOnTable.length > 0) {
                s += `  Opponent pass-back probability (by value on table):\n`;
                for (const v of valuesOnTable.slice(0, 6)) {
                    const p = tracker.getProbabilityCanPass(v);
                    s += `    value ${v}: ${(p * 100).toFixed(0)}%\n`;
                }
                if (valuesOnTable.length > 6) s += `    ...\n`;
            }
        }

        s += `\n`;
        return s;
    }
    
    private formatMove(move: LegalMove): string {
        switch (move.type) {
            case 'attack':
                if (move.cards && move.cards.length > 0) {
                    return `ATTACK with ${move.cards.map(c => this.cardName(c)).join(', ')}`;
                }
                return 'ATTACK (no cards specified)';
                
            case 'cover':
                if (move.cards && move.cards.length > 0) {
                    return `COVER attacks with ${move.cards.map(c => this.cardName(c)).join(', ')}`;
                }
                return 'COVER';
                
            case 'pass':
                if (move.cards && move.cards.length > 0) {
                    return `PASS and add ${move.cards.map(c => this.cardName(c)).join(', ')}`;
                }
                return 'PASS';
                
            case 'pickup':
                return 'PICKUP all cards from table';
                
            case 'good':
                return 'SAY GOOD (end your turn)';
                
            case 'wait':
                return 'WAIT';
                
            default:
                return `Unknown move type: ${move.type}`;
        }
    }
    
    private cardName(card: Card): string {
        const suits = ['♣', '♦', '♥', '♠'];
        const values: Record<number, string> = {
            2: '2', 3: '3', 4: '4', 5: '5', 6: '6', 7: '7', 8: '8', 9: '9', 10: '10',
            11: 'J', 12: 'Q', 13: 'K', 14: 'A'
        };
        return `${values[card.value] || card.value}${suits[card.suit]}`;
    }
    
    private suitName(suit: number): string {
        const suits = ['Clubs♣', 'Diamonds♦', 'Hearts♥', 'Spades♠'];
        return suits[suit] || `Suit${suit}`;
    }
}

