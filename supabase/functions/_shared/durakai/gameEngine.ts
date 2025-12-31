/**
 * Durak Game Engine - Using Real Game Types
 * No adapters needed - uses Game, Card, Battle, etc. from types.ts
 */

// @ts-ignore - seedrandom doesn't have types
import seedrandom from 'seedrandom';
import { 
    Game, 
    Card, 
    Battle, 
    PrivatePlayer, 
    PLAYER_STATUS, 
    GAME_STATUS,
    GameLog,
    LogCardPair,
    LOG_TYPE,
    LogType
} from '../types';

// -------------------------------------------------------------------------
//                          Helper Functions
// -------------------------------------------------------------------------

export function cardToString(card: Card): string {
    const face: Record<number, string> = {
        11: "J", 12: "Q", 13: "K", 14: "A"
    };
    const suits = ['♠', '♥', '♦', '♣'];
    return `${face[card.value] || card.value}${suits[card.suit]}`;
}

export function cardEquals(a: Card, b: Card): boolean {
    return a.value === b.value && a.suit === b.suit;
}

export function cardBeats(attacker: Card, defender: Card, trumpSuit: number): boolean {
    // Same suit: higher value wins
    if (attacker.suit === defender.suit) {
        return attacker.value > defender.value;
    }
    // Trump beats non-trump
    if (attacker.suit === trumpSuit && defender.suit !== trumpSuit) {
        return true;
    }
    return false;
}

// -------------------------------------------------------------------------
//                          Deck Configuration
// -------------------------------------------------------------------------

export function getDeckConfig(playerCount: number): { ranks: number[], cardsPerPlayer: number } {
    if (playerCount >= 2 && playerCount <= 4) {
        return { ranks: [6, 7, 8, 9, 10, 11, 12, 13, 14], cardsPerPlayer: 6 }; // 36 cards
    } else if (playerCount >= 5 && playerCount <= 8) {
        return { ranks: [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14], cardsPerPlayer: 6 }; // 52 cards
    } else {
        throw new Error(`Invalid player count: ${playerCount}. Must be 2-8.`);
    }
}

export function createShuffledDeck(playerCount: number, rng: () => number = Math.random): Card[] {
    const { ranks } = getDeckConfig(playerCount);
    const deck: Card[] = [];
    
    for (const rank of ranks) {
        for (let suit = 0; suit < 4; suit++) {
            deck.push({ value: rank, suit });
        }
    }
    
    // Fisher-Yates shuffle with seeded RNG
    for (let i = deck.length - 1; i > 0; i--) {
        const j = Math.floor(rng() * (i + 1));
        [deck[i], deck[j]] = [deck[j], deck[i]];
    }
    
    return deck;
}

// -------------------------------------------------------------------------
//                          Game State Management
// -------------------------------------------------------------------------

export function addLog(game: Game, log: Omit<GameLog, 'id' | 'created_at' | 'game_id'>): void {
    game.logs.push({
        id: crypto.randomUUID(),
        created_at: new Date().toISOString(),
        game_id: game.id,
        ...log
    });
}

// Replenish hands after a round
export function replenishHands(game: Game): void {
    const { cardsPerPlayer } = getDeckConfig(game.players.length);
    
    // Replenish in order: attacker first, then defender, then others
    const replenishOrder = [game.first_attacker, game.defender];
    for (let p = 0; p < game.players.length; p++) {
        if (!replenishOrder.includes(p) && game.players[p].status === PLAYER_STATUS.IN) {
            replenishOrder.push(p);
        }
    }
    
    for (const p of replenishOrder) {
        if (game.players[p].status === PLAYER_STATUS.IN) {
            const cardsDrawn: Card[] = [];
            while (game.players[p].hand.length < cardsPerPlayer && game.deck.length > 0) {
                const card = game.deck.shift()!;
                game.players[p].hand.push(card);
                cardsDrawn.push(card);
            }
            
            // Log draws (as unknown cards for other players to see)
            if (cardsDrawn.length > 0) {
                addLog(game, {
                    log_type: LOG_TYPE.DRAW,
                    player_id: game.players[p].player_id,
                    card_pairs: cardsDrawn.map(() => ({
                        primary: { value: -1, suit: -1 } // Unknown card
                    })),
                    defender_index: null
                });
            }
            
            // If player has no cards and couldn't draw any (deck empty), they go OUT
            if (cardsDrawn.length === 0 && game.players[p].hand.length === 0) {
                game.players[p].status = PLAYER_STATUS.OUT;
                addLog(game, {
                    log_type: LOG_TYPE.PLAYER_OUT,
                    player_id: game.players[p].player_id,
                    card_pairs: [],
                    defender_index: null
                });
            }
        }
    }
}

