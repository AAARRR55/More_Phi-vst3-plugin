# This script sets up the AWS resources using the AWS CLI and runs the python worker locally.

$ErrorActionPreference = "Stop"

$REGION = "us-east-1"
# AWS S3 Bucket names must be globally unique! We append a random string to ensure it works.
$RANDOM_SUFFIX = -join ((48..57) + (97..122) | Get-Random -Count 6 | % { [char]$_ })
$INPUT_BUCKET = "morphsnap-input-$RANDOM_SUFFIX"
$OUTPUT_BUCKET = "morphsnap-output-$RANDOM_SUFFIX"
$QUEUE_NAME = "morphsnap-training-queue-$RANDOM_SUFFIX"

Write-Host "=========================================="
Write-Host "  AWS Local Worker Setup Script"
Write-Host "=========================================="
Write-Host ""
Write-Host "1. Creating S3 Input Bucket: $INPUT_BUCKET"
aws s3 mb s3://$INPUT_BUCKET --region $REGION

Write-Host "2. Creating S3 Output Bucket: $OUTPUT_BUCKET"
aws s3 mb s3://$OUTPUT_BUCKET --region $REGION

Write-Host "3. Creating SQS Queue: $QUEUE_NAME"
$QUEUE_URL = aws sqs create-queue --queue-name $QUEUE_NAME --region $REGION --query 'QueueUrl' --output text

Write-Host ""
Write-Host "Configuration created successfully!"
Write-Host "Queue URL: $QUEUE_URL"

# We must set up the Event Notification so S3 tells SQS about new files.
# First, update the SQS policy to allow S3 to write to it.
$QUEUE_ARN = aws sqs get-queue-attributes --queue-url $QUEUE_URL --attribute-names QueueArn --query 'Attributes.QueueArn' --output text
$ACCOUNT_ID = (aws sts get-caller-identity --query 'Account' --output text)

$policy = @"
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": "*",
      "Action": "sqs:SendMessage",
      "Resource": "$QUEUE_ARN",
      "Condition": {
        "ArnLike": { "aws:SourceArn": "arn:aws:s3:::$INPUT_BUCKET" }
      }
    }
  ]
}
"@
# Write policy to temp file for aws cli to read
$policy | Out-File -FilePath "sqs_policy.json" -Encoding ASCII
aws sqs set-queue-attributes --queue-url $QUEUE_URL --attributes file://sqs_policy.json
Remove-Item "sqs_policy.json"

# Now tell the S3 bucket to trigger SQS
$notification = @"
{
  "QueueConfigurations": [
    {
      "QueueArn": "$QUEUE_ARN",
      "Events": ["s3:ObjectCreated:*"]
    }
  ]
}
"@
$notification | Out-File -FilePath "s3_notification.json" -Encoding ASCII
aws s3api put-bucket-notification-configuration --bucket $INPUT_BUCKET --notification-configuration file://s3_notification.json
Remove-Item "s3_notification.json"


Write-Host ""
Write-Host "=========================================="
Write-Host "Setup Complete! Starting Python Worker..."
Write-Host "=========================================="
Write-Host "The worker will listen to the queue. Open a new terminal and upload a JSON file to see it run:"
Write-Host "  > aws s3 cp dataset_metadata.json s3://$INPUT_BUCKET/"
Write-Host ""

# Set Environment Variables for the Python Script
$env:SQS_QUEUE_URL = $QUEUE_URL
$env:S3_INPUT_BUCKET = $INPUT_BUCKET
$env:S3_OUTPUT_BUCKET = $OUTPUT_BUCKET
$env:AWS_REGION = $REGION

# Install requirements quietly
pip install -r requirements.txt -q

# Run the worker!
python train_worker.py
