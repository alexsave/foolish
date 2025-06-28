import * as http from 'http';
import WebSocket from 'ws';
import { database, personalize_game, lobbify_game, Game, GAME_STATUS, GAME_MOVE_TYPE, LOBBY_MOVE_TYPE, Message } from './shared/common';
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


// strictly local. I think
interface UserPort {
    [key: string]: WebSocket;
}
const user_ports: UserPort = {};

const PORT = 3000;
const WS_PORT = 3001;

// Create HTTP server
const server = http.createServer((req: http.IncomingMessage, res: http.ServerResponse) => {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Foolish Card Game Server Running\n');
});

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


wss.on('connection', (ws: WebSocket) => {
    console.log('New client connected');

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


// Speed up or down for the hell of it
const SERVER_LOOP_INTERVAL = 1000;

setInterval(() => {
    const { games, public_game_channel, private_user_channel, user_ports } = database;
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