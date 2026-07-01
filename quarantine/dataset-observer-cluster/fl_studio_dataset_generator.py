import socket
import json
import time
import random
import os
import sys
import sounddevice as sd
import numpy as np
import librosa

# SECURITY FIX: Token loaded from environment variable, not hardcoded.
# Set MORE_PHI_TOKEN env var or create a .env file.
TOKEN = os.environ.get("MORE_PHI_TOKEN", "").strip()
PORT = int(os.environ.get("MORE_PHI_PORT", "30001"))

# =====================================================================
# FabFilter Pro-Q 4 Parameter Map — Safe Guardrails
# =====================================================================
# Pro-Q 4 has 24 bands × 24 params each = 576 band params, then globals.
# Each band's 24 parameters follow the same layout at offset (band-1)*24:
#   +0  Used              (binary)  — controls if band exists
#   +1  Enabled           (binary)  — enables band processing
#   +2  Frequency         (continuous 0-1, maps ~10Hz–30kHz)
#   +3  Gain              (continuous 0-1, maps ~ -30dB to +30dB)
#   +4  Q                 (continuous 0-1, maps ~0.025–40)
#   +5  Shape             (discrete, filter type: Bell/LowShelf/HighShelf/etc.)
#   +6  Slope             (discrete, filter slope: 6/12/24/48 dB/oct etc.)
#   +7  Stereo Placement  (settings)
#   +8  Speakers          (settings)
#   +9  Dynamic Range     (continuous)
#   +10 Dynamics Enabled  (binary)
#   +11 Dynamics Auto     (binary)
#   +12 Threshold         (continuous)
#   +13 Attack            (continuous)
#   +14 Release           (continuous)
#   +15 External Side Chain (binary)
#   +16 Side Chain Filtering (binary)
#   +17 Side Chain Low Frequency
#   +18 Side Chain High Frequency
#   +19 Side Chain Audition (binary)
#   +20 Spectral Enabled  (binary)
#   +21 Spectral Density
#   +22 Solo              (binary)
#   +23 Spectral Tilt

PARAMS_PER_BAND = 24
NUM_BANDS = 24  # Pro-Q 4 supports up to 24 bands

# Offsets within each band that are SAFE to randomize (core DSP)
SAFE_OFFSETS = {
    2: "Frequency",   # Continuous — the core EQ frequency
    3: "Gain",        # Continuous — boost/cut amount
    4: "Q",           # Continuous — bandwidth
    5: "Shape",       # Discrete  — filter type (Bell=0, LowShelf, HighShelf, etc.)
    6: "Slope",       # Discrete  — filter slope (6/12/24/48 dB/oct)
}

# Offsets that MUST be pinned to specific values to avoid silence/bypass
PIN_VALUES = {
    0:  1.0,  # Used        — band MUST be active
    1:  1.0,  # Enabled     — band MUST be enabled
    10: 0.0,  # Dynamics Enabled — OFF (avoids gating silence)
    11: 0.0,  # Dynamics Auto    — OFF
    15: 0.0,  # External Side Chain — OFF
    16: 0.0,  # Side Chain Filtering — OFF
    19: 0.0,  # Side Chain Audition — OFF (would mute main output)
    20: 0.0,  # Spectral Enabled — OFF
    22: 0.0,  # Solo — OFF (would mute other bands)
}

# Global parameter indices that MUST be pinned
GLOBAL_PIN = {
    583: 0.0,  # Bypass             — MUST be OFF
    584: 0.0,  # Output Invert Phase — MUST be OFF
    600: 0.0,  # Host Bypass        — MUST be OFF
}

# Global params that are safe to leave at default (don't touch)
# 576: Processing Mode, 577: Processing Resolution, 578: Character
# 579: Gain Scale, 580: Output Level, 581: Output Pan, 585: Auto Gain
# 586+: Analyzer/UI/MIDI params — never touch

# Pro-Q 4 Shape values (normalized 0-1 for the discrete steps):
# The exact mapping depends on the parameter step count, but typical:
#   Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt
SHAPE_VALUES = [0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0]

# Pro-Q 4 Slope values (normalized):
# 6dB, 12dB, 18dB, 24dB, 30dB, 36dB, 48dB, 72dB, 96dB
SLOPE_VALUES = [0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0]

PARAM_COUNT = 737
DEFAULT_PARAM_VALUE = 0.5


def build_full_param_vector(params_array, param_count=PARAM_COUNT,
                            default_value=DEFAULT_PARAM_VALUE):
    """Convert sparse MCP parameter array into full parameter vector."""
    full_params = [default_value] * param_count
    for item in params_array:
        idx = item.get("index")
        val = item.get("value")
        if isinstance(idx, int) and 0 <= idx < param_count:
            full_params[idx] = float(val)
    return full_params


