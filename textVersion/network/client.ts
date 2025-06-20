import WebSocket from 'ws';
import * as readline from 'readline';

interface Message {
    type: string;
    message: string;
    timestamp?: string;
    game_id?: string;
}

const WS_URL = 'ws://localhost:3001';

// Create readline interface for user input
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout
});

console.log('Connecting to server...');

// What "mode" the client is in
let current_game_id: string | null = null;

const games: Set<string> = new Set();

// Create WebSocket connection
const ws = new WebSocket(WS_URL);

ws.on('open', () => {
    console.log('Connected to server!');
    console.log('Type messages to send to server (type "quit" to exit):');

    // Start accepting user input
    promptUser();
});

ws.on('message', (data: WebSocket.Data) => {
    try {
        const message: Message = JSON.parse(data.toString());
        console.log(`[Server]: ${message.message}`);
        if (message.game_id) {
            games.add(message.game_id);
            if (message.type === 'game_created' || message.type === 'player_joined_game') {
                current_game_id = message.game_id;
            }
        }

        if (message.type === 'welcome') {
            console.log('Welcome message received from server');
        } else if (message.type === 'echo') {
            console.log(`Echo received at: ${message.timestamp}`);
        }
    } catch (error) {
        console.error('Error parsing server message:', error);
    }
});

ws.on('close', () => {
    console.log('Disconnected from server');
    rl.close();
});

ws.on('error', (error: Error) => {
    console.error('Connection error:', error);
    rl.close();
});

function promptUser(): void {
    rl.question(`${current_game_id ? `G${current_game_id}` : ''}> `, (input: string) => {
        const args = input.split(' ');

        const command = args[0].toLowerCase();


        if (command === 'quit') {
            console.log('Closing connection...');
            ws.close();
            return;
        }

        if (ws.readyState === WebSocket.OPEN) {
            // Login first. we'll keep it simple as it's local
            if (command === 'login') {
                if (args.length < 2) {
                    console.log('Please provide a player name');
                    //return;
                } else {


                    const player_name = args[1];

                    ws.send(JSON.stringify({
                        type: 'login',
                        player_name
                    }))
                }
            }

            // What's the first thing we want to do? Start a game
            // > game Alex [hash]
            else if (command === 'game') {
                const type = args.length > 1 ? 'join' : 'create';
                const game_id = args.length > 1 ? args[1] : null;

                ws.send(JSON.stringify({
                    type: type,
                    game_id: game_id,
                }))
            } else if (command === 'start') {
                if (current_game_id === null) {
                    console.log('Please enter a game first');
                } else {
                    ws.send(JSON.stringify({
                        type: 'start',
                        game_id: current_game_id,
                    }))
                }
                // The rest are purely local client side, for viewing games and entering them
            } else if (command === 'list') {
                console.log('Games:');
                games.forEach(game_id => {
                    console.log(`- ${game_id}`);
                });
            } else if (command === 'enter') {
                // if there is only one game, enter it
                if (games.size === 1) {
                    current_game_id = Array.from(games)[0];
                } else if (args.length < 2) {
                    console.log('Please provide a game id');
                } else {
                    const game_id = args[1];
                    current_game_id = game_id;
                }
            } else if (command === 'leave') {
                // this is purely client side
                current_game_id = null;
            }
        } else {
            console.log('Connection is not open');
        }

        // Continue prompting for input
        promptUser();
    });
}

// Handle process interruption
process.on('SIGINT', () => {
    console.log('\nClosing client...');
    ws.close();
    rl.close();
    process.exit(0);
}); 