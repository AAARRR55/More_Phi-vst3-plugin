const net = require("net");

const SOURCE_PORT = 8001;
const TARGET_PORT = 4000;
const TARGET_HOST = "127.0.0.1";

const server = net.createServer((clientSocket) => {
  const serverSocket = net.createConnection({ port: TARGET_PORT, host: TARGET_HOST });

  clientSocket.pipe(serverSocket);
  serverSocket.pipe(clientSocket);

  serverSocket.on("error", (err) => {
    console.error("Target connection error:", err.message);
    clientSocket.end();
  });

  clientSocket.on("error", (err) => {
    console.error("Client connection error:", err.message);
    serverSocket.end();
  });
});

server.listen(SOURCE_PORT, "0.0.0.0", () => {
  console.log(`Proxy listening on port ${SOURCE_PORT} -> ${TARGET_HOST}:${TARGET_PORT}`);
});
