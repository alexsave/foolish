import express from 'express';
import { wrap400, verify_game_id, verify_player_in_game, database, personalize_game, Game, GAME_STATUS, SERVER_EVENT_TYPE, PLAYER_STATUS, refill, get_next_player_index } from '../shared';

export const good = wrap400((req: express.Request, res: express.Response) => {

    const { games } = database;
    const player_id = req.body.player_id;
    const game_id = verify_game_id(req.body.game_id);
    verify_player_in_game(game_id, player_id);

    handle_good(games[game_id], game_id, player_id);

    res.end(JSON.stringify({
        game_id: game_id,
        game: personalize_game(games[game_id], player_id)
    }));
});

const handle_good = (game: Game, game_id: string, player_id: string) => {
    const { public_game_channel } = database;

    // player is done attacking
    // we need to check if they have any cards left in their hand

    if (game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game_id} is not in wait_for_attackers mode`);
    }
    const player = game.players.find(player => player.id === player_id)!;
    // If they're in but can't play cards, just let them proceed
    if (player.status !== PLAYER_STATUS.IN && player.status !== PLAYER_STATUS.AWAITING_ATTACK) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // set them to done attacking
    player.status = PLAYER_STATUS.IN;

    // ok now we need to check if all players are done attacking
    // dont count the defender
    // the status check is critical
    const playable_players = game.players.filter(player => player.id !== game.players[game.currentlyAttacked].id && player.hand.some(card => card.value === game.flipped!.value) && player.status === PLAYER_STATUS.AWAITING_ATTACK);
    if (playable_players.length !== 0) {
        return;
    }

    // we are done attacking.
    // this has to be after a successful cover. Otherwise we'd still be waiting on the defender
    // shift
    // change all done_attacking to in
    game.players.forEach(player => {
        if (player.status === PLAYER_STATUS.AWAITING_ATTACK) {
            player.status = PLAYER_STATUS.IN;
        }
    });

    public_game_channel.push({
        game_id: game_id,
        message: {
            type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
            message: `Player ${player_id} successfully defended the attack`,
            game: game
        }
    });

    game.table = [];
    refill(game_id);

    //shift 
    game.firstAttacker = game.currentlyAttacked;
    game.currentlyAttacked = get_next_player_index(game, game.firstAttacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;
}