import os
import json
import time
import uuid
import boto3
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
import numpy as np

# =====================================================================
# Model Architecture (identical to local)
# =====================================================================
class MorePhiDataset(Dataset):
    def __init__(self, metadata_path):
        with open(metadata_path, 'r') as f:
            self.metadata = json.load(f)
            
    def __len__(self):
        return len(self.metadata)
    
    def __getitem__(self, idx):
        sample = self.metadata[idx]
        
        # Audio feature vector (e.g. MFCCs extracted locally and sent to cloud)
        features_key = 'features_mfcc' if 'features_mfcc' in sample else 'audio_features'
        features = np.array(sample[features_key], dtype=np.float32)
        
        # Labels: Parameter Vector that caused that audio change
        params = np.array(sample['parameters'], dtype=np.float32)
        
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

# =====================================================================
# AWS SQS & S3 Worker Engine
# =====================================================================
def get_aws_clients():
    region = os.getenv("AWS_REGION", "us-east-1")
    s3 = boto3.client('s3', region_name=region)
    sqs = boto3.client('sqs', region_name=region)
    return s3, sqs

def process_training_job(s3, message, input_bucket, output_bucket):
    try:
        # Parse job details from SQS message body
        body = json.loads(message['Body'])
        job_id = body.get('job_id', str(uuid.uuid4()))
        metadata_key = body.get('s3_metadata_key') # e.g., 'uploads/fl_studio_training_data.json'
        
        if not metadata_key:
            raise Exception("No S3 metadata key provided in the message.")

        print(f"[{job_id}] Starting training job for: s3://{input_bucket}/{metadata_key}")

        # 1. Download file from S3 to local container TMP
        local_json_path = f"/tmp/{job_id}_metadata.json"
        s3.download_file(input_bucket, metadata_key, local_json_path)

        # 2. Setup Dataset & Dataloader
        dataset = MorePhiDataset(local_json_path)
        dataloader = DataLoader(dataset, batch_size=16, shuffle=True)
        
        # Determine tensor sizes from the first index dynamically
        sample_feat, sample_param = dataset[0]
        model = ParameterPredictor(len(sample_feat), len(sample_param))
        
        criterion = nn.MSELoss()
        optimizer = optim.Adam(model.parameters(), lr=0.001)

        # 3. Train Model
        epochs = int(os.getenv("TRAINING_EPOCHS", "50"))
        print(f"[{job_id}] Training for {epochs} epochs on {len(dataset)} samples...")
        
        for epoch in range(epochs):
            total_loss = 0
            for features, labels in dataloader:
                optimizer.zero_grad()
                outputs = model(features)
                loss = criterion(outputs, labels)
                loss.backward()
                optimizer.step()
                total_loss += loss.item()
            
            if epoch % 10 == 0:
                print(f"[{job_id}] Epoch {epoch}, Loss: {total_loss/len(dataloader):.4f}")

        # 4. Save locally
        local_model_path = f"/tmp/{job_id}_morephi_model.pth"
        torch.save(model.state_dict(), local_model_path)

        # 5. Upload trained model back to output bucket
        output_key = f"models/{job_id}/morephi_model.pth"
        s3.upload_file(local_model_path, output_bucket, output_key)
        
        # Cleanup
        os.remove(local_json_path)
        os.remove(local_model_path)

        print(f"[{job_id}] SUCCESS! Uploaded to s3://{output_bucket}/{output_key}")
        return True

    except Exception as e:
        print(f"Error processing job: {e}")
        return False

def listen_to_queue():
    queue_url = os.getenv("SQS_QUEUE_URL")
    input_bucket = os.getenv("S3_INPUT_BUCKET")
    output_bucket = os.getenv("S3_OUTPUT_BUCKET")
    
    if not all([queue_url, input_bucket, output_bucket]):
        raise ValueError("Missing AWS Environment Variables (SQS_QUEUE_URL, S3_INPUT_BUCKET, S3_OUTPUT_BUCKET)")

    s3, sqs = get_aws_clients()
    print(f"Listening to SQS Queue: {queue_url}...")

    while True:
        # Long polling for messages
        response = sqs.receive_message(
            QueueUrl=queue_url,
            MaxNumberOfMessages=1,
            WaitTimeSeconds=20 # Max wait
        )

        messages = response.get('Messages', [])
        for msg in messages:
            success = process_training_job(s3, msg, input_bucket, output_bucket)
            if success:
                # Delete from queue on success so it doesn't process again
                sqs.delete_message(
                    QueueUrl=queue_url,
                    ReceiptHandle=msg['ReceiptHandle']
                )
            else:
                print(f"Job failed. Message stays in queue for redelivery.")
                
        time.sleep(1)

if __name__ == "__main__":
    # When deployed to ECS, this script runs forever listening to the SQS queue.
    listen_to_queue()
