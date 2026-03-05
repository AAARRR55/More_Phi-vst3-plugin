import socket
import json
import time
import random
import os
import sounddevice as sd
import numpy as np
import librosa

class MorphSnapMCPClient:
    def __init__(self, port=30001, token="79 65 a0 4d 9f f0 70 ba 7d 3f 03 31 f2 0a 8d c1"):
        self.port = port
        self.token = token
        self.sock = None
        self.req_id = 1
        
    def connect(self):
        """Connects to the MorphSnap MCP Server running in FL Studio"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.sock.connect(('127.0.0.1', self.port))
            print(f"Connected to MorphSnap MCP server on port {self.port}.")
            
            # Send initialization packet
            resp = self.send_request("initialize", {"bearer_token": self.token})
            if resp:
                print("Successfully authenticated with MorphSnap.")
        except ConnectionRefusedError:
            print("CONNECTION FAILED: Ensure FL Studio is open, MorphSnap is loaded, and 'MCP: ON' is visible.")
            exit(1)

    def send_request(self, method, params=None):
        if params is None:
            params = {}
            
        req = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
            "id": self.req_id
        }
        self.req_id += 1
        
        message = json.dumps(req) + "\n"
        self.sock.sendall(message.encode('utf-8'))
        
        # Simple blocking receive loop
        buffer = ""
        while True:
            data = self.sock.recv(4096).decode('utf-8')
            if not data:
                break
            buffer += data
            if '\n' in buffer:
                line = buffer.split('\n')[0]
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    return {"error": "Invalid JSON response"}
                    
    def get_parameters(self):
        """Fetch all exposed parameters from the hosted plugin"""
        return self.send_request("list_parameters")
        
    def set_parameters(self, params_array):
        """
        Uses the extended MCP tool to batch set parameters without UI lag.
        params_array format: [{"index": 0, "value": 0.5}, {"index": 1, "value": 0.8}]
        """
        return self.send_request("tools/call", {
            "name": "set_parameters_optimized",
            "arguments": {
                "parameters": params_array
            }
        })
        
    def close(self):
        if self.sock:
            self.sock.close()

# =====================================================================
# Audio Recording & Feature Extraction
# =====================================================================
def record_audio(duration_sec=1.0, sample_rate=44100):
    """
    Records audio via system loopback 'Stereo Mix' or default recording device.
    Make sure your default Windows Recording Device captures desktop audio.
    """
    print(f"  [Recording {duration_sec}s of audio...]")
    recording = sd.rec(int(duration_sec * sample_rate), samplerate=sample_rate, channels=2, dtype='float32')
    sd.wait()
    return recording

def extract_features(audio_data, sample_rate=44100):
    """
    Extracts 13 MFCCs averaged across the duration.
    """
    print("  [Extracting MFCC features...]")
    # Librosa expects mono, so we mean across channels first (axis 1 if shape is samples, channels)
    # sounddevice returns shape: (frames, channels)
    mono_audio = np.mean(audio_data, axis=1)
    mfccs = librosa.feature.mfcc(y=mono_audio, sr=sample_rate, n_mfcc=13)
    return np.mean(mfccs, axis=1).tolist()

# =====================================================================
# Main Generation Loop
# =====================================================================
def generate_dataset(num_samples=50, output_file="dataset_manifest.json"):
    mcp = MorphSnapMCPClient()
    mcp.connect()
    
    dataset = []
    
    print("\n--- Starting Data Generation ---")
    for i in range(num_samples):
        print(f"\nSample {i+1}/{num_samples}")
        
        # 1. Randomize 5 parameters (indices 0 through 4 as an example)
        p0 = random.random()
        p1 = random.random()
        p2 = random.random()
        p3 = random.random()
        p4 = random.random()

        random_params = [
            {"index": 0, "value": p0},
            {"index": 1, "value": p1},
            {"index": 2, "value": p2},
            {"index": 3, "value": p3},
            {"index": 4, "value": p4}
        ]
        
        # 2. Tell MorphSnap in FL Studio to apply these values
        print(f"  [Setting plugin parameters to random values]")
        mcp.set_parameters(random_params)
        
        # Wait for the plugin to settle its audio buffer
        time.sleep(0.5)
        
        # 3. Capture audio output from FL Studio
        audio = record_audio(duration_sec=1.5)
        
        # 4. Extract data
        features = extract_features(audio)
        
        # 5. Append to local dataset in Colab format
        dataset.append({
            "sample_id": i,
            "parameters": {
                "Param0": p0,
                "Param1": p1,
                "Param2": p2,
                "Param3": p3,
                "Param4": p4
            },
            "audio_features": features
        })
        
    mcp.close()
    
    # Save the output file
    with open(output_file, 'w') as f:
        json.dump(dataset, f, indent=4)
        
    print(f"\nSuccess! Dataset generation complete. Metadata saved to {output_file}")
    print("Next step: Upload this file to your Google Drive 'MorphSnap' folder!")
    
if __name__ == "__main__":
    # Ensure FL Studio is playing a loop of dry audio before running this!
    # And make sure your PC's default recording device is "Stereo Mix" / Loopback
    generate_dataset(num_samples=50, output_file="dataset_manifest.json")

