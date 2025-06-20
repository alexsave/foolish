import * as http from 'http';
import WebSocket from 'ws';

interface Message {
    type: string;
    message: string;
    timestamp?: string;
}

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
            
            // Echo back to client
            const echoMessage: Message = {
                type: 'echo',
                message: `Server received: ${message.message}`,
                timestamp: new Date().toISOString()
            };
            ws.send(JSON.stringify(echoMessage));
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