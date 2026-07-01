import json
import numpy as np
import librosa
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
import os

class MorePhiDataset(Dataset):
    def __init__(self, audio_path, metadata_path, sample_rate=44100, clip_duration=1.0):
        with open(metadata_path, 'r') as f:
            self.metadata = json.load(f)

        self.sample_rate = sample_rate
        self.clip_samples = int(clip_duration * sample_rate)

        self.features_key = None
        if self.metadata and isinstance(self.metadata[0], dict):
            if 'features_mfcc' in self.metadata[0]:
                self.features_key = 'features_mfcc'
            elif 'audio_features' in self.metadata[0]:
                self.features_key = 'audio_features'

        self.use_precomputed_features = self.features_key is not None

        if self.use_precomputed_features:
            self.audio = None
        else:
            if not audio_path or not os.path.exists(audio_path):
                raise ValueError("Audio file not found and no precomputed features in metadata.")
            self.audio, _ = librosa.load(audio_path, sr=sample_rate)
        
    def __len__(self):
        return len(self.metadata)
    
    def __getitem__(self, idx):
        sample = self.metadata[idx]

        if self.use_precomputed_features:
            features = np.array(sample[self.features_key], dtype=np.float32)
        else:
            start = idx * self.clip_samples
            end = start + self.clip_samples
            audio_clip = self.audio[start:end]
            if len(audio_clip) < self.clip_samples:
                audio_clip = np.pad(audio_clip, (0, self.clip_samples - len(audio_clip)))

            # Feature Extraction: MFCC
            mfccs = librosa.feature.mfcc(y=audio_clip, sr=self.sample_rate, n_mfcc=13)
            features = np.mean(mfccs, axis=1).astype(np.float32)

        # Labels: Parameter Vector
        params = sample.get('parameters')
        if isinstance(params, dict):
            raise ValueError("Metadata 'parameters' must be a numeric vector, not a dict.")
        params = np.array(params, dtype=np.float32)

        return torch.tensor(features, dtype=torch.float32), torch.tensor(params, dtype=torch.float32)

class ParameterPredictor(nn.Module):
    def __init__(self, input_size, output_size):
        super(ParameterPredictor, self).__init__()
        self.model = nn.Sequential(
            nn.Linear(input_size, 128),
            nn.ReLU(),
            nn.Dropout(0.2),
            nn.Linear(128, 256),
            nn.ReLU(),
            nn.Linear(256, output_size),
            nn.Sigmoid() # Parameters are 0-1
        )
        
    def forward(self, x):
        return self.model(x)

def train_model(dataset_dir):
    audio_path = os.path.join(dataset_dir, "dataset_audio.wav")
    metadata_path = os.path.join(dataset_dir, "dataset_metadata.json")

    if not os.path.exists(audio_path):
        audio_path = None

    dataset = MorePhiDataset(audio_path, metadata_path)
    dataloader = DataLoader(dataset, batch_size=16, shuffle=True)
    
    # Get vector sizes from first sample
    sample_feat, sample_param = dataset[0]
    model = ParameterPredictor(len(sample_feat), len(sample_param))
    
    criterion = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=0.001)
    
    print(f"Starting training on {len(dataset)} samples...")
    for epoch in range(50):
        total_loss = 0
        for features, labels in dataloader:
            optimizer.zero_grad()
            outputs = model(features)
            loss = criterion(outputs, labels)
            loss.backward()
            optimizer.step()
            total_loss += loss.item()
        
        if epoch % 10 == 0:
            print(f"Epoch {epoch}, Loss: {total_loss/len(dataloader):.4f}")
    
    torch.save(model.state_dict(), "morephi_model.pth")
    print("Training Complete. Model saved as morephi_model.pth")

if __name__ == "__main__":
    # Point this to your generated folder
    # train_model("c:/Users/HP/morphy/_bmad-output/generated_dataset_XXXXXX")
    pass
