# More-Phi Store — VST3 Purchasing System Design

- **Date:** 2026-06-16
- **Status:** Approved (design phase)
- **Product:** More-Phi VST3/AU plugin, v3.3.0
- **Scope:** Backend service that sells the plugin as a one-time purchase, issues machine-bound license keys, and validates activations.

## 1. Overview

A production-ready purchasing system for the More-Phi plugin. Customers buy the plugin through a Stripe-hosted checkout; on successful payment the server issues a license key and emails it. The plugin activates against a machine fingerprint, with a cap on simultaneous activations per license. Refunds revoke the license.

This is the **backend only**. The plugin-side fingerprint computation and activation client are documented as contracts here but implemented in C++ separately. The storefront SPA is out of scope (the API is frontend-agnostic).

### Scope is a single bounded context

One cohesive service: product catalog (one product, modeled for extensibility), customers, orders, licenses, activations, Stripe payments, and email delivery. No decomposition into sub-projects is required.

## 2. Foundational Decisions

Confirmed with the product owner:

| Decision | Choice | Rationale |
|---|---|---|
| Stack | **Node.js + TypeScript** (Fastify + Prisma + PostgreSQL) | Best-in-class Stripe SDK, type-safe end-to-end, large ecosystem, simple deploy |
| Location | **`store-backend/` subdirectory** in this repo | Keeps plugin + storefront together; backend has its own `package.json`/Dockerfile and never touches the C++ build |
| License model | **Online activation, machine-bound** | Server validates each activation against a machine fingerprint; supports a max-N-devices cap, deactivation, revocation |
| Payment surface | **Stripe Checkout (hosted page)** | Lowest PCI scope (SAQ-A), no card UI to build, Apple Pay/Google Pay automatic |

Additional micro-decisions made with rationale:

- **Fulfillment = webhook-driven, idempotent** (not polling). Real-time, Stripe-recommended.
- **Guest checkout with auto-account creation** — email comes from Stripe; a set-password link is emailed with the license. Better conversion than forcing signup first.
- **Validation = Zod** as the single source of truth for types + runtime validation, applied per route.

## 3. Architecture

Single monolithic Node.js service, cleanly layered. HTTP routes are thin; business logic lives in pure, testable services.

```
store-backend/
├── src/
│   ├── server.ts                 # Fastify bootstrap + lifecycle
│   ├── config/                   # typed, Zod-validated env loading
│   ├── plugins/                  # cors, helmet, rate-limit, auth(jwt), errorHandler, requestId
│   ├── routes/v1/
│   │   ├── products.routes.ts
│   │   ├── customers.routes.ts
│   │   ├── auth.routes.ts
│   │   ├── checkout.routes.ts
│   │   ├── webhooks.stripe.routes.ts
│   │   ├── licenses.routes.ts        # plugin-facing activation/verify
│   │   └── me.routes.ts
│   ├── services/                 # business logic (no HTTP imports)
│   │   ├── CustomerService.ts
│   │   ├── OrderService.ts
│   │   ├── LicenseService.ts       # key generation + status
│   │   ├── ActivationService.ts    # machine-bound activation, slot counting
│   │   ├── StripeService.ts        # checkout sessions
│   │   ├── WebhookService.ts       # idempotent fulfillment
│   │   └── EmailService.ts         # provider-abstracted
│   ├── db/                       # Prisma client singleton + transaction helpers
│   └── lib/                      # logger (pino), crypto (keygen), fingerprint, jwt
├── prisma/
│   ├── schema.prisma
│   ├── migrations/
│   └── seed.ts                   # seeds the More-Phi product + pricing
├── test/                         # Vitest + Fastify inject
├── Dockerfile  docker-compose.yml  .env.example  README.md
```

## 4. Data Model (Prisma / PostgreSQL)

