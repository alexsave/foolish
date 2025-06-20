import * as http from 'http';
import { refill_deck, Card, Game, initialize_hands, draw, cardDisplay, determine_lowest_power_index, set_positions } from './index';
import WebSocket from 'ws';

interface Message {
    type: string;
    message: string;
    timestamp?: string;
    game_id?: string;
    cards?: Card[];
    player_name?: string;
}
interface Player {
    name: string;
    id: string;
    status: 'idle' | 'ready' | 'in' | 'out'
}
// idle just joined or waiting for other players, also after game end
// ready is ready to play
// in is in the game
// out is when they get rid of their hands, but still in the group


interface GameMap {
    [key: string]: Game;
}

interface PlayerGameMap {
    [key: string]: string[];
}


// game_id -> players: [players], state: {}, status
const games: GameMap = {};

// also players can have multiple game so
const player_games: PlayerGameMap = {};

// user table of id to name
interface User {
    name: string;
    id: string | undefined;
}

interface UserMap {
    [key: string]: User;
}

const createId = () => {
    return crypto.randomUUID().slice(0, 6);
}

// strictly local. I think
interface UserPort {
    [key: string]: WebSocket;
}
const user_ports: UserPort = {};

const users: UserMap = {};

const PORT = 3000;
const WS_PORT = 3001;

// Create HTTP server
const server = http.createServer((req: http.IncomingMessage, res: http.ServerResponse) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Foolish Card Game Server Running\n');
});

// Create WebSocket server
const wss = new WebSocket.Server({ port: WS_PORT });

console.log(`HTTP Server running on http://localhost:${PORT}`);
console.log(`WebSocket Server running on ws://localhost:${WS_PORT}`);

wss.on('connection', (ws: WebSocket) => {
    console.log('New client connected');

    // Give the client a unique id
    const player_id = createId();
    users[player_id] = {
        name: '',
        id: player_id
    }
    user_ports[player_id] = ws;

    // Send welcome message to client
    const welcomeMessage: Message = {
        type: 'welcome',
        message: 'Connected to Foolish Card Game Server'
    };
    ws.send(JSON.stringify(welcomeMessage));

    // Handle messages from client
    ws.on('message', (data: WebSocket.Data) => {
        try {
            const message: Message = JSON.parse(data.toString());
            // const message: Message = JSON.parse(data.toString());
            console.log('Received from client:', message);

            if (message.type === 'play_attack') {
                const game_id = message.game_id!;
                if (!games[game_id]) {
                    ws.send(JSON.stringify({
                        type: 'game_not_found',
                        message: `Game ${game_id} not found`
                    }));
                }

                // oh there's so much to verify here
                // ensure they are in the game 
                if (!player_games[player_id].includes(game_id)) {
                    ws.send(JSON.stringify({
                        type: 'player_not_in_game',
                        message: `Player ${player_id} is not in game ${game_id}`
                    }));
                    return;
                }
                const game = games[game_id];

                if (game.status === 'first_attacker') {
                    // check if player is first attacker
                    if (game.players[game.firstAttacker].id !== player_id) {
                        ws.send(JSON.stringify({
                            type: 'player_not_first_attacker',
                            message: `Player ${player_id} is not the first attacker`
                        }));
                        return;
                    }
                    if (!message.cards) {
                        ws.send(JSON.stringify({
                            type: 'no_cards_provided',
                            message: `No cards provided`
                        }));
                        return;
                    }
                    const mCards = message.cards!;
                    // check if every card is in hand
                    if (!mCards.every(card => game.players[game.firstAttacker].hand.includes(card))) {
                        ws.send(JSON.stringify({
                            type: 'card_not_in_hand',
                            message: `Card ${mCards.map(card => cardDisplay(card)).join(', ')} is not in player ${player_id}'s hand`
                        }));
                    }
                    // Ok passed checks, we can put the cards on the table
                    // remove from hand, put on table
                    game.players[game.firstAttacker].hand = game.players[game.firstAttacker].hand.filter(card => !mCards.includes(card));
                    for (const card of mCards) {
                        game.table.push({
                            attack: card,
                            defense: null
                        });
                    }
                    broadcast_to_game(game_id, {
                        type: 'attack_played', // i really gotta get the types under control
                        message: `Player ${player_id} played ${mCards.map(card => cardDisplay(card)).join(', ')}`,
                        cards: mCards
                    });
                    // check win later, becuase a "first attack" could win, putting the game into idle
                } else {
                    // handle others later
                }

            } else if (message.type === 'login') {
                const name: string = message.player_name!;
                users[player_id].name = name;
                player_games[player_id] = [];
                ws.send(JSON.stringify({
                    type: 'login_success',
                    message: `Player ${message.player_name} logged in`
                }));
            } else if (message.type === 'join') {
                const game_id: string = message.game_id!;
                if (!games[game_id]) {
                    ws.send(JSON.stringify({
                        type: 'game_not_found',
                        message: `Game ${game_id} not found`
                    }));
                    return;
                }

                // check if player is already in game
                if (player_games[player_id].includes(game_id)) {
                    ws.send(JSON.stringify({
                        type: 'player_already_in_game',
                        message: `Player ${player_id} is already in game ${game_id}`
                    }));
                    return;
                }

                // check if game is ongoing
                if (games[game_id].status !== 'waiting') {
                    ws.send(JSON.stringify({
                        type: 'game_not_waiting',
                        message: `Game ${game_id} is not waiting`
                    }));
                    return;
                }


                // add player to game
                games[game_id].players.push({
                    name: users[player_id].name,
                    id: player_id,
                    status: 'idle',
                    hand: []
                });

                player_games[player_id].push(game_id);

                // send to all players in game
                broadcast_to_game(game_id, {
                    type: 'player_joined_game',
                    message: `Player ${users[player_id].name} joined game ${game_id}`,
                    game_id: game_id
                });

            } else if (message.type === 'create') {
                const game_id = createId();
                games[game_id] = {
                    status: 'waiting',
                    players: [{
                        name: users[player_id].name,
                        id: player_id,
                        status: 'idle',
                        hand: []
                    }],
                    deck: [],
                    flipped: null,
                    powerSuit: 0,
                    firstAttacker: 0,
                    currentlyAttacked: 0,
                    previousFirstAttacker: 0,
                    previousCurrentlyAttacked: 0,
                    table: []
                }

                ws.send(JSON.stringify({
                    type: 'game_created',
                    message: `Game created with id ${game_id}`,
                    game_id: game_id
                }));
            } else if (message.type === 'start') {
                // user wants to start a game. switch them to ready and see if all other players are ready. and if tehre are 2+ players
                const game_id: string = message.game_id!;

                // check if game exists
                if (!games[game_id]) {
                    ws.send(JSON.stringify({
                        type: 'game_not_found',
                        message: `Game ${game_id} not found`
                    }));
                    return;
                }

                const game = games[game_id];

                // check if player is in game
                if (!game.players.find(player => player.id === player_id)) {
                    ws.send(JSON.stringify({
                        type: 'player_not_in_game',
                        message: `Player ${player_id} is not in game ${message.game_id}`
                    }));
                    return;
                }

                // check if game is waiting
                if (game.status !== 'waiting') {
                    ws.send(JSON.stringify({
                        type: 'game_not_waiting',
                        message: `Game ${message.game_id} is not waiting`
                    }));
                    return;
                }

                // set player to ready
                game.players.find(player => player.id === player_id)!.status = 'ready';

                broadcast_to_game(game_id, {
                    type: 'player_ready',
                    message: `Player ${users[player_id].name} is ready`
                });

                // check if all players are ready
                if (game.players.length >= 2 &&
                    game.players.every(player => player.status === 'ready')) {
                    // send to all players in game
                    broadcast_to_game(game_id, {
                        type: 'game_started',
                        message: `Game ${message.game_id} started`
                    });

                    start_game(game_id);

                }
            }

        } catch (error) {
            console.error('Error parsing message:', error);
        }
    });

    // Handle client disconnect
    ws.on('close', () => {
        console.log('Client disconnected');
    });

    // Handle errors
    ws.on('error', (error: Error) => {
        console.error('WebSocket error:', error);
    });
});