def build_safe_random_params(num_active_bands=4, rng=None):
    """
    Build a parameter set that only randomizes safe DSP parameters
    and pins all dangerous switches to safe values.

    Returns:
        params_array: list of {"index": int, "value": float} for MCP
        human_readable: dict mapping param names to values for metadata
    """
    if rng is None:
        rng = random.Random()

    params_array = []
    human_readable = {}

    # Clamp active bands to valid range
    num_active_bands = max(1, min(num_active_bands, NUM_BANDS))

    for band_idx in range(NUM_BANDS):
        base = band_idx * PARAMS_PER_BAND
        band_num = band_idx + 1

        if band_idx < num_active_bands:
            # --- Active band: pin switches, randomize DSP ---
            for offset, pin_val in PIN_VALUES.items():
                params_array.append({"index": base + offset, "value": pin_val})

            # Frequency: full range (0.0 = ~10Hz, 1.0 = ~30kHz)
            freq = rng.uniform(0.05, 0.95)
            params_array.append({"index": base + 2, "value": freq})
            human_readable[f"Band {band_num} Frequency"] = freq

            # Gain: limit to ±15dB range (0.25-0.75 in normalized, center=0.5=0dB)
            gain = rng.uniform(0.25, 0.75)
            params_array.append({"index": base + 3, "value": gain})
            human_readable[f"Band {band_num} Gain"] = gain

            # Q: moderate range to avoid extreme resonance
            q = rng.uniform(0.15, 0.85)
            params_array.append({"index": base + 4, "value": q})
            human_readable[f"Band {band_num} Q"] = q

            # Shape: pick a random filter type
            shape = rng.choice(SHAPE_VALUES)
            params_array.append({"index": base + 5, "value": shape})
            human_readable[f"Band {band_num} Shape"] = shape

            # Slope: pick a random slope
            slope = rng.choice(SLOPE_VALUES)
            params_array.append({"index": base + 6, "value": slope})
            human_readable[f"Band {band_num} Slope"] = slope

        else:
            # --- Inactive band: turn it off ---
            params_array.append({"index": base + 0, "value": 0.0})  # Used = OFF
            params_array.append({"index": base + 1, "value": 0.0})  # Enabled = OFF

    # Pin critical global parameters
    for idx, val in GLOBAL_PIN.items():
        params_array.append({"index": idx, "value": val})

    return params_array, human_readable


class MorePhiMCPClient:
    def __init__(self, port=None, token=None):
        self.port = port or PORT
        self.token = (token or TOKEN).strip()
        self.sock = None
        self.req_id = 1

        if not self.token:
            print("[!] ERROR: No bearer token configured!")
            print("    Set MORE_PHI_TOKEN env var or create a .env file.")
            sys.exit(1)

    def connect(self):
        """Connects to the MorePhi MCP Server running in FL Studio"""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.sock.connect(('127.0.0.1', self.port))
            print(f"Connected to MorePhi MCP server on port {self.port}.")

            resp = self.send_request("initialize", {"bearer_token": self.token})
            if resp and "result" in resp:
                print("Successfully authenticated with MorePhi.")
            else:
                err = resp.get("error", {}).get("message", "Unknown") if resp else "No response"
                print(f"Authentication FAILED: {err}")
                sys.exit(1)
        except ConnectionRefusedError:
            print("CONNECTION FAILED: Ensure FL Studio is open, MorePhi is loaded, and 'MCP: ON' is visible.")
            sys.exit(1)

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

        buffer = ""
        self.sock.settimeout(10)
        try:
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
        except socket.timeout:
            return {"error": "Timeout waiting for response"}

    def get_parameters(self):
        """Fetch all exposed parameters from the hosted plugin"""
        return self.send_request("list_parameters")

    def set_parameters(self, params_array):
        """
        Batch set parameters via MCP.
        params_array format: [{"index": 0, "value": 0.5}, ...]
        """
        return self.send_request("tools/call", {
            "name": "set_parameters_optimized",
            "arguments": {
                "parameters": params_array
            }
        })

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


# =====================================================================
# Audio Recording & Feature Extraction
# =====================================================================
def record_audio(duration_sec=1.5, sample_rate=44100):
    """
    Records audio via system loopback (Stereo Mix or WASAPI loopback).
    IMPORTANT: Your Windows default recording device must capture desktop audio.
    """
    print(f"  [Recording {duration_sec}s of audio...]")
    recording = sd.rec(int(duration_sec * sample_rate), samplerate=sample_rate,
                       channels=2, dtype='float32')
    sd.wait()
    return recording


def extract_features(audio_data, sample_rate=44100):
    """Extracts 13 MFCCs averaged across the duration."""
    mono_audio = np.mean(audio_data, axis=1)
    mfccs = librosa.feature.mfcc(y=mono_audio, sr=sample_rate, n_mfcc=13)
    return np.mean(mfccs, axis=1).tolist()