function nextPlayerIndex(game: Game, playerIndex: number): number {
    const startIndex = playerIndex;
    let nextP = (playerIndex + 1) % game.players.length;
    let attempts = 0;
    const maxAttempts = game.players.length;
    
    while (game.players[nextP].status === PLAYER_STATUS.OUT && attempts < maxAttempts) {
        nextP = (nextP + 1) % game.players.length;
        attempts++;
    }
    
    // If we've checked all players and they're all OUT except possibly the starting player
    // This means the game is terminal - return the starting index
    if (attempts >= maxAttempts) {
        console.error(`[ENGINE] nextPlayerIndex: No valid next player found from P${playerIndex}`);
        return playerIndex; // Return self to avoid infinite loop
    }
    
    return nextP;
}

function activePlayerCount(game: Game): number {
    return game.players.filter(p => p.status === PLAYER_STATUS.IN).length;
}

// -------------------------------------------------------------------------
//                          Legal Moves
// -------------------------------------------------------------------------

function legalAttacks(game: Game, playerIndex: number): Card[] {
    const attacker = game.players[playerIndex];
    
    if (game.table_battles.length === 0) {
        // First attack: any card
        return [...attacker.hand];
    }
    
    // Can only attack with ranks already on table (attack or defense cards)
    const allowedRanks = new Set<number>();
    for (const battle of game.table_battles) {
        allowedRanks.add(battle.attack.value);
        if (battle.defense) {
            allowedRanks.add(battle.defense.value);
        }
    }
    
    // Also enforce defender hand limit
    const defenderHandSize = game.players[game.defender].hand.length;
    const currentAttacks = game.table_battles.length;
    if (currentAttacks >= defenderHandSize) {
        return []; // Can't attack more than defender can handle
    }
    
    return attacker.hand.filter(c => allowedRanks.has(c.value));
}

function legalDefenses(game: Game): Card[] {
    const defender = game.players[game.defender];
    
    // Find first undefended attack
    const attackCard = game.table_battles.find(b => b.defense === null)?.attack;
    if (!attackCard) {
        return [];
    }
    return defender.hand.filter(c => cardBeats(c, attackCard, game.power_suit));
}

function legalPasses(game: Game): Card[] {
    if (game.table_battles.length === 0 || game.table_battles.length >= 6) {
        return [];
    }
    
    const defender = game.players[game.defender];
    
    // Get all ATTACK ranks on table (defense cards don't matter for passing)
    const attackRanks = new Set<number>();
    for (const battle of game.table_battles) {
        attackRanks.add(battle.attack.value);
    }
    
    // Can only pass if ALL attacks have same rank
    if (attackRanks.size !== 1) {
        return [];
    }
    
    // Can only pass if next defender can handle the load
    const nextDefender = nextPlayerIndex(game, game.defender);
    if (game.players[nextDefender].hand.length < game.table_battles.length + 1) {
        return [];
    }
    
    // Can pass cards matching the attack rank
    const requiredRank = Array.from(attackRanks)[0];
    return defender.hand.filter(c => c.value === requiredRank);
}

export type MoveType = 'attack' | 'defend' | 'pass' | 'pickup' | 'good' | 'wait';

export interface SimpleMove {
    type: MoveType;
    card?: Card; // For backwards compatibility and single-card moves
    cards?: Card[]; // For multi-card moves (attacks, covers, passes)
}

// -------------------------------------------------------------------------
//                          Turn Management
// -------------------------------------------------------------------------

/**
 * Determines whose turn it is based on current game state.
 * Returns the player index, or -1 if the game is over.
 * When multiple players can make valid moves, picks one randomly to simulate 
 * real-world "first-come-first-serve" timing.
 */
