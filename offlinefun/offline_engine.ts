// Minimal offline game - just enough to display the UI
import { PersonalGame, Card, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from './types.ts';

export const createOfflineGame = (playerName: string, gameName: string, botCount: number): PersonalGame => {
    // Simple IDs
    const playerId = `offline_player`;
    const gameId = `offline_game`;
    
    // Basic 6 random cards for the player
    const suits = [0, 1, 2, 3];
    const values = [6, 7, 8, 9, 10, 11, 12, 13, 14];
    const playerHand: Card[] = [];
    
    for (let i = 0; i < 6; i++) {
        playerHand.push({
            suit: suits[Math.floor(Math.random() * suits.length)],
            value: values[Math.floor(Math.random() * values.length)]
        });
    }
    
    // Random flipped card
    const flippedCard: Card = {
        suit: suits[Math.floor(Math.random() * suits.length)],
        value: values[Math.floor(Math.random() * values.length)]
    };

    return {
        id: gameId,
        name: gameName,
        status: GAME_STATUS.PLAYING,
        players: [
            {
                player_id: playerId,
                name: playerName,
                status: PLAYER_STATUS.IN,
                hand_length: 6,
                is_ai: false
            },
            ...Array.from({ length: botCount }, (_, i) => ({
                player_id: `bot_${i + 1}`,
                name: `Bot ${i + 1}`,
                status: PLAYER_STATUS.IN,
                hand_length: 6,
                is_ai: true
            }))
        ],
        self: {
            player_id: playerId,
            name: playerName,
            status: PLAYER_STATUS.IN,
            hand: playerHand,
            hand_length: 6,
            awaiting_attack: true,
            is_ai: false,
            strategy_key: STRATEGY_KEY.HUMAN
        },
        deck_length: 20,
        discard_pile_length: 0,
        flipped: flippedCard,
        power_suit: flippedCard.suit,
        first_attacker: 0,
        defender: 1,
        table_battles: [],
        elimination_order: []
    };
};

// Minimal stubs - just return the same game state (ignore parameters)
export const attackOffline = async (cards: Card[]): Promise<any> => ({ game_id: 'offline_game' });
export const coverOffline = async (defenseCards: Card[], attackCards: Card[]): Promise<any> => ({ game_id: 'offline_game' });
export const passOffline = async (cards: Card[]): Promise<any> => ({ game_id: 'offline_game' });
export const pickupOffline = async (): Promise<any> => ({ game_id: 'offline_game' });
export const goodOffline = async (): Promise<any> => ({ game_id: 'offline_game' });
export const processBotMoves = async () => {};