// Start HTTP server
server.listen(PORT, () => {
    console.log(`Server is listening on port ${PORT}`);
});

// Handle server shutdown gracefully
process.on('SIGINT', () => {
    console.log('\nShutting down server...');
    server.close();
    wss.close();
    process.exit(0);
});

const start_game = (game_id: string) => {
    // Assume that this is safe to call because we only call from server
    const game = games[game_id];

    // This is the game entry
    game.status = 'playing';
    game.players.forEach(player => {
        player.status = 'in';
    });

    game.deck = refill_deck();

    const hands = initialize_hands(game);
    for (let i = 0; i < game.players.length; i++) {
        game.players[i].hand = hands[i];

        user_ports[game.players[i].id].send(JSON.stringify({
            type: 'player_hand',
            message: `Player ${game.players[i].name} hand ${game.players[i].hand.map(card => cardDisplay(card)).join(', ')}`,
            hand: game.players[i].hand
        }));
    }

    game.flipped = draw(game);
    game.powerSuit = game.flipped!.suit;
    // Everyone needs to know
    broadcast_to_game(game_id, {
        type: 'flipped_card',
        message: `Flipped card is ${cardDisplay(game.flipped!)}`,
        //flipped: game.flipped
    });

    const lowest_power_index = determine_lowest_power_index(game);

    broadcast_to_game(game_id, {
        type: 'first_attacker',
        message: `Player ${game.players[lowest_power_index].name} is the first attacker`
    });

    game.firstAttacker = lowest_power_index;
    set_positions(game);


    // Ok NOW it is different from the single script
    /*
    the loop is
    chooseAttack from firstAttacker
    allowAdditionalAttacks from all attackers
    aiDefend from defender
    handleChoice based on defender choice
        while (continueBattle === true) {
            // Allow attacking again
            allowAttacks();
            choice = aiDefend();
            continueBattle = handleChoice(choice);
        }

    draw from deck
    check if game is over

    for server-client, this looks like
    requestAttack from first attacker, reject all other requests
    then pretty much anything can happen, the defender can cover, defender can pick up, defender can pass, additional attacks can be made
    only limit is that defender can't pick up for about 30s, to keep peopel from just swallowing 1 card and continuing

    */
   // request attack from first attacker

   game.status = 'first_attacker';
   broadcast_to_game(game_id, {
    type: 'game_status',
    message: `Wait for first attacker to attack`
   });
   user_ports[game.players[game.firstAttacker].id].send(JSON.stringify({
    type: 'request_first_attack',
    message: `Please choose an attack. Options are ${game.players[game.firstAttacker].hand.map(card => cardDisplay(card)).join(', ')}`,
   }));
    
}

const broadcast_to_game = (game_id: string, message: Message) => {
    games[game_id].players.forEach(player => {
        user_ports[player.id].send(JSON.stringify(message));
    });
}