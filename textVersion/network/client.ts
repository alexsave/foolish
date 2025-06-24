import WebSocket from 'ws';
import * as readline from 'readline';
import { Card } from './index';
import { LOBBY_MOVE_TYPE, GAME_MOVE_TYPE } from './common';

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

function parse_card(card: string): Card {
    const value: string = card.slice(0, 1);
    const suit: string = card.slice(1);
    // 'SHCD'
    const suit_map = {
        'S': 0,
        'H': 1,
        'C': 2,
        'D': 3
    }

    const value_map = {
        '2': 1,
        '3': 2,
        '4': 3,
        '5': 4,
        '6': 5,
        '7': 6,
        '8': 7,
        '9': 8,
        'T': 9,
        'J': 10,
        'Q': 11,
        'K': 12,
        'A': 13,
    }

    return {
        value: value_map[value as keyof typeof value_map],
        suit: suit_map[suit as keyof typeof suit_map]
    };
}

function promptUser(): void {
    rl.question(`${current_game_id ? `Table-${current_game_id}` : ''}> `, (input: string) => {
        const args = input.split(' ');

        const command = args[0].toLowerCase();


        if (command === 'quit') {
            console.log('Closing connection...');
            ws.close();
            return;
        }

        if (ws.readyState === WebSocket.OPEN) {

            if (command === GAME_MOVE_TYPE.ATTACK) {
                // not ideal, best to just let them type the card name
                if (args.length < 2) {
                    console.log('Please provide a card(s) value. Ex. King of Hearts -> KH, 7 of Clubs -> 7C');
                } else {

                    // take all but the first argument
                    const cards = args.slice(1).map(parse_card);

                    // maybe do some client side validation here to save on server use
                    ws.send(JSON.stringify({
                        type: GAME_MOVE_TYPE.ATTACK,
                        cards: cards,
                        game_id: current_game_id
                    }))
                }
            } else if (command === GAME_MOVE_TYPE.GOOD) {
                // simply means we are done attacking
                ws.send(JSON.stringify({
                    type: GAME_MOVE_TYPE.GOOD,
                    game_id: current_game_id
                }))
            } else if (command === GAME_MOVE_TYPE.COVER) {
                // simply means we are done covering
                if (args.length < 2) {
                    console.log('Please provide a card(s) value and the card it will cover in pairs. Ex. Ten of Clubs covers 7 of clubs -> TC 7C');
                } else {
                    const cards = args.slice(1).map(parse_card);
                    // so evens will become cover array, odds will become attack array
                    const cover_cards = cards.filter((_, index) => index % 2 === 0);
                    const attack_cards = cards.filter((_, index) => index % 2 === 1);

                    ws.send(JSON.stringify({
                        type: GAME_MOVE_TYPE.COVER,
                        cards: cover_cards,
                        attack_cards: attack_cards,
                        game_id: current_game_id
                    }))
                }
            } else if (command === GAME_MOVE_TYPE.PASS) {
                if (args.length < 2) {
                    console.log('Please provide a card(s) value. Ex. King of Hearts -> KH, 7 of Clubs -> 7C');
                } else {
                    const cards = args.slice(1).map(parse_card);
                    ws.send(JSON.stringify({
                        type: GAME_MOVE_TYPE.PASS,
                        cards: cards,
                        game_id: current_game_id
                    }))
                }
            } else if (command === GAME_MOVE_TYPE.PICKUP) {
                // simply means we are done picking up
                ws.send(JSON.stringify({
                    type: GAME_MOVE_TYPE.PICKUP,
                    game_id: current_game_id
                }))

                // Login first. we'll keep it simple as it's local
            } else if (command === LOBBY_MOVE_TYPE.LOGIN) {
                if (args.length < 2) {
                    console.log('Please provide a player name');
                    //return;
                } else {


                    const player_name = args[1];

                    ws.send(JSON.stringify({
                        type: LOBBY_MOVE_TYPE.LOGIN,
                        player_name
                    }))
                }

                // What's the first thing we want to do? Start a game
                // > game Alex [hash]
            } else if (command === LOBBY_MOVE_TYPE.CREATE) {

                ws.send(JSON.stringify({
                    type: LOBBY_MOVE_TYPE.CREATE,
                }))
            } else if (command === LOBBY_MOVE_TYPE.JOIN) {
                const game_id = args.length > 1 ? args[1] : null;

                ws.send(JSON.stringify({
                    type: LOBBY_MOVE_TYPE.JOIN,
                    game_id: game_id,
                }))
            } else if (command === LOBBY_MOVE_TYPE.START) {
                if (current_game_id === null) {
                    console.log('Please enter a game first');
                } else {
                    ws.send(JSON.stringify({
                        type: LOBBY_MOVE_TYPE.START,
                        game_id: current_game_id,
                    }))
                }
            } else if (command === GAME_MOVE_TYPE.STATUS) {
                if (current_game_id === null) {
                    console.log('Please enter a game first');
                } else {
                    // request status from server
                    ws.send(JSON.stringify({
                        type: GAME_MOVE_TYPE.STATUS,
                        game_id: current_game_id,
                    }))
                }

            } else if (command === LOBBY_MOVE_TYPE.LIST) {
                // The rest are purely local client side, for viewing games and entering them
                console.log('Games:');
                games.forEach(game_id => {
                    console.log(`- ${game_id}`);
                });
            } else if (command === LOBBY_MOVE_TYPE.ENTER) {
                // if there is only one game, enter it
                if (games.size === 1) {
                    current_game_id = Array.from(games)[0];
                } else if (args.length < 2) {
                    console.log('Please provide a game id');
                } else {
                    const game_id = args[1];
                    current_game_id = game_id;
                }
            } else if (command === LOBBY_MOVE_TYPE.BACK) {
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