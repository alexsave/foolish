import { BotStrategy } from '../bot_interfaces.ts';
import { Game, Card, LOG_TYPE } from '../types.ts';
import { LegalMove } from '../bot_interfaces.ts';
import { CardTracker } from '../durakai/cardTracker.ts';
import { calculateMoveStats, formatMoveStats } from './move_stats.ts';
import { cardDisplay } from '../common_utils.ts';

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
    
    constructor(apiKey?: string, model: string = 'gpt-5.2', verbose: boolean = false) {
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
                                'Rule variations: Passing is enabled. Players can attack with as many cards as the defender has, even if more than 6. ' +
                                'CRITICAL RULES: ' +
                                '1) "wait" is ALWAYS better than "pickup" - wait lets you see if more attacks come before deciding. You can still pickup later after waiting, but you cannot undo a pickup. NEVER choose pickup if wait is available. ' +
                                '2) "good" means an attacker signals they are done attacking - once all attackers say good and all attacks are covered, the round ends successfully. ' +
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
                    reasoning: {
                        effort: 'medium'
                    },
                    text: {
                        // Low verbosity for concise JSON output
                        verbosity: 'low',
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
                const cards = matched.cards?.map(c => cardDisplay(c)).join(', ') || '';
                console.log(`✅ GPT chose: ${matched.type.toUpperCase()}${cards ? ` [${cards}]` : ''}`);
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
        const tracker = new CardTracker(game, myPlayerId);
        const defenderIdx = game.defender;
        const isDefender = game.players[defenderIdx]?.player_id === myPlayerId;
        
        // Build entire game state as JSON
        const gameStateJson = {
            rules: "Passing is enabled. Players can attack with as many cards as the defender has, even if more than 6.",
            game: {
                trump_suit: game.power_suit,
                trump_symbol: ['♣', '♦', '♥', '♠'][game.power_suit],
                deck_size: game.deck.length,
                flipped_card: game.flipped ? this.cardToJson(game.flipped) : null,
                your_role: isDefender ? 'DEFENDER' : 'ATTACKER'
            },
            your_hand: myPlayer.hand.map(c => this.cardToJson(c)),
            table: game.table_battles.map(b => ({
                attack: this.cardToJson(b.attack),
                defense: b.defense ? this.cardToJson(b.defense) : null
            })),
            opponents: game.players
                .filter(p => p.player_id !== myPlayerId)
                .map((p, idx) => ({
                    name: p.name,
                    hand_size: p.hand.length,
                    is_defender: game.players.indexOf(p) === defenderIdx
                })),
            card_knowledge: this.buildCardKnowledge(game, myPlayerId, tracker),
            legal_moves: legalMoves.map((move, i) => {
                const { stats } = calculateMoveStats(game, myPlayerId, move, tracker);
                return {
                    index: i + 1,
                    type: move.type,
                    cards: move.cards?.map(c => this.cardToJson(c)) || [],
                    attack_cards: move.attack_cards?.map(c => this.cardToJson(c)) || [],
                    probabilities: this.statsToJson(stats),
                    move_json: this.legalMoveToJsonMove(move)
                };
            }),
            instructions: {
                choose: "Select the best move index based on probabilities and strategy",
                probabilities_guide: {
                    "P_Cover": "Higher = opponent more likely to cover your attack",
                    "P_AllowsAtk": "Lower = better for covers, less chance opponents can throw in",
                    "P_DrawBetter": "Higher = better cards likely after discarding these",
                    "P_PassBackPossible": "Higher = next player can pass back to you"
                },
                response_format: "Return JSON with: type, pairs (from move_json), chat_message (witty 1-2 sentences), move_justification (2-3 sentences)"
            }
        };
        
        return JSON.stringify(gameStateJson, null, 2);
    }

    private cardToJson(card: Card): { suit: number; value: number; display: string } {
        return {
            suit: card.suit,
            value: card.value,
            display: cardDisplay(card)
        };
    }

    private statsToJson(stats: any): Record<string, number> {
        const result: Record<string, number> = {};
        if (!stats) return result;
        
        // Handle attack stats (nested under stats.attack)
        if (stats.attack) {
            const a = stats.attack;
            if (typeof a.probCover === 'number') result.P_Cover = Math.round(a.probCover * 10000) / 100;
            if (typeof a.probCoverAllowsAttack === 'number') result.P_CoveringWillAllowAttack = Math.round(a.probCoverAllowsAttack * 10000) / 100;
            if (typeof a.probPass === 'number') result.P_PassBackPossible = Math.round(a.probPass * 10000) / 100;
        }
        
        // Handle cover stats (nested under stats.cover)
        if (stats.cover) {
            const c = stats.cover;
            if (typeof c.probAllowsAdditionalAttack === 'number') result.P_AllowsAtk = Math.round(c.probAllowsAdditionalAttack * 10000) / 100;
            if (typeof c.probDrawBetterCard === 'number') result.P_DrawBetter = Math.round(c.probDrawBetterCard * 10000) / 100;
        }
        
        // Handle pass stats (nested under stats.pass)
        if (stats.pass) {
            const p = stats.pass;
            if (typeof p.probCover === 'number') result.P_Cover = Math.round(p.probCover * 10000) / 100;
            if (typeof p.probCoverAllowsAttack === 'number') result.P_CoveringWillAllowAttack = Math.round(p.probCoverAllowsAttack * 10000) / 100;
            if (typeof p.probPass === 'number') result.P_PassBackPossible = Math.round(p.probPass * 10000) / 100;
        }
        
        return result;
    }

    private buildCardKnowledge(game: Game, myPlayerId: string, tracker: CardTracker): any {
        // Table cards
        const tableCards = game.table_battles.flatMap(b => 
            b.defense ? [this.cardToJson(b.attack), this.cardToJson(b.defense)] : [this.cardToJson(b.attack)]
        );
        
        // Discard
        const discardCards = tracker.getCardsInDiscard().map(c => this.cardToJson(c));

        // Known opponent cards
        const knownOpponentCards: Record<string, any[]> = {};
        for (const p of game.players) {
            if (p.player_id === myPlayerId) continue;
            const knownSet = tracker.knownCardsByPlayer.get(p.player_id);
            if (knownSet && knownSet.size > 0) {
                const cards = Array.from(knownSet).map(k => {
                    const [suit, value] = k.split('-').map(Number);
                    return this.cardToJson({ suit, value } as Card);
                });
                knownOpponentCards[p.name] = cards;
            }
        }
        
        // Unknown cards
        const unknownCards = this.getUnknownCards(game, myPlayerId, tracker).map(c => this.cardToJson(c));
        
        return {
            table_cards: tableCards,
            discard_pile: discardCards,
            discard_count: discardCards.length,
            known_opponent_cards: knownOpponentCards,
            unknown_cards: unknownCards,
            unknown_count: unknownCards.length
        };
    }
    
    private getUnknownCards(game: Game, myPlayerId: string, tracker: CardTracker): Card[] {
        const unknownCards: Card[] = [];
        const me = game.players.find(p => p.player_id === myPlayerId);
        const myHandKeys = new Set(me?.hand.map(c => `${c.suit}-${c.value}`) || []);
        const discardKeys = new Set(tracker.getCardsInDiscard().map(c => `${c.suit}-${c.value}`));
        const flippedKey = game.flipped ? `${game.flipped.suit}-${game.flipped.value}` : null;
        
        const tableKeys = new Set<string>();
        for (const battle of game.table_battles) {
            tableKeys.add(`${battle.attack.suit}-${battle.attack.value}`);
            if (battle.defense) tableKeys.add(`${battle.defense.suit}-${battle.defense.value}`);
        }
        
        const allKnownOpponentKeys = new Set<string>();
        for (const knownCards of tracker.knownCardsByPlayer.values()) {
            for (const key of knownCards) allKnownOpponentKeys.add(key);
        }
        
        const startValue = tracker.getMinCardValue();
        const ACE_VALUE = 13;
        
        for (let suit = 0; suit < 4; suit++) {
            for (let value = startValue; value <= ACE_VALUE; value++) {
                const key = `${suit}-${value}`;
                if (!myHandKeys.has(key) && !discardKeys.has(key) && key !== flippedKey && 
                    !tableKeys.has(key) && !allKnownOpponentKeys.has(key)) {
                    unknownCards.push({ suit, value });
                }
            }
        }
        return unknownCards;
    }
}