export function getCurrentPlayer(game: Game): number {
    if (isTerminal(game)) {
        return -1;
    }

    // If no battles on table, it's the first attacker's turn (must be IN)
    if (game.table_battles.length === 0) {
        // Make sure first_attacker is IN, otherwise skip to next IN player
        let attacker = game.first_attacker;
        let attempts = 0;
        while (attempts < game.players.length && game.players[attacker].status !== PLAYER_STATUS.IN) {
            attacker = (attacker + 1) % game.players.length;
            attempts++;
        }
        return attacker;
    }

    const allCovered = game.table_battles.every(b => b.defense !== null);
    
    // Special case: if all covered AND defender is OUT, auto-transition immediately
    // No need to wait for "good" moves - just trigger transition
    if (allCovered && game.players[game.defender].status === PLAYER_STATUS.OUT) {
        // Return any IN attacker - they'll auto-trigger transition since defender is OUT
        for (let i = 0; i < game.players.length; i++) {
            if (i !== game.defender && game.players[i].status === PLAYER_STATUS.IN) {
                return i;
            }
        }
        return -1; // Game should be terminal
    }
    
    // Collect ALL players who have valid moves
    const playersWithMoves: number[] = [];
    
    // Check each player for legal moves
    for (let i = 0; i < game.players.length; i++) {
        const player = game.players[i];
        if (player.status !== PLAYER_STATUS.IN) {
            continue;
        }
        
        const legalMoves = getLegalMoves(game, i);
        if (legalMoves.length > 0) {
            playersWithMoves.push(i);
        }
    }
    
    if (playersWithMoves.length === 0) {
        // No players have valid moves - this shouldn't happen
        console.error('[ENGINE ERROR] No players have valid moves');
        console.error(`  activeCount: ${game.players.filter(p => p.status === PLAYER_STATUS.IN).length}`);
        console.error(`  allCovered: ${allCovered}, defender: ${game.defender}, first_attacker: ${game.first_attacker}`);
        return -1;
    }
    
    // Pick a random player from those who can move (simulates real-world timing)
    const randomIndex = Math.floor(Math.random() * playersWithMoves.length);
    return playersWithMoves[randomIndex];
}

export function getLegalMoves(game: Game, playerIndex: number): SimpleMove[] {
    const moves: SimpleMove[] = [];
    const player = game.players[playerIndex];
    
    if (game.status !== "playing") {
        return moves;
    }
    
    const isDefender = playerIndex === game.defender;
    const isFirstAttacker = playerIndex === game.first_attacker;
    const allCovered = game.table_battles.every(b => b.defense !== null);
    const hasPlayerSaidGood = game.good_players?.includes(player.player_id) || false;
    
    if (game.table_battles.length === 0 && isFirstAttacker) {
        // First attack
        for (const card of legalAttacks(game, playerIndex)) {
            moves.push({ type: 'attack', card });
        }
    } else if (isDefender && game.table_battles.length > 0) {
        // Defender's turn
        for (const card of legalDefenses(game)) {
            moves.push({ type: 'defend', card });
        }
        for (const card of legalPasses(game)) {
            moves.push({ type: 'pass', card });
        }
        moves.push({ type: 'pickup' });
        
        // Defender can only wait if:
        // 1. All attacks are covered
        // 2. There are still attackers who haven't said "good" yet
        if (allCovered) {
            const activePlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN).length;
            const playersSaidGood = game.good_players?.length || 0;
            // Can wait if not all other players (excluding defender) have said good
            if (playersSaidGood < activePlayers - 1) {
                moves.push({ type: 'wait' });
            }
        }
    } else if (!isDefender && game.table_battles.length > 0) {
        // Attacker adding cards
        // Only allow attacks if player hasn't said "good" yet
        if (!hasPlayerSaidGood) {
            for (const card of legalAttacks(game, playerIndex)) {
                moves.push({ type: 'attack', card });
            }
        }
        // Can say "good" if all covered and hasn't said it yet
        if (allCovered && !hasPlayerSaidGood) {
            moves.push({ type: 'good' });
        }
    }
    
    return moves;
}

// -------------------------------------------------------------------------
//                          Apply Moves
// -------------------------------------------------------------------------

