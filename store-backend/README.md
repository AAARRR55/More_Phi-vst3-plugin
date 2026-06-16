# More-Phi Store Backend

Production-ready purchasing and licensing backend for the More-Phi VST3/AU plugin.

## Stack

- **Runtime:** Node.js 20+ + TypeScript
- **Framework:** Fastify 4.x
- **Database:** PostgreSQL 15+ with Prisma ORM
- **Payments:** Stripe Checkout
- **Auth:** bcrypt + JWT (access + refresh tokens)
- **Validation:** Zod
- **Testing:** Vitest

## Quick Start

```bash
cd store-backend
cp .env.example .env
# Edit .env with your Stripe keys and secrets
npm install
npx prisma migrate dev
npx prisma db seed
npm run dev
```

The server will be available at `http://localhost:4000`.

## Environment Variables

See `.env.example` for the full list. Required variables include:

| Variable | Description |
|----------|-------------|
| `DATABASE_URL` | PostgreSQL connection string |
| `JWT_SECRET` | 32+ byte secret for JWT signing |
| `STRIPE_SECRET_KEY` | Stripe secret key |
| `STRIPE_WEBHOOK_SECRET` | Stripe webhook endpoint secret |
| `STRIPE_SUCCESS_URL` / `STRIPE_CANCEL_URL` | Checkout redirect URLs |
| `CORS_ORIGIN` | Allowed frontend origin(s) |
| `LICENSE_SIGNING_PRIVATE_KEY` | Ed25519 PEM private key for license certificates |
| `EMAIL_PROVIDER` | `console`, `resend`, or `smtp` |

### Generating an Ed25519 signing key

```bash
node -e "const { generateKeyPairSync } = require('crypto'); const { privateKey } = generateKeyPairSync('ed25519', { privateKeyEncoding: { type: 'pkcs8', format: 'pem' } }); console.log(privateKey.replace(/\n/g, '\\\\n'));"
```

## API Reference

### Products

#### `GET /v1/products/:slug`

Public. Returns product metadata and pricing.

**Response:**
```json
{
  "slug": "more-phi",
  "name": "More-Phi",
  "description": "Plugin-morphing VST3/AU host...",
  "priceCents": 7900,
  "currency": "usd",
  "version": "3.3.0",
  "maxActivations": 3,
  "licensingTerms": "One perpetual license per purchase; valid on up to 3 machines."
}
```

### Customers

#### `POST /v1/customers`

Register a new customer account.

**Request:**
```json
{
  "email": "jane@example.com",
  "password": "Password123!",
  "name": "Jane Doe",
  "country": "US"
}
```

**Response:** `201 Created`
```json
{
  "id": "cuid",
  "email": "jane@example.com"
}
```

### Auth

#### `POST /v1/auth/login`

**Request:**
```json
{
  "email": "jane@example.com",
  "password": "Password123!"
}
```

**Response:**
```json
{
  "accessToken": "eyJ...",
  "customer": {
    "id": "cuid",
    "email": "jane@example.com"
  }
}
```

Sets `mp_auth` and `mp_refresh` httpOnly cookies.

#### `POST /v1/auth/refresh`

Refreshes access token using the `mp_refresh` cookie.

#### `POST /v1/auth/logout`

Revokes refresh token and clears cookies.

### Checkout

#### `POST /v1/checkout`

Creates a Stripe Checkout Session. Supports guest checkout.

**Request:**
```json
{
  "productSlug": "more-phi",
  "email": "jane@example.com"
}
```

**Response:**
```json
{
  "checkoutUrl": "https://checkout.stripe.com/c/...",
  "orderId": "cuid"
}
```

### Webhooks

#### `POST /v1/webhooks/stripe`

Stripe webhook endpoint. Configure in the Stripe Dashboard.

Handled event types:
- `checkout.session.completed` — fulfill order, generate license, email customer.
- `checkout.session.async_payment_failed` / `payment_intent.payment_failed` — mark order failed.
- `charge.refunded` — revoke license and deactivate all activations.

### Licenses (Plugin-facing)

#### `POST /v1/licenses/activations`

Activate a license on a machine.

**Request:**
```json
{
  "licenseKey": "MP-7F3K-9QH2-XR4M-8LNT",
  "machineFingerprint": "<opaque plugin-computed string>",
  "hostname": "STUDIO-PC",
  "os": "win32",
  "appVersion": "3.3.0"
}
```

**Response:**
```json
{
  "status": "ACTIVE",
  "activationId": "cuid",
  "licenseId": "cuid",
  "activationsUsed": 1,
  "maxActivations": 3,
  "certificate": {
    "payload": "base64url(canonical-json)",
    "signature": "base64url(ed25519-signature)",
    "keyId": "prod-ed25519-2026-01"
  }
}
```

#### `POST /v1/licenses/activations/refresh`

Refresh an existing activation and receive a renewed certificate.

**Request:**
```json
{
  "activationId": "cuid",
  "machineFingerprint": "<opaque plugin-computed string>"
}
```

#### `POST /v1/licenses/activations/deactivate`

Release an activation slot.

**Request:**
```json
{
  "licenseKey": "MP-7F3K-9QH2-XR4M-8LNT",
  "machineFingerprint": "<opaque plugin-computed string>"
}
```

#### `GET /v1/licenses/verify?key=MP-...`

Lightweight license status check.

**Response:**
```json
{
  "valid": true,
  "status": "ACTIVE",
  "revokedAt": null
}
```

### Me

#### `GET /v1/me`

Returns the authenticated customer's profile, orders, and licenses.

### Health

#### `GET /health`

Liveness/readiness probe.

## Testing

Tests use a dedicated PostgreSQL database configured in `.env.test`.

```bash
# Create the test database
createdb morephi_store_test

# Run tests
npm test
```

For local Stripe webhook testing:

```bash
stripe login
stripe listen --forward-to http://localhost:4000/v1/webhooks/stripe
```

## Deployment

### Docker Compose

```bash
cp .env.example .env
# Edit .env
docker compose up --build
```

### Render / Railway / Fly.io

1. Set all environment variables in the platform dashboard.
2. Use `npm run db:deploy` to run migrations during deploy.
3. Run `npx prisma db seed` once to seed the product.
4. Configure Stripe webhooks to point to `https://your-domain/v1/webhooks/stripe`.

## Security Notes

- Card data never touches this server; Stripe Checkout keeps PCI scope to SAQ-A.
- Stripe webhook signatures are verified on the raw request body.
- License keys are looked up by SHA-256 hash, never by raw key in queries.
- Machine fingerprints are stored only as SHA-256 hashes.
- Passwords are hashed with bcrypt (cost 12 by default).
- JWT access tokens are short-lived (15 min) with rotating refresh tokens.
- Rate limiting is enforced on auth, checkout, and activation endpoints.

## License Model

- One-time purchase, perpetual license.
- Each license supports up to 3 machine activations (configurable per product).
- Re-activating the same machine is idempotent and does not consume a seat.
- Refunds revoke the license and deactivate all machines.
- Activations receive an Ed25519-signed certificate for offline validation with a 30-day online check window and 14-day grace period.
