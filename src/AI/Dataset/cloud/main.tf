provider "aws" {
  region = "us-east-1"
}

# ==========================================
# 1. S3 Buckets (Input and Output)
# ==========================================
resource "aws_s3_bucket" "input_bucket" {
  bucket = "morphsnap-ml-training-input"
}

resource "aws_s3_bucket" "output_bucket" {
  bucket = "morphsnap-ml-training-output"
}

# ==========================================
# 2. SQS Queue for Asynchronous Jobs
# ==========================================
resource "aws_sqs_queue" "training_queue" {
  name                       = "morphsnap-training-queue"
  visibility_timeout_seconds = 3600 # 1 hour max processing time
}

# Configure S3 to send an event to SQS when a new metadata file is uploaded
resource "aws_s3_bucket_notification" "bucket_notification" {
  bucket = aws_s3_bucket.input_bucket.id

  queue {
    queue_arn     = aws_sqs_queue.training_queue.arn
    events        = ["s3:ObjectCreated:*"]
    filter_suffix = ".json"
  }
}

# Policy to allow S3 to write to SQS
resource "aws_sqs_queue_policy" "sqs_policy" {
  queue_url = aws_sqs_queue.training_queue.id

  policy = <<POLICY
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": "*",
      "Action": "sqs:SendMessage",
      "Resource": "${aws_sqs_queue.training_queue.arn}",
      "Condition": {
        "ArnEquals": { "aws:SourceArn": "${aws_s3_bucket.input_bucket.arn}" }
      }
    }
  ]
}
POLICY
}

# ==========================================
# 3. IAM Roles for the ECS Task
# ==========================================
resource "aws_iam_role" "ecs_task_execution_role" {
  name = "ecsTaskExecutionRoleMorphSnap"

  assume_role_policy = <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Action": "sts:AssumeRole",
      "Principal": {
        "Service": "ecs-tasks.amazonaws.com"
      },
      "Effect": "Allow",
      "Sid": ""
    }
  ]
}
EOF
}

resource "aws_iam_role_policy_attachment" "ecs_task_execution_role_policy" {
  role       = aws_iam_role.ecs_task_execution_role.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy"
}

# Custom policy so the worker container can read from S3 input, write to S3 output, and poll SQS
resource "aws_iam_policy" "worker_policy" {
  name = "MorphSnapWorkerPolicy"
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Effect = "Allow"
        Action = ["s3:GetObject"]
        Resource = ["${aws_s3_bucket.input_bucket.arn}/*"]
      },
      {
        Effect = "Allow"
        Action = ["s3:PutObject"]
        Resource = ["${aws_s3_bucket.output_bucket.arn}/*"]
      },
      {
        Effect = "Allow"
        Action = ["sqs:ReceiveMessage", "sqs:DeleteMessage", "sqs:GetQueueAttributes"]
        Resource = [aws_sqs_queue.training_queue.arn]
      }
    ]
  })
}

resource "aws_iam_role_policy_attachment" "worker_policy_attachment" {
  role       = aws_iam_role.ecs_task_execution_role.name # Usually you split task role and execution role, combined here for simplicity
  policy_arn = aws_iam_policy.worker_policy.arn
}

# ==========================================
# 4. Amazon ECS Cluster & Task Definition
# ==========================================
resource "aws_ecs_cluster" "ml_cluster" {
  name = "morphsnap-ml-cluster"
}

resource "aws_ecs_task_definition" "ml_worker" {
  family                   = "morphsnap-training-worker"
  network_mode             = "awsvpc"
  requires_compatibilities = ["FARGATE"]
  cpu                      = "1024" # 1 vCPU
  memory                   = "4096" # 4 GB RAM - adjust as needed
  execution_role_arn       = aws_iam_role.ecs_task_execution_role.arn
  task_role_arn            = aws_iam_role.ecs_task_execution_role.arn

  container_definitions = jsonencode([
    {
      name      = "ml-worker"
      image     = "YOUR_ECR_REGISTRY_URL/morphsnap-ml-worker:latest" # Update after docker push
      essential = true
      environment = [
        { name = "SQS_QUEUE_URL", value = aws_sqs_queue.training_queue.url },
        { name = "S3_INPUT_BUCKET", value = aws_s3_bucket.input_bucket.bucket },
        { name = "S3_OUTPUT_BUCKET", value = aws_s3_bucket.output_bucket.bucket },
        { name = "TRAINING_EPOCHS", value = "50" }
      ]
      logConfiguration = {
        logDriver = "awslogs"
        options = {
          "awslogs-group"         = "/ecs/morphsnap-ml-worker"
          "awslogs-region"        = "us-east-1"
          "awslogs-stream-prefix" = "ecs"
        }
      }
    }
  ])
}

# ECS Service to run 1 instance of the worker constantly
resource "aws_ecs_service" "worker_service" {
  name            = "morphsnap-worker-service"
  cluster         = aws_ecs_cluster.ml_cluster.id
  task_definition = aws_ecs_task_definition.ml_worker.arn
  desired_count   = 1
  launch_type     = "FARGATE"

  network_configuration {
    # Requires a VPC setup (simplifying by assuming default subnets/security groups exist)
    subnets          = ["subnet-xxxxxxx"] 
    security_groups  = ["sg-xxxxxxx"]
    assign_public_ip = true
  }
}
