import * as http from 'http';
import { Card, initialize_hands, draw, cardDisplay, determine_lowest_power_index, set_positions, CARDS_PER_PLAYER, canCover, ACE_VALUE } from './index';
import WebSocket from 'ws';
import { wrap400, createId, verify_game_id, personalize_game, verify_player_in_game, start_game, lobbify_game, User, PLAYER_STATUS, Game, Player, PlayerStatus, GAME_STATUS, GameStatus, GAME_MOVE_TYPE, LOBBY_MOVE_TYPE, GameMoveType, Message, LobbyGame, SERVER_EVENT_TYPE, PersonalGame, OtherPlayer, PrivateMessage, GameMessage, check_win } from './common';
import express from 'express'
import { create } from './create';
import { login } from './login';
import { join } from './join';
import { start } from './start';
import { status } from './status';
import { attack } from './attack';
import { pass } from './pass';
import { pickup } from './pickup';
import { cover } from './cover';
import { good } from './good';

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


interface UserMap {
    [key: string]: User;
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


// index really
const name_to_id: { [key: string]: string } = {};

const app = express()
app.use(express.json());

app.use((req, res, next) => {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS, PUT, PATCH, DELETE');
    res.setHeader('Access-Control-Allow-Headers', 'X-Requested-With, Content-Type, Accept');
    res.setHeader('Content-Type', 'application/json');
    next();
});

app.get('/', (req, res) => {
    res.send('Hello World')
})

app.post('/' + LOBBY_MOVE_TYPE.LOGIN, login);
app.post('/' + LOBBY_MOVE_TYPE.CREATE, create);
app.post('/' + LOBBY_MOVE_TYPE.JOIN, join);
app.post('/' + LOBBY_MOVE_TYPE.START, start);
app.post('/' + GAME_MOVE_TYPE.STATUS, status);
app.post('/' + GAME_MOVE_TYPE.ATTACK, attack);
app.post('/' + GAME_MOVE_TYPE.PASS, pass);
app.post('/' + GAME_MOVE_TYPE.PICKUP, pickup);
app.post('/' + GAME_MOVE_TYPE.COVER, cover);
app.post('/' + GAME_MOVE_TYPE.GOOD, good);

app.listen(3009)



// Create WebSocket server
const wss = new WebSocket.Server({ port: WS_PORT });

console.log(`HTTP Server running on http://localhost:${PORT}`);
console.log(`WebSocket Server running on ws://localhost:${WS_PORT}`);


// different from the one in index.ts because we do this BEFORE shifting positions
// hope it works

// This will emulate one of the realtime channels of supabase. Most server events will go here
const public_game_channel: GameMessage[] = [];

// I think just "request good" and "draws _ card" will be here
const private_user_channel: PrivateMessage[] = [];

wss.on('connection', (ws: WebSocket) => {
    console.log('New client connected');

    // Give the client a unique id
    //const player_id = createId();
    //users[player_id] = {
    //name: '',
    //id: player_id
    ////}
    //user_ports[player_id] = ws;

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
            console.log('Received from client:', message);
            if (message.type === LOBBY_MOVE_TYPE.WEBSOCKET_CONNECT) {
                const player_id = message.player_id!;
                // Really the critical part here
                user_ports[player_id] = ws;
            } else {
                throw new Error(`Unknown message type: ${message.type}`);
            }

        } catch (error) {
            // This is the only type we actually care about lol
            ws.send(JSON.stringify({
                type: 'error',
                message: `Error: ${error}`
            }));
            console.error('Error parsing message:', error);
        }
    });

    // Handle client disconnect
    ws.on('close', () => {
        console.log('Client disconnected');
        // remove from user_ports
        // find where in user_ports this ws is
        const player_id = Object.keys(user_ports).find(id => user_ports[id] === ws);
        if (player_id) {
            delete user_ports[player_id];
        }
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


const broadcast_to_game = (game_id: string, message: Message) => {
    games[game_id].players.forEach(player => {
        user_ports[player.id].send(JSON.stringify(message));
    });
}

// Speed up or down for the hell of it
const SERVER_LOOP_INTERVAL = 1000;

setInterval(() => {
    // Batch to every 10s?

    // assuming they are kinda threadsafe lol
    while (public_game_channel.length > 0) {
        const message = public_game_channel.shift();
        if (message) {
            const game_id = message.game_id;
            if (games[game_id].status === GAME_STATUS.WAITING) {
                // we need to send the game to the client
                message.message.game = lobbify_game(games[game_id]);
                games[game_id].players.forEach(player => {
                    const port = user_ports[player.id];
                    if (port && port.readyState === WebSocket.OPEN) {
                        port.send(JSON.stringify(message));
                    }
                });
            } else {
                // we need to send the game to the client
                games[game_id].players.forEach(player => {
                    message.message.game = personalize_game(games[game_id], player.id);
                    const port = user_ports[player.id];
                    if (port && port.readyState === WebSocket.OPEN) {
                        port.send(JSON.stringify(message));
                    }
                });
            }
        }
    }

    while (private_user_channel.length > 0) {
        const message = private_user_channel.shift();
        if (message) {
            const port = user_ports[message.user_id];
            message.message.game = personalize_game(message.message.game as Game, message.user_id);
            if (port && port.readyState === WebSocket.OPEN) {
                port.send(JSON.stringify(message));
            }
        }
    }

}, SERVER_LOOP_INTERVAL);