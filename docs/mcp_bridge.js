const net = require('net');

const PORT = 30001;
const HOST = '127.0.0.1';

// Get token from env
const authHeader = process.env.AUTHORIZATION || '';
const bearerToken = authHeader.replace(/^Bearer\s+/i, '').trim();

if (!bearerToken) {
    console.error('Error: AUTHORIZATION environment variable must be set with the Bearer token.');
    process.exit(1);
}

const client = new net.Socket();

client.connect(PORT, HOST, () => {
    // We are connected.
});

client.on('error', (err) => {
    console.error('TCP Connection Error:', err.message);
    process.exit(1);
});

client.on('close', () => {
    process.exit(0);
});

// Buffer for receiving data from TCP
let tcpBuffer = '';

client.on('data', (data) => {
    tcpBuffer += data.toString('utf8');
    let nlPos;
    while ((nlPos = tcpBuffer.indexOf('\n')) !== -1) {
        const message = tcpBuffer.slice(0, nlPos).trim();
        tcpBuffer = tcpBuffer.slice(nlPos + 1);
        if (message) {
            // Forward TCP message to stdout (MCP Client)
            process.stdout.write(message + '\n');
        }
    }
});

// Read from stdin (MCP Client)
const readline = require('readline');
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    terminal: false
});

rl.on('line', (line) => {
    if (!line.trim()) return;

    try {
        const req = JSON.parse(line);
        // Intercept initialize request to inject bearer_token
        if (req.method === 'initialize') {
            req.params = req.params || {};
            req.params.bearer_token = bearerToken;
        }

        // Forward to TCP socket
        client.write(JSON.stringify(req) + '\n');
    } catch (e) {
        // If it's not valid JSON, just pass it through 
        client.write(line + '\n');
    }
});