export function applyMove(game: Game, playerIndex: number, move: SimpleMove): void {
    const player = game.players[playerIndex];
    
    if (move.type === 'attack') {
        // Support both single card and multi-card attacks
        const cardsToAttack = move.cards || (move.card ? [move.card] : []);
        
        if (cardsToAttack.length === 0) {
            console.error('[ENGINE ERROR] Attack move with no cards');
            return;
        }
        
        // Remove cards from attacker's hand and add to table
        cardsToAttack.forEach(card => {
            const cardIdx = player.hand.findIndex(c => cardEquals(c, card));
            if (cardIdx !== -1) {
                player.hand.splice(cardIdx, 1);
                game.table_battles.push({ attack: card, defense: null });
            }
        });
        
        // Reset good_players when new attack is added - game state has changed!
        game.good_players = [];
        
        // Log attack
        addLog(game, {
            log_type: LOG_TYPE.ATTACK,
            player_id: player.player_id,
            card_pairs: cardsToAttack.map(card => ({ primary: card })),
            defender_index: null
        });
    } else if (move.type === 'defend') {
        // Support both single card and multi-card covers
        const cardsToCover = move.cards || (move.card ? [move.card] : []);
        
        if (cardsToCover.length === 0) {
            console.error('[ENGINE ERROR] Defend move with no cards');
            return;
        }
        
        const cardPairs: LogCardPair[] = [];
        
        // Cover attacks in order
        cardsToCover.forEach(defenseCard => {
            const cardIdx = player.hand.findIndex(c => cardEquals(c, defenseCard));
            if (cardIdx !== -1) {
                player.hand.splice(cardIdx, 1);
                
                // Find the first undefended attack
                const battleIdx = game.table_battles.findIndex(b => b.defense === null);
                if (battleIdx !== -1) {
                    const attackCard = game.table_battles[battleIdx].attack;
                    game.table_battles[battleIdx].defense = defenseCard;
                    cardPairs.push({ primary: defenseCard, target: attackCard });
                }
            }
        });
        
        // Log cover
        addLog(game, {
            log_type: LOG_TYPE.COVER,
            player_id: player.player_id,
            card_pairs: cardPairs,
            defender_index: null
        });
        
        // Check if all attacks are covered
        // In multiplayer (3+ players), attacks can continue after all are covered
        // The round only ends when an attacker says "good"
        // In 2-player, the round ends immediately when all attacks are covered
        const allCovered = game.table_battles.every(b => b.defense !== null);
        const playerCount = game.players.length;
        
        if (allCovered && playerCount === 2) {
            // 2-player: automatically end the round
            addLog(game, {
                log_type: LOG_TYPE.DISCARD,
                player_id: null,
                card_pairs: game.table_battles.flatMap(b => [
                    { primary: b.attack },
                    { primary: b.defense! }
                ]),
                defender_index: null
            });
            
            game.table_battles = [];
            replenishHands(game);
            
            // Check for player out
            if (player.hand.length === 0 && game.deck.length === 0) {
                player.status = PLAYER_STATUS.OUT;
                addLog(game, {
                    log_type: LOG_TYPE.PLAYER_OUT,
                    player_id: player.player_id,
                    card_pairs: [],
                    defender_index: null
                });
            }
            
            game.first_attacker = game.defender;
            game.defender = nextPlayerIndex(game, game.defender);
            
            addLog(game, {
                log_type: LOG_TYPE.DEFENDER_CHANGE,
                player_id: null,
                card_pairs: [],
                defender_index: game.defender
            });
        }
        // In multi-player (3+), wait for attackers to say "good"
    } else if (move.type === 'pass' && move.card) {
        // Remove card from defender's hand and add to table
        const cardIdx = player.hand.findIndex(c => cardEquals(c, move.card!));
        player.hand.splice(cardIdx, 1);
        game.table_battles.push({ attack: move.card, defense: null });
        
        // Reset good_players when new attack is added via pass - game state has changed!
        game.good_players = [];
        
        // Log pass
        addLog(game, {
            log_type: LOG_TYPE.PASS,
            player_id: player.player_id,
            card_pairs: [{ primary: move.card }],
            defender_index: null
        });
        
        // Pass to next player
        game.defender = nextPlayerIndex(game, game.defender);
        
        addLog(game, {
            log_type: LOG_TYPE.DEFENDER_CHANGE,
            player_id: null,
            card_pairs: [],
            defender_index: game.defender
        });
    } else if (move.type === 'pickup') {
        // Collect all cards from table
        const pickedUpCards: Card[] = [];
        for (const battle of game.table_battles) {
            pickedUpCards.push(battle.attack);
            player.hand.push(battle.attack);
            if (battle.defense) {
                pickedUpCards.push(battle.defense);
                player.hand.push(battle.defense);
            }
        }
        
        // Log pickup
        addLog(game, {
            log_type: LOG_TYPE.PICKUP,
            player_id: player.player_id,
            card_pairs: pickedUpCards.map(c => ({ primary: c })),
            defender_index: null
        });
        
        game.table_battles = [];
        replenishHands(game);
        
        // Only transition roles if game is not terminal
        if (!isTerminal(game)) {
            // Next player attacks, skip the defender
            game.first_attacker = nextPlayerIndex(game, game.defender);
            game.defender = nextPlayerIndex(game, game.first_attacker);
            
            addLog(game, {
                log_type: LOG_TYPE.DEFENDER_CHANGE,
                player_id: null,
                card_pairs: [],
                defender_index: game.defender
            });
        }
    } else if (move.type === 'good') {
        // Add player to good_players list
        if (!game.good_players) {
            game.good_players = [];
        }
        if (!game.good_players.includes(player.player_id)) {
            game.good_players.push(player.player_id);
        }
        
        // Log good
        addLog(game, {
            log_type: LOG_TYPE.GOOD,
            player_id: player.player_id,
            card_pairs: [],
            defender_index: null
        });
        
        // Check if ALL attackers (non-defenders with IN status) have said good
        // OR if defender is OUT (then we don't need to wait for "good")
        const defender = game.players[game.defender];
        const defenderIsOut = defender.status === PLAYER_STATUS.OUT;
        
        const activePlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN);
        const potentialAttackers = activePlayers.filter((p, idx) => {
            const playerIndex = game.players.indexOf(p);
            return playerIndex !== game.defender; // Not the defender
        });
        const attackerCount = potentialAttackers.length;
        const goodCount = game.good_players.length;
        
        // Only end round if all potential attackers have said good OR defender is OUT
        if (goodCount >= attackerCount || defenderIsOut) {
            // Successfully defended - discard all cards
            addLog(game, {
                log_type: LOG_TYPE.DISCARD,
                player_id: null,
                card_pairs: game.table_battles.flatMap(b => [
                    { primary: b.attack },
                    ...(b.defense ? [{ primary: b.defense }] : [])
                ]),
                defender_index: null
            });
            
            game.table_battles = [];
            game.good_players = []; // Reset for next round
            replenishHands(game);
            
            // Check for player out
            const defender = game.players[game.defender];
            if (defender.hand.length === 0 && game.deck.length === 0) {
                defender.status = PLAYER_STATUS.OUT;
                addLog(game, {
                    log_type: LOG_TYPE.PLAYER_OUT,
                    player_id: defender.player_id,
                    card_pairs: [],
                    defender_index: null
                });
            }
            
            // Only transition roles if game is not terminal
            if (!isTerminal(game)) {
                // Defender successfully defended, so they become the first attacker
                // But if they're OUT, skip to the next IN player
                if (defender.status === PLAYER_STATUS.OUT) {
                    game.first_attacker = nextPlayerIndex(game, game.defender);
                } else {
                    game.first_attacker = game.defender;
                }
                game.defender = nextPlayerIndex(game, game.first_attacker);
                
                addLog(game, {
                    log_type: LOG_TYPE.DEFENDER_CHANGE,
                    player_id: null,
                    card_pairs: [],
                    defender_index: game.defender
                });
            }
        }
        // Otherwise, just mark player as having said good and continue
    } else if (move.type === 'wait') {
        // Defender is waiting for attackers to finish or say "good"
        // No state change needed - just pass the turn back to an attacker
        // The game loop will handle selecting the next attacker
    }
}

