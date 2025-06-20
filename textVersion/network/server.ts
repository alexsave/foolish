import * as http from 'http';
import WebSocket from 'ws';

interface Message {
    type: string;
    message: string;
    timestamp?: string;
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

interface Game {
    players: Player[];
    status: 'waiting' | 'playing'

}

interface GameMap{
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
    id: string|undefined;
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
            const message = JSON.parse(data.toString());
            // const message: Message = JSON.parse(data.toString());
            console.log('Received from client:', message);

            if (message.type === 'login') {
                users[player_id].name = message.player_name;
                player_games[player_id] = [];
                ws.send(JSON.stringify({
                    type: 'login_success',
                    message: `Player ${message.player_name} logged in`
                }));
            } else if (message.type === 'join') {
                const game_id = message.game_id;
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

                // add player to game
                games[game_id].players.push({
                    name: users[player_id].name,
                    id: player_id,
                    status: 'idle'
                });

                player_games[player_id].push(game_id);

                // send to all players in game
                games[game_id].players.forEach(player => {
                    user_ports[player.id].send(JSON.stringify({
                        type: 'player_joined_game',
                        message: `Player ${users[player_id].name} joined game ${game_id}`,
                        game_id: game_id
                    }));
                });

            } else if (message.type === 'create') {
                const game_id = createId();
                games[game_id] = {
                    status: 'waiting',
                    players: [{
                        name: users[player_id].name,
                        id: player_id,
                        status: 'idle'
                    }]
                }

                ws.send(JSON.stringify({
                    type: 'game_created',
                    message: `Game created with id ${game_id}`, 
                    game_id: game_id
                }));
            } else if (message.type === 'start') {
                // user wants to start a game. switch them to ready and see if all other players are ready. and if tehre are 2+ players

                // check if game exists
                if (!games[message.game_id]) {
                    ws.send(JSON.stringify({
                        type: 'game_not_found',
                        message: `Game ${message.game_id} not found`
                    }));
                    return;
                }

                const game = games[message.game_id];

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

                // send to all players in game
                game.players.forEach(player => {
                    user_ports[player.id].send(JSON.stringify({
                        type: 'player_ready',
                        message: `Player ${users[player_id].name} is ready`
                    }));
                });

                // check if all players are ready
                if (game.players.length >= 2 &&
                    game.players.every(player => player.status === 'ready')) {

                        // This is the game entry
                    game.status = 'playing';
                    game.players.forEach(player => {
                        player.status = 'in';
                    });
                    // send to all players in game
                    game.players.forEach(player => {
                        user_ports[player.id].send(JSON.stringify({
                            type: 'game_started',
                            message: `Game ${message.game_id} started`
                        }));
                    });
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