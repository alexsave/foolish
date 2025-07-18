import { wrap400 } from '../_shared/utils.ts';
import { PublicGame, PublicPlayer, GAME_STATUS } from '../_shared/types.ts'; 

// TODO: just remove this. With the right policies we can query from client
wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state from separated tables
    //const game: Game = await loadCompleteGame(game_id);

    // Check if player is in game
    const playerInGame = game.players.find(player => player.player_id === user_id);
    console.log(JSON.stringify(playerInGame) + " playerInGame " + JSON.stringify(game.players) + " game.players " + user_id + " user_id");
    
    if (playerInGame) {
        console.log("player in the game");
        // Bandaid, but if  all players have 0 cards, set to waiting status
        // TODO: figure out why ending the game doesn't set status to waiting
        // Guard against overwriting GAME_OVER status
        if (game.status !== GAME_STATUS.GAME_OVER && game.players.every(player => player.hand.length === 0)) {
            game.status = GAME_STATUS.WAITING;
            //await saveCompleteGame(game);
        }

        // Player is in game, return personalized view
        //return {
        //    game: personalize_game(game, user_id)
        //};
    } else {
        console.log("player not in the game");
        // Player is not in game, return public view for spectating
        const publicGame: PublicGame = {
            id: game.id,
            name: game.name,
            deck_length: game.deck.length,
            flipped: game.flipped,
            players: game.players.map(player => ({
                name: player.name,
                player_id: player.player_id,
                status: player.status,
                hand_length: player.hand.length,
                is_ai: player.is_ai,
            } as PublicPlayer)),
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            defender: game.defender,
            table_battles: game.table_battles,
            elimination_order: game.elimination_order,
            discard_pile_length: game.discard_pile_length
        };
        
        //return {
        //    game: publicGame
        //};
    }
}, true);

