import { Card, AnimationEvent, ANIMATION_EVENT_TYPE, Game } from './types.ts';

// Helper function to create a deep copy of the game state for animation events
const cloneGameState = (game: Game): Game => {
    return JSON.parse(JSON.stringify(game));
}

// Animation event manager for collecting events during operations
export class AnimationEventManager {
    private events: AnimationEvent[] = [];
    
    clear() {
        this.events = [];
    }
    
    getEvents(): AnimationEvent[] {
        return [...this.events];
    }
    
    addEvent(event: AnimationEvent) {
        this.events.push(event);
    }
    
    addAttackEvent(player_id: string, cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table',
            game_state: cloneGameState(game)
        });
    }
    
    addPassEvent(player_id: string, cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table',
            game_state: cloneGameState(game)
        });
    }
    
    addCoverEvent(player_id: string, cover_card: Card, attack_card: Card, battle_index: number, game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.COVER,
            player_id,
            cards: [cover_card],
            target_card: attack_card,
            battle_index,
            from_location: 'hand',
            to_location: 'table',
            game_state: cloneGameState(game)
        });
    }
    
    addPickupEvent(player_id: string, cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.PICKUP,
            player_id,
            cards,
            from_location: 'table',
            to_location: 'hand',
            game_state: cloneGameState(game)
        });
    }
    
    addMagicTransitionEvent(message: string, game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
            message,
            game_state: cloneGameState(game)
        });
    }
    
    addDealEvent(player_id: string, cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEAL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand',
            game_state: cloneGameState(game)
        });
    }
    
    addFlippedEvent(card: Card, game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.FLIPPED,
            cards: [card],
            from_location: 'deck',
            to_location: 'flipped',
            game_state: cloneGameState(game)
        });
    }
    
    addDefenderMoveEvent(player_id: string, game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id,
            game_state: cloneGameState(game)
        });
    }
    
    addOutEvent(player_id: string, game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id,
            game_state: cloneGameState(game)
        });
    }
    
    addRefillEvent(player_id: string, cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.REFILL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand',
            game_state: cloneGameState(game)
        });
    }
    
    addCardsToTrashEvent(cards: Card[], game: Game) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards,
            from_location: 'table',
            to_location: 'discard',
            game_state: cloneGameState(game)
        });
    }
} 