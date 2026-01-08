import { BotStrategy } from '../bot_interfaces.ts';
import { Game, Card, LOG_TYPE } from '../types.ts';
import { LegalMove } from '../bot_interfaces.ts';
import { CardTracker } from '../durakai/cardTracker.ts';

type JsonCard = { suit: number; value: number };
type JsonMovePair = { primary: JsonCard; target?: JsonCard | null };
type JsonMove = {
    type: LegalMove['type'];
    pairs?: JsonMovePair[];
    reasoning?: string;
    chat_message?: string;
    move_justification?: string;
};

/**
 * Helper to get environment variable, works with both Deno and Node.js
 * For Node.js, will try to load from .env file
 */
function getEnv(key: string): string | undefined {
    // Check if we're in Deno
    // @ts-ignore - Deno is not available in Node.js
    if (typeof Deno !== 'undefined' && typeof Deno.env !== 'undefined') {
        // @ts-ignore - Deno is not available in Node.js
        return Deno.env.get(key);
    }
    
    // We're in Node.js
    if (typeof process !== 'undefined' && process.env) {
        // Try to load dotenv if not already loaded
        if (!process.env[key]) {
            try {
                // Try to require dotenv (might not be installed)
                const dotenv = require('dotenv');
                dotenv.config();
            } catch (e) {
                // dotenv not available, try manual .env parsing
                try {
                    const fs = require('fs');
                    const path = require('path');
                    const envPath = path.resolve(process.cwd(), '.env');
                    
                    if (fs.existsSync(envPath)) {
                        const envContent = fs.readFileSync(envPath, 'utf8');
                        envContent.split('\n').forEach((line: string) => {
                            const match = line.match(/^([^=:#]+)\s*=\s*(.*)$/);
                            if (match) {
                                const key = match[1].trim();
                                const value = match[2].trim().replace(/^["']|["']$/g, '');
                                process.env[key] = value;
                            }
                        });
                    }
                } catch (fsError) {
                    // Ignore file read errors
                }
            }
        }
        return process.env[key];
    }
    
    return undefined;
}

/**
 * Bot strategy that uses an OpenAI model to make decisions.
 * Uses the Responses API with strict JSON schema output for reliable parsing.
 * Works with both Deno and Node.js (with .env file support).
 */
export class GPTBotStrategy implements BotStrategy {
    public readonly name: string = 'gpt';
    private apiKey: string;
    private model: string;
    private verbose: boolean;
    private baseUrl: string;
    
    constructor(apiKey?: string, model: string = 'gpt-4o-2024-08-06', verbose: boolean = false) {
        this.apiKey = apiKey || getEnv('OPENAI_API_KEY') || '';
        this.model = model;
        this.verbose = verbose || getEnv('OPENAI_VERBOSE') === 'true';
        this.baseUrl = (getEnv('OPENAI_BASE_URL') || 'https://api.openai.com').replace(/\/+$/, '');
        
        if (!this.apiKey) {
            throw new Error('OpenAI API key required. Set OPENAI_API_KEY environment variable or add to .env file.');
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
            const timeoutMs = Number(getEnv('OPENAI_TIMEOUT_MS') || 30_000);
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
                            content: 'You are an expert Durak (Russian card game) player with personality. ' +
                                'Choose the best move from the provided legal moves. ' +
                                'Include a short, witty chat message to other players (max 1-2 sentences). ' +
                                'Include a brief move justification (max 2-3 sentences). ' +
                                'Return ONLY a JSON object that matches the provided schema.'
                        },
                        {
                            role: 'user',
                            content: prompt
                        }
                    ],
                    text: {
                        format: {
                            type: 'json_schema',
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
                                        anyOf: [
                                            { type: 'null' },
                                            {
                                                type: 'array',
                                                description: 'Array of card pairs. For attack/pass: target should be null. For cover: target is the attacked card being covered.',
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
                                                    required: ['primary', 'target']
                                                }
                                            }
                                        ]
                                    },
                                    chat_message: {
                                        type: 'string',
                                        description: 'A short, witty message to other players (1-2 sentences max).'
                                    },
                                    move_justification: {
                                        type: 'string',
                                        description: 'Brief explanation of move strategy (2-3 sentences max).'
                                    }
                                },
                                required: ['type', 'pairs', 'chat_message', 'move_justification']
                            },
                            strict: true
                        }
                    }
                }),
                signal: controller.signal
            });
            clearTimeout(timeout);
            
            if (!response.ok) {
                const errorText = await response.text();
                throw new Error(`OpenAI API error: ${response.statusText} - ${errorText}`);
            }
            
            const data = await response.json();
            
            // Handle the response based on status
            if (data.status === 'incomplete') {
                throw new Error(`Incomplete response: ${data.incomplete_details?.reason}`);
            }
            
            // Check for refusal
            if (data.output && data.output[0]?.content?.[0]?.type === 'refusal') {
                throw new Error(`Model refused: ${data.output[0].content[0].refusal}`);
            }
            
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

            // Display chat message and justification
            if (jsonMove.chat_message) {
                console.log(`\n💬 GPT says: "${jsonMove.chat_message}"\n`);
            }
            if (jsonMove.move_justification) {
                console.log(`🧠 GPT reasoning: ${jsonMove.move_justification}\n`);
            }

            const matched = this.matchJsonMoveToLegalMove(jsonMove, legalMoves);
            if (!matched) {
                console.warn('GPT returned a move that does not match any legal move. Falling back to first legal move.');
                return legalMoves[0];
            }

            if (this.verbose) {
                console.log(`✅ GPT chose: ${this.formatMove(matched)}`);
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
        // Check for output_text field (some SDKs provide this)
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
                // Handle both output_text and text fields
                const text = part?.output_text || part?.text;
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
        const chat_message = typeof raw?.chat_message === 'string' ? raw.chat_message : undefined;
        const move_justification = typeof raw?.move_justification === 'string' ? raw.move_justification : undefined;

        return { type, pairs, chat_message, move_justification };
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
            prompt += `OPPONENT: ${opp.name} - ${opp.hand.length} cards in hand\n`;
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
        prompt += 'Return JSON with: type, pairs (if needed), chat_message (witty 1-2 sentences), move_justification (brief 2-3 sentences).\n';
        
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