```prisma
model Product {
  id             String   @id @default(cuid())
  slug           String   @unique        // "more-phi"
  name           String
  description    String
  priceCents     Int                      // e.g. 7900 = $79.00
  currency       String   @default("usd") // ISO 4217 lowercase
  version        String                   // "3.3.0"
  downloadUrl    String                   // signed URL to .vst3 on object storage
  maxActivations Int      @default(3)
  isActive       Boolean  @default(true)
  createdAt      DateTime @default(now())
  updatedAt      DateTime @updatedAt
  orders         Order[]
  licenses       License[]
}

model Customer {
  id              String   @id @default(cuid())
  email           String   @unique
  passwordHash    String?
  name            String?
  company         String?
  country         String?
  stripeCustomerId String?
  createdAt       DateTime @default(now())
  updatedAt       DateTime @updatedAt
  orders          Order[]
  licenses        License[]
  refreshTokens   RefreshToken[]
}

model Order {
  id                       String   @id @default(cuid())
  customerId               String
  customer                 Customer @relation(fields: [customerId], references: [id])
  productId                String
  product                  Product  @relation(fields: [productId], references: [id])
  stripeCheckoutSessionId  String   @unique
  stripePaymentIntentId    String?
  amountCents              Int
  currency                 String   @default("usd")
  status                   OrderStatus @default(PENDING)
  createdAt                DateTime @default(now())
  paidAt                   DateTime?
  refundedAt               DateTime?
  licenses                 License[]
  paymentEvents            PaymentEvent[]
}

enum OrderStatus {
  PENDING
  PAID
  FAILED
  REFUNDED
}

model License {
  id             String   @id @default(cuid())
  customerId     String
  customer       Customer @relation(fields: [customerId], references: [id])
  orderId        String
  order          Order    @relation(fields: [orderId], references: [id])
  productId      String
  product        Product  @relation(fields: [productId], references: [id])
  key            String   @unique        // human-readable serial
  keyHash        String   @unique        // sha256(key), used for lookups
  status         LicenseStatus @default(ACTIVE)
  maxActivations Int      @default(3)
  createdAt      DateTime @default(now())
  revokedAt      DateTime?
  activations    Activation[]

  @@index([customerId])
}

enum LicenseStatus {
  ACTIVE
  REVOKED
  EXPIRED
}

model Activation {
  id              String   @id @default(cuid())
  licenseId       String
  license         License  @relation(fields: [licenseId], references: [id])
  fingerprintHash String                     // sha256(machineFingerprint)
  hostname        String?
  os              String?
  appVersion      String?
  status          ActivationStatus @default(ACTIVE)
  activatedAt     DateTime @default(now())
  lastSeenAt      DateTime @default(now())

  @@unique([licenseId, fingerprintHash])   // re-activate same machine, no extra slot
  @@index([fingerprintHash])
}

enum ActivationStatus {
  ACTIVE
  DEACTIVATED
}

model PaymentEvent {
  id              String   @id @default(cuid())
  orderId         String?
  order           Order?   @relation(fields: [orderId], references: [id])
  stripeEventId   String   @unique          // idempotency key
  type            String                     // checkout.session.completed, etc.
  amountCents     Int?
  status          String
  rawPayload      Json
  createdAt       DateTime @default(now())

  @@index([type])
}

model RefreshToken {
  id         String   @id @default(cuid())
  customerId String
  customer   Customer @relation(fields: [customerId], references: [id])
  tokenHash  String   @unique
  expiresAt  DateTime
  revokedAt  DateTime?

  @@index([customerId])
}
```

Key data decisions:
- License lookups go through `keyHash` (`sha256(key)`); the raw key is never used in a `WHERE` clause on the wire.
- Machine fingerprints are stored **only** as a hash — raw hardware IDs are never persisted.
- `PaymentEvent` is the reconciliation/audit log, keyed by Stripe event ID so replays are safe.

## 5. API Surface (`/v1`, JSON, versioned)

All responses use a consistent envelope. Success: the resource directly. Error:
```json
{ "error": { "code": "VALIDATION_ERROR", "message": "email is required", "details": { ... } } }
```

### Storefront / customer

#### `GET /v1/products/:slug` — product info (public)
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

#### `POST /v1/customers` — register (public, rate-limited)
```json
// request
{ "email": "jane@x.com", "password": "••••••••", "name": "Jane", "country": "US" }
// 201 response
{ "id": "cuid", "email": "jane@x.com" }
```

