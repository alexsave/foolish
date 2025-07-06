import { Card, Game, PLAYER_STATUS, PersonalGame } from "./types";

export const get_next_player_index = (game: Game | PersonalGame, current_player: number): number => {
    let next_player = (current_player + 1) % game.players.length;
    while (game.players[next_player].status === PLAYER_STATUS.OUT) {
        next_player = (next_player + 1) % game.players.length;
    }
    return next_player;
}

export const canCover = (attack: Card, defense: Card, powerSuit: number) => {
    if (defense.suit !== attack.suit) {
        // only different suit scenario that works
        return defense.suit === powerSuit && attack.suit !== powerSuit;
    }
    return defense.value > attack.value;
};
