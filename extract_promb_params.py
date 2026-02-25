#!/usr/bin/env python3
import socket
import json
import time

TOKEN = 'da 44 c7 96 84 b8 dc 5b 8f c6 97 b8 70 b9 e2 c2'

s = socket.socket()
s.connect(('127.0.0.1', 30002))

# Initialize
init_msg = json.dumps({'jsonrpc': '2.0', 'method': 'initialize', 'params': {'bearer_token': TOKEN}, 'id': 1})
s.sendall(init_msg.encode() + b'\n')
time.sleep(0.2)

# List parameters
list_msg = json.dumps({'jsonrpc': '2.0', 'method': 'list_parameters', 'params': {}, 'id': 2})
s.sendall(list_msg.encode() + b'\n')
time.sleep(0.5)

# Receive all data
data = b''
while True:
    part = s.recv(65536)
    if not part:
        break
    data += part
    if len(part) < 65536:
        break

# Parse and save
response = data.decode()
print(response)

# Save to file
with open('promb_params.json', 'w') as f:
    f.write(response)

print("\n\nSaved to promb_params.json")