#### `POST /v1/auth/login` — → JWT + refresh (rate-limited)
```json
// request
{ "email": "jane@x.com", "password": "••••••••" }
// 200 response + Set-Cookie: mp_auth (httpOnly); mp_refresh (httpOnly)
{ "accessToken": "eyJ...", "customer": { "id": "cuid", "email": "jane@x.com" } }
```

`POST /v1/auth/refresh` · `POST /v1/auth/logout` · `GET /v1/me` (profile + orders + licenses, JWT)

#### `POST /v1/checkout` — create Stripe Checkout Session (public or JWT)
```json
// request
{ "productSlug": "more-phi", "email": "jane@x.com" }
// 200 response
{ "checkoutUrl": "https://checkout.stripe.com/c/...", "orderId": "cuid" }
```

### Plugin-facing (the license client inside the VST3)

#### `POST /v1/activations` — activate (rate-limited)
```json
// request
{ "licenseKey": "MP-7F3K-9QH2-XR4M-8LNT",
  "machineFingerprint": "<opaque plugin-computed string>",
  "hostname": "STUDIO-PC", "os": "win32", "appVersion": "3.3.0" }
// 200 response
{ "activationId": "cuid", "licenseId": "cuid", "activationsUsed": 1,
  "maxActivations": 3, "status": "ACTIVE" }
// 409 when slots exhausted
{ "error": { "code": "MAX_ACTIVATIONS_REACHED", "message": "..." } }
```

`POST /v1/activations/deactivate` — `{licenseKey, machineFingerprint}` frees a slot.

#### `GET /v1/licenses/verify?key=...` — lightweight status (rate-limited)
```json
{ "valid": true, "status": "ACTIVE", "revokedAt": null }
```

### Internal
- `POST /v1/webhooks/stripe` — signature-verified, idempotent, **exempt** from the rate limiter.
- `GET /health` — liveness/readiness (`{ "status": "ok", "db": true }`).

## 6. Stripe Flow (one-time purchase)

```
[Buy]    POST /v1/checkout
           → create Order(status=PENDING)
           → Stripe Checkout Session (mode=payment, one line item from Product)
           → return { checkoutUrl, orderId } → frontend redirects

[Pay]    user pays on Stripe-hosted page

[Fulfill] POST /v1/webhooks/stripe  (checkout.session.completed)
           → verify signature on RAW body, dedupe on stripeEventId
           → upsert Customer by email, Order→PAID + paidAt
           → generate License key, persist keyHash
           → email { licenseKey, downloadLink, setPasswordLink }
           → record PaymentEvent

[Fail]   payment_intent.payment_failed → Order→FAILED, log PaymentEvent
         (Stripe Smart Retries handle card retries server-side)

[Refund] charge.refunded → License→REVOKED, deactivate all activations, log
```

Retry logic: Stripe's Smart Retries handle card retries; the server logs failures and reconciles. No custom card-retry logic.

## 7. License Generation & Activation

- **Key format:** `MP-7F3K-9QH2-XR4M-8LNT` — Crockford Base32, ambiguity-stripped (no `0`/`O`/`1`/`I`), ~24 chars entropy via `crypto.randomBytes`. Uniqueness checked against `keyHash` (collision is astronomically unlikely; generation retries on the rare collision).
- **Machine fingerprint:** computed plugin-side from stable OS identifiers (host + MAC + volume/CPU IDs), sent as an opaque string. Server validates length/charset, stores **only** `sha256(fingerprint)`. Activation counts unique fingerprint hashes per license; `maxActivations` (default 3) enforced. Re-activating the same machine is free (the `@@unique([licenseId, fingerprintHash])` constraint). Deactivation releases a slot.

## 8. Security & Production Hardening

