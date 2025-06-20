import WebSocket from 'ws';
import * as readline from 'readline';

interface Message {
    type: string;
    message: string;
    timestamp?: string;
}

const WS_URL = 'ws://localhost:3001';

// Create readline interface for user input
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout
});

console.log('Connecting to server...');

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
    rl.question('> ', (input: string) => {
        if (input.toLowerCase() === 'quit') {
            console.log('Closing connection...');
            ws.close();
            return;
        }
        
        if (ws.readyState === WebSocket.OPEN) {
            const message: Message = {
                type: 'user_message',
                message: input,
                timestamp: new Date().toISOString()
            };
            
            console.log('Sending to server:', input);
            ws.send(JSON.stringify(message));
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