// -------------------------------------------------------------------------
//                          Game Setup
// -------------------------------------------------------------------------

export function initializeGame(playerCount: number, seed?: string): Game {
    if (playerCount < 2 || playerCount > 8) {
        throw new Error(`Invalid player count: ${playerCount}. Must be 2-8.`);
    }
    
    // Create and shuffle deck with seeded RNG
    const rng = seed ? seedrandom(seed) : Math.random;
    const deck = createShuffledDeck(playerCount, rng);
    const trumpSuit = deck[deck.length - 1].suit;
    const { cardsPerPlayer } = getDeckConfig(playerCount);
    
    // Deal cards
    const players: PrivatePlayer[] = [];
    for (let i = 0; i < playerCount; i++) {
        const hand: Card[] = [];
        for (let j = 0; j < cardsPerPlayer; j++) {
            if (deck.length > 0) {
                hand.push(deck.shift()!);
            }
        }
        players.push({
            player_id: `player_${i}`,
            name: `Player ${i}`,
            hand,
            status: PLAYER_STATUS.IN,
            hand_length: hand.length,
            is_ai: true,
            awaiting_attack: true,
            done_attacking_this_round: false
        });
    }
    
    // Find player with lowest trump to start
    let lowestTrumpValue = 15;
    let lowestTrumpPlayer = 0;
    
    for (let i = 0; i < playerCount; i++) {
        for (const card of players[i].hand) {
            if (card.suit === trumpSuit && card.value < lowestTrumpValue) {
                lowestTrumpValue = card.value;
                lowestTrumpPlayer = i;
            }
        }
    }
    
    const firstAttacker = lowestTrumpPlayer;
    const defender = (firstAttacker + 1) % playerCount;
    
    const game: Game = {
        id: 'test_game',
        name: 'Test Game',
        deck,
        deck_length: deck.length,
        discard_pile_length: 0,
        flipped: deck.length > 0 ? deck[deck.length - 1] : null,
        power_suit: trumpSuit,
        players,
        status: "playing",
        first_attacker: firstAttacker,
        defender,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        logs: []
    };
    
    // Add GAME_START log
    addLog(game, {
        log_type: LOG_TYPE.GAME_START,
        player_id: null,
        card_pairs: [],
        defender_index: defender
    });
    
    return game;
}

// -------------------------------------------------------------------------
//                          Game Status
// -------------------------------------------------------------------------

export function isTerminal(game: Game): boolean {
    // Use the same game completion logic as the real game
    // Game is terminal when only 1 player remains IN
    const in_players = game.players.filter(player => player.status === PLAYER_STATUS.IN);
    if (in_players.length === 1) {
        return true;
    }
    
    // Also check if deck is empty and any player with IN status has no cards
    // This handles the edge case where a player goes OUT mid-game
    if (game.deck.length === 0) {
        for (const player of game.players) {
            if (player.status === PLAYER_STATUS.IN && player.hand.length === 0) {
                player.status = PLAYER_STATUS.OUT;
                // IMPORTANT: emit PLAYER_OUT so finishing order and benchmarks don't misclassify.
                addLog(game, {
                    log_type: LOG_TYPE.PLAYER_OUT,
                    player_id: player.player_id,
                    card_pairs: [],
                    defender_index: null
                });
                // Check again if only 1 player left
                const remaining = game.players.filter(p => p.status === PLAYER_STATUS.IN);
                if (remaining.length === 1) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

export function getWinner(game: Game): string | null {
    const activePlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN);
    if (activePlayers.length === 1) {
        return activePlayers[0].player_id;
    }
    return null;
}

export function getFinishingOrder(game: Game): number[] {
    // Game must be terminal (only 1 player left IN)
    const inPlayers = game.players.filter(p => p.status === PLAYER_STATUS.IN);
    if (inPlayers.length !== 1) {
        return []; // Game not done yet
    }
    
    // The last remaining IN player is the loser (the fool)
    // Winners are those who went OUT (in the order they went out)
    
    // Extract PLAYER_OUT logs to determine order
    const playerOutLogs = game.logs
        .filter(log => log.log_type === LOG_TYPE.PLAYER_OUT)
        .map(log => {
            const playerIndex = game.players.findIndex(p => p.player_id === log.player_id);
            return playerIndex;
        });
    
    // Add any remaining IN player as the loser (fool) at the end
    const loser = game.players.findIndex(p => p.status === PLAYER_STATUS.IN);
    
    // Return winners in order they went out, then loser (fool) last
    return loser >= 0 ? [...playerOutLogs, loser] : playerOutLogs;
}