def check_audio_quality(audio_data):
    """Check if recorded audio is silent or clipping."""
    peak = np.max(np.abs(audio_data))
    rms = np.sqrt(np.mean(audio_data ** 2))

    if peak < 0.001:
        return "silent"
    if peak > 0.999:
        return "clipping"
    if rms < 0.0005:
        return "near_silent"
    return "ok"


# =====================================================================
# Main Generation Loop
# =====================================================================
def generate_dataset(num_samples=50, num_bands=4, output_file="dataset_manifest.json"):
    """
    Generate a dataset by randomizing ONLY safe Pro-Q 4 DSP parameters
    (Frequency, Gain, Q, Shape, Slope) while pinning all dangerous switches
    (Bypass, Solo, Mute, Phase Invert) to safe values.

    PREREQUISITES:
    1. FL Studio running with MorePhi loaded, hosting FabFilter Pro-Q 4
    2. Pink noise (or a full drum break) playing on a loop through the plugin
       - Generate pink noise: python generate_pink_noise.py
    3. Windows recording device set to Stereo Mix / WASAPI loopback
    4. MORE_PHI_TOKEN env var set to your bearer token
    """
    print("=" * 65)
    print("FabFilter Pro-Q 4 Dataset Generator — Safe Parameter Mode")
    print("=" * 65)
    print()
    print(f"  Samples to generate: {num_samples}")
    print(f"  Active EQ bands:     {num_bands}")
    print(f"  Output file:         {output_file}")
    print()
    print("  SAFETY: Only Frequency/Gain/Q/Shape/Slope are randomized.")
    print("  PINNED: Bypass=OFF, Solo=OFF, Mute=OFF, Phase=OFF")
    print()

    mcp = MorePhiMCPClient()
    mcp.connect()

    rng = random.Random()
    dataset = []
    silent_count = 0
    retry_limit = 3

    print("\n--- Starting Data Generation ---")
    print("    (Feed PINK NOISE through the plugin for best results!)")
    print()

    for i in range(num_samples):
        print(f"Sample {i+1}/{num_samples}")

        # Pick random number of active bands (2 to num_bands) for variety
        active_bands = rng.randint(max(1, num_bands // 2), num_bands)

        # Build safe parameter set
        params_array, human_readable = build_safe_random_params(
            num_active_bands=active_bands, rng=rng
        )

        # Apply parameters to the plugin
        print(f"  [Setting {active_bands} EQ bands with safe random values]")
        resp = mcp.set_parameters(params_array)
        if resp and "error" in resp:
            print(f"  [WARN] Parameter set error: {resp['error']}")

        # Wait for plugin to settle
        time.sleep(0.5)

        # Record audio with retry on silence
        audio = None
        quality = "silent"
        for attempt in range(retry_limit):
            audio = record_audio(duration_sec=1.5)
            quality = check_audio_quality(audio)
            if quality == "ok":
                break
            if quality == "silent" or quality == "near_silent":
                print(f"  [WARN] Audio is {quality} — attempt {attempt+1}/{retry_limit}")
                if attempt < retry_limit - 1:
                    time.sleep(1.0)  # Wait and retry

        if quality != "ok":
            print(f"  [SKIP] Audio quality: {quality} after {retry_limit} attempts")
            silent_count += 1
            continue

        # Extract features
        features = extract_features(audio)

        full_params = build_full_param_vector(params_array)

        dataset.append({
            "sample_id": i,
            "active_bands": active_bands,
            "parameters": full_params,
            "parameters_named": human_readable,
            "features_mfcc": features,
            "audio_features": features,
            "audio_quality": quality
        })
        print(f"  [OK] Captured (peak={np.max(np.abs(audio)):.3f})")

    mcp.close()

    # Save manifest
    with open(output_file, 'w') as f:
        json.dump(dataset, f, indent=4)

    # Summary
    print()
    print("=" * 65)
    print("GENERATION COMPLETE")
    print("=" * 65)
    print(f"  Captured: {len(dataset)}/{num_samples} samples")
    print(f"  Silent/skipped: {silent_count}")
    print(f"  Manifest: {output_file}")
    if silent_count > num_samples * 0.1:
        print()
        print("  [!] High silent rate — check that:")
        print("      1. Pink noise (not a test tone!) is playing in FL Studio")
        print("      2. FL Studio transport is running (play button)")
        print("      3. Windows recording device = Stereo Mix / loopback")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate EQ dataset with safe Pro-Q 4 parameter randomization"
    )
    parser.add_argument("--samples", type=int, default=50,
                        help="Number of samples to generate (default: 50)")
    parser.add_argument("--bands", type=int, default=4,
                        help="Max active EQ bands per sample (default: 4)")
    parser.add_argument("--output", type=str, default="dataset_manifest.json",
                        help="Output manifest file (default: dataset_manifest.json)")
    args = parser.parse_args()

    generate_dataset(
        num_samples=args.samples,
        num_bands=args.bands,
        output_file=args.output
    )
