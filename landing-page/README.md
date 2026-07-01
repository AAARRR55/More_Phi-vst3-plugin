# More-Phi VST3 Purchasing System

Complete e-commerce and licensing system for the More-Phi VST3/AU audio plugin.

## Stack

- Frontend: Next.js 16, React 19, Tailwind CSS
- Backend: FastAPI, MongoDB via Motor
- Payments: Stripe Checkout through PCI-safe hosted checkout
- Auth: JWT email/password customer accounts

## Core Features

- Product metadata and licensing terms endpoint
- Customer registration/login with hashed passwords and rate limiting
- Stripe Checkout one-time purchase flow for the $129 plugin license
- Stripe webhook handler with signature verification through the payment integration
- Idempotent license key generation after successful payment
- Customer dashboard for purchase history, license keys, and activations
- Payment transaction/event logs for reconciliation
- Database schema endpoint and human-readable API docs

## Environment

Backend: copy `backend/.env.example` to `backend/.env` and set:

```env
MONGO_URL=mongodb://localhost:27017
DB_NAME=more_phi_store
JWT_SECRET=replace-with-a-long-random-secret
STRIPE_API_KEY=sk_test_your_secret_key
STRIPE_PUBLISHABLE_KEY=pk_test_your_publishable_key
ACCESS_TOKEN_EXPIRE_MINUTES=10080
CORS_ALLOWED_ORIGINS=https://your-frontend-domain.example,http://localhost:3000
SMTP_HOST=smtp.your-provider.com
SMTP_PORT=587
SMTP_USERNAME=your-smtp-username
SMTP_PASSWORD=your-smtp-password
SMTP_FROM=licenses@your-domain.com
SMTP_USE_TLS=true
```

Frontend: copy `.env.example` to `.env.local` and set:

```env
NEXT_PUBLIC_API_URL=http://localhost:8001
```

## Local Setup

```bash
# backend
cd backend
pip install -r requirements.txt
uvicorn server:app --host 0.0.0.0 --port 8001

# frontend
cd /app
yarn install
yarn dev
```

## API Documentation

Open `/developer-docs` in the frontend for endpoint examples, or call:

- `GET /api/docs-text`
- `GET /api/schema`
- FastAPI OpenAPI docs at `/docs`

## Database Schema

MongoDB collections:

- `customers`: customer profile, email, hashed password, company, country
- `orders`: product purchase status, amount, currency, Stripe checkout session
- `licenses`: generated license keys tied to customer and order
- `payment_transactions`: session/payment status, metadata, retry count
- `payment_events`: Stripe webhook event log
- `license_activations`: activated machines/DAWs per license
- `email_events`: license delivery email integration hook records
- `rate_limits`: temporary request rate limit buckets

## Security Notes

- Raw card data never touches this application; customers pay on Stripe Checkout.
- Backend defines all prices server-side to prevent price manipulation.
- JWT protects dashboard, checkout, order, license, and activation endpoints.
- Login, registration, and checkout endpoints are rate limited.
- MongoDB ObjectIds are excluded from API responses.

## Payment Flow

1. Customer signs up or signs in.
2. Frontend sends only `origin_url` to `POST /api/payments/checkout`.
3. Backend creates a Stripe Checkout session using server-side product price.
4. Backend creates pending `orders` and `payment_transactions` records.
5. Customer completes payment on Stripe.
6. Frontend polls `/api/payments/checkout/status/{session_id}` after redirect.
7. Stripe webhook and/or polling confirms payment.
8. Backend provisions a license exactly once and stores a license delivery email hook.

## Email Delivery

Configure the SMTP variables above to send license keys automatically after successful Stripe confirmation. Every delivery attempt is also stored in `email_events` for audit and retry workflows.