- **PCI:** card data never touches our servers (Stripe Checkout → **SAQ-A**).
- **Webhook:** raw-body signature verification (`stripe.webhooks.constructEvent`), idempotent on Stripe event ID.
- **Auth:** bcrypt (cost 12); JWT HS256 access (15m) + rotating refresh (30d) in `httpOnly`+`Secure`+`SameSite=Strict` cookies; Bearer option for API clients.
- **Hardening:** `@fastify/helmet`, `@fastify/cors` (env allowlist), `@fastify/rate-limit` (in-memory; Redis store noted for multi-instance scaling).
- **Logging:** `pino` structured JSON, request-ID correlation, sensitive fields redacted (password, license key masked).
- **Errors:** centralized handler → the error envelope above; no stack traces in production.
- **Secrets:** all via `.env`; validated with Zod at boot (fail fast on misconfig).

## 9. Testing

Vitest + Fastify `app.inject()` (in-process, no port binding). Stripe SDK mocked for determinism. Dedicated test database with `prisma migrate reset` between suites. Coverage:

- Auth + login rate limiting.
- Checkout session creation.
- Webhook fulfillment: happy path, idempotent replay (same event twice), failed payment, refund → revocation.
- Activation: happy, max-slots-reached, revoked license rejected, same-machine re-activation free, deactivation frees a slot.
- License key uniqueness + fingerprint hashing.

## 10. Deployment

- Multi-stage **Dockerfile** (compile TS → slim runtime) + **docker-compose** (app + postgres).
- `GET /health` for probes. Configured for Render / Fly.io / Railway / VPS + managed Postgres.
- Stripe webhook endpoint set in the Stripe dashboard → `/v1/webhooks/stripe`.
- Plugin binary (`MorePhi.vst3`) hosted on object storage (S3 / Cloudflare R2); `Product.downloadUrl` points to a signed-URL generator.

## 11. Environment Variables (`.env`)

```
NODE_ENV=
PORT=4000
DATABASE_URL=postgresql://postgres:postgres@localhost:5432/morephi_store
DATABASE_URL_TEST=postgresql://postgres:postgres@localhost:5432/morephi_store_test

# Auth
JWT_SECRET=            # 32+ random bytes
JWT_ACCESS_TTL=900     # 15 min
JWT_REFRESH_TTL=2592000 # 30 days
BCRYPT_COST=12

# Stripe
STRIPE_SECRET_KEY=sk_test_...
STRIPE_WEBHOOK_SECRET=whsec_...
STRIPE_SUCCESS_URL=http://localhost:5173/success?order={ORDER_ID}
STRIPE_CANCEL_URL=http://localhost:5173/cancel

# CORS / rate limiting
CORS_ORIGIN=http://localhost:5173
RATE_LIMIT_MAX=100
RATE_LIMIT_WINDOW=60000
LOGIN_RATE_LIMIT_MAX=10
LOGIN_RATE_LIMIT_WINDOW=60000
ACTIVATION_RATE_LIMIT_MAX=20
ACTIVATION_RATE_LIMIT_WINDOW=60000

# Licensing
LICENSE_KEY_PREFIX=MP
LICENSE_MAX_ACTIVATIONS=3

# Email
EMAIL_PROVIDER=console   # console | resend | smtp
EMAIL_FROM=orders@morephi.example
RESEND_API_KEY=
SMTP_HOST=  SMTP_PORT=  SMTP_USER=  SMTP_PASS=

# Misc
LOG_LEVEL=info
DOWNLOAD_URL_BASE=https://cdn.morephi.example/more-phi/
```

## 12. Defaults to Confirm

These are set in the seed/config and trivially adjustable; flagged here so they can be overridden before launch:

- **Price:** `7900` cents ($79.00 USD) — **placeholder**, set the real launch price.
- **Currency:** `usd`.
- **`maxActivations`:** `3` machines per license.
- **Refund behavior:** revokes the license (deactivates all machines). Alternative: keep license active on refund — change the `charge.refunded` handler if that's desired.
- **Plugin fingerprint algorithm:** defined as a contract; the exact stable-ID set is implemented C++-side. The server treats it as opaque.

## 13. Out of Scope (YAGNI)

Subscriptions, trials/demos, an admin UI, multi-currency switching, coupons/discounts, team/bulk licensing, a full storefront SPA, offline/cracked-key revocation lists. All modeled to be *addable* without rework; none built now.

## 14. Next Step

Proceed to the implementation plan via the `writing-plans` skill, which breaks this design into ordered, testable implementation tasks.
