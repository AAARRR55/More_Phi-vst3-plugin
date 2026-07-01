#!/usr/bin/env python3
"""
Fully Automated Dataset Generator for MorePhi with Hosted Plugin
Crash-resistant with automatic retry and status monitoring

See SYNTHETIC_AUDIO_PARAMETER_DATASET_FRAMEWORK.md for the complete
dataset generation framework documentation.
"""
import socket
import json
import time
import os
import sys
from datetime import datetime

# ============================================================================
# CONFIGURATION
# ============================================================================
# SECURITY FIX: Credentials are now loaded from environment variables or config file.
# Do NOT hardcode credentials in this file.
#
# Set environment variables:
#   MORE_PHI_TOKEN=<your_bearer_token>
#   MORE_PHI_PORT=30001
#
# Or create a .env file in the same directory (add .env to .gitignore!)

import os

def load_config():
    """Load configuration from environment variables or .env file."""
    # Try to load from .env file if it exists
    env_file = os.path.join(os.path.dirname(__file__), '.env')
    if os.path.exists(env_file):
        with open(env_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#') and '=' in line:
                    key, value = line.split('=', 1)
                    os.environ.setdefault(key.strip(), value.strip())
    
    token = os.environ.get('MORE_PHI_TOKEN', '')
    port = int(os.environ.get('MORE_PHI_PORT', '30001'))
    
    return token, port

BEARER_TOKEN, PORT = load_config()

# Dataset configurations (conservative to prevent crashes)
DATASETS = [
    {"name": "Batch_001_Safe", "samples": 10, "duration": 0.5},
    {"name": "Batch_002_Safe", "samples": 10, "duration": 0.5},
    {"name": "Batch_003_Safe", "samples": 10, "duration": 0.5},
    {"name": "Batch_004_Safe", "samples": 10, "duration": 0.5},
    {"name": "Batch_005_Safe", "samples": 10, "duration": 0.5},
]

OUTPUT_BASE = "C:/MorePhi_Datasets"

# ============================================================================

class MorePhiMCP:
    def __init__(self, port, token):
        self.port = port
        self.token = token.strip()
        self.req_id = 1
        self.sock = None
        self.authenticated = False
        
    def connect(self, timeout=10):
        """Connect to MorePhi MCP"""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(('127.0.0.1', self.port))
            return True
        except Exception as e:
            print(f"[ERROR] Connection failed: {e}")
            return False
    
    def authenticate(self):
        """Authenticate with bearer token"""
        try:
            req = {
                "jsonrpc": "2.0",
                "method": "initialize",
                "params": {"bearer_token": self.token},
                "id": self.req_id
            }
            self.req_id += 1
            
            self.sock.sendall((json.dumps(req) + "\n").encode())
            resp = self.sock.recv(4096).decode().strip()
            data = json.loads(resp.split('\n')[0])
            
            if "result" in data:
                self.authenticated = True
                return True
            else:
                print(f"[ERROR] Auth failed: {data.get('error',{}).get('message','Unknown')}")
                return False
        except Exception as e:
            print(f"[ERROR] Auth error: {e}")
            return False
    
    def get_plugin_info(self):
        """Get hosted plugin information"""
        try:
            req = {
                "jsonrpc": "2.0",
                "method": "get_plugin_info",
                "params": {},
                "id": self.req_id
            }
            self.req_id += 1
            
            self.sock.sendall((json.dumps(req) + "\n").encode())
            resp = self.sock.recv(4096).decode().strip()
            data = json.loads(resp.split('\n')[0])
            
            return data.get("result", {})
        except:
            return {}
    
    def generate_dataset(self, name, samples, duration):
        """Request dataset generation with crash prevention"""
        output_path = f"{OUTPUT_BASE}/{name}"
        os.makedirs(output_path, exist_ok=True)
        
        req = {
            "jsonrpc": "2.0",
            "method": "generate_dataset",
            "params": {
                "pipeline": "v3",
                "samples": samples,
                "duration": duration,
                "output_path": output_path,
                "respect_sanity": True,
                "allow_unsafe_live_dataset": True  # Required for hosted plugin
            },
            "id": self.req_id
        }
        self.req_id += 1
        
        try:
            print(f"  Sending request...")
            self.sock.sendall((json.dumps(req) + "\n").encode())
            
            # Short timeout - don't wait for completion
            self.sock.settimeout(3)
            try:
                resp = self.sock.recv(4096).decode().strip()
                if resp:
                    data = json.loads(resp.split('\n')[0])
                    if "result" in data:
                        return True, output_path
                    elif "error" in data:
                        error_msg = data['error'].get('message', 'Unknown error')
                        if "already in progress" in error_msg.lower():
                            return "wait", output_path
                        return False, error_msg
            except socket.timeout:
                # Timeout = request accepted, processing started
                return True, output_path
                
        except Exception as e:
            return False, str(e)
    
    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except:
                pass

def check_dataset_status(name):
    """Check if dataset has generated files"""
    path = f"{OUTPUT_BASE}/{name}"
    if not os.path.exists(path):
        return "not_started", 0
    
    # Count audio files
    audio_count = 0
    audio_path = os.path.join(path, "audio")
    if os.path.exists(audio_path):
        for root, dirs, files in os.walk(audio_path):
            audio_count += len([f for f in files if f.endswith('.wav')])
    
    if audio_count > 0:
        return "generating", audio_count
    
    # Check for metadata
    meta_path = os.path.join(path, "metadata")
    if os.path.exists(meta_path) and os.listdir(meta_path):
        return "initializing", 0
    
    return "started", 0

def generate_all_datasets():
    """Main generation loop with crash prevention"""
    print("=" * 70)
    print("AUTOMATED DATASET GENERATOR - Hosted Plugin Edition")
    print("=" * 70)
    print()
    print(f"Token: {BEARER_TOKEN[:30]}...")
    print(f"Port: {PORT}")
    print(f"Datasets to create: {len(DATASETS)}")
    print(f"Total samples: {sum(d['samples'] for d in DATASETS)}")
    print()
    
    # Test connection first
    print("Testing MorePhi connection...")
    mcp = MorePhiMCP(PORT, BEARER_TOKEN)
    
    if not mcp.connect():
        print("[FAIL] Cannot connect to MorePhi")
        print("Make sure FL Studio is running with MorePhi loaded")
        return
    
    if not mcp.authenticate():
        print("[FAIL] Authentication failed")
        print("Token is invalid - copy new one from AI Status panel")
        mcp.close()
        return
    
    print("[OK] Connected and authenticated")
    print()
    
    # Get plugin info
    print("Checking hosted plugin...")
    plugin_info = mcp.get_plugin_info()
    if plugin_info:
        plugin_name = plugin_info.get('name', 'Unknown')
        print(f"[OK] Hosted plugin: {plugin_name}")
    else:
        print("[WARN] No hosted plugin info available")
    
    mcp.close()
    print()
    
    # Create datasets one by one with delays
    results = []
    
    for i, ds in enumerate(DATASETS, 1):
        print(f"[{i}/{len(DATASETS)}] Creating {ds['name']}...")
        print(f"       Samples: {ds['samples']}, Duration: {ds['duration']}s")
        
        # Try to generate with retries
        max_retries = 3
        for attempt in range(max_retries):
            mcp = MorePhiMCP(PORT, BEARER_TOKEN)
            
            if not mcp.connect():
                print(f"       [Attempt {attempt+1}] Connection failed, waiting...")
                time.sleep(5)
                continue
            
            if not mcp.authenticate():
                print(f"       [Attempt {attempt+1}] Auth failed, retrying...")
                mcp.close()
                time.sleep(2)
                continue
            
            success, msg = mcp.generate_dataset(
                ds['name'], 
                ds['samples'], 
                ds['duration']
            )
            
            mcp.close()
            
            if success == True:
                print(f"       [OK] Dataset started!")
                results.append({"name": ds['name'], "status": "started", "output": msg})
                break
            elif success == "wait":
                print(f"       [WAIT] Another generation in progress, waiting 30s...")
                time.sleep(30)
                continue
            else:
                print(f"       [Attempt {attempt+1}] Failed: {msg}")
                if attempt < max_retries - 1:
                    time.sleep(5)
                else:
                    print(f"       [FAIL] Max retries reached")
                    results.append({"name": ds['name'], "status": "failed", "error": msg})
        
        # Long delay between datasets to prevent crashes
        if i < len(DATASETS):
            print(f"       Waiting 15 seconds before next dataset...")
            time.sleep(15)
        
        print()
    
    # Final summary
    print("=" * 70)
    print("GENERATION SUMMARY")
    print("=" * 70)
    print()
    
    started = [r for r in results if r['status'] == 'started']
    failed = [r for r in results if r['status'] == 'failed']
    
    print(f"Successfully started: {len(started)}/{len(DATASETS)}")
    for r in started:
        print(f"  [OK] {r['name']}")
        status, count = check_dataset_status(r['name'])
        print(f"    Status: {status}, Audio files: {count}")
    
    if failed:
        print()
        print(f"Failed: {len(failed)}")
        for r in failed:
            print(f"  ✗ {r['name']}: {r.get('error', 'Unknown error')}")
    
    print()
    print("All datasets are being generated in the background.")
    print(f"Check {OUTPUT_BASE}/ for progress.")
    print()
    print("IMPORTANT:")
    print("- Do NOT close FL Studio until all generations complete")
    print("- Check audio folder for .wav files")
    print("- Each batch takes ~5-10 minutes")

def monitor_progress():
    """Monitor all dataset generation progress"""
    print("\nMonitoring dataset progress...")
    print("(Press Ctrl+C to stop monitoring)")
    print()
    
    try:
        while True:
            print(f"\n[{datetime.now().strftime('%H:%M:%S')}]")
            for ds in DATASETS:
                status, count = check_dataset_status(ds['name'])
                print(f"  {ds['name']}: {status} ({count} audio files)")
            
            time.sleep(10)
    except KeyboardInterrupt:
        print("\n\nMonitoring stopped.")

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) > 1 and sys.argv[1] == "monitor":
        monitor_progress()
    else:
        # Validate token
        if not BEARER_TOKEN:
            print("[!] ERROR: No bearer token configured!")
            print("    Please set MORE_PHI_TOKEN environment variable or create a .env file")
            print()
            print("    Example .env file:")
            print("    MORE_PHI_TOKEN=your_token_here")
            print("    MORE_PHI_PORT=30001")
            print()
            sys.exit(1)
        
        # SECURITY FIX: Removed hardcoded placeholder check
        # Token is now loaded securely from environment/config
        
        generate_all_datasets()
        print()
        print("Run 'python auto_dataset_generator.py monitor' to watch progress")
