# More-Phi Store — VST3 Purchasing System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a production-ready Node.js backend that sells the More-Phi VST3 plugin as a one-time Stripe purchase and issues machine-bound license keys validated online.

**Architecture:** A single Fastify service layered as routes → services → Prisma/PostgreSQL. Stripe Checkout handles payment (PCI SAQ-A); webhooks fulfill orders idempotently. The plugin activates against a hashed machine fingerprint with a cap on simultaneous activations.

**Tech Stack:** Node.js 20, TypeScript (strict), Fastify 4, Prisma 5, PostgreSQL 15, Stripe SDK, Zod, bcryptjs, pino, Vitest, Docker.

All paths are relative to `store-backend/` unless noted. Every task ends with a green test run and a commit.

---

## File Structure

```
store-backend/
├── package.json
├── tsconfig.json
├── vitest.config.ts
├── .env.example
├── .gitignore
├── Dockerfile
├── docker-compose.yml
├── prisma/
│   ├── schema.prisma
│   └── seed.ts
└── src/
    ├── server.ts
    ├── app.ts                      # Fastify factory (testable)
    ├── config/env.ts
    ├── db/prisma.ts
    ├── lib/
    │   ├── licenseKey.ts
    │   ├── hash.ts
    │   └── logger.ts
    ├── plugins/
    │   ├── errorHandler.ts
    │   ├── rateLimit.ts
    │   └── auth.ts
    ├── services/
    │   ├── CustomerService.ts
    │   ├── OrderService.ts
    │   ├── LicenseService.ts
    │   ├── ActivationService.ts
    │   ├── StripeService.ts
    │   ├── WebhookService.ts
    │   └── EmailService.ts
    └── routes/v1/
        ├── health.routes.ts
        ├── products.routes.ts
        ├── customers.routes.ts
        ├── auth.routes.ts
        ├── me.routes.ts
        ├── checkout.routes.ts
        ├── licenses.routes.ts
        └── webhooks.stripe.routes.ts
test/
├── setup.ts
├── helpers.ts
└── unit/ + integration/ (per task)
```

---

## Task 1: Project scaffold + health endpoint

**Files:**
- Create: `store-backend/package.json`, `store-backend/tsconfig.json`, `store-backend/vitest.config.ts`, `store-backend/.gitignore`, `store-backend/.env.example`, `store-backend/src/app.ts`, `store-backend/src/server.ts`, `store-backend/src/routes/v1/health.routes.ts`, `store-backend/test/setup.ts`, `store-backend/test/integration/health.test.ts`

- [ ] **Step 1: Create `package.json`**

```json
{
  "name": "more-phi-store",
  "version": "1.0.0",
  "private": true,
  "type": "module",
  "engines": { "node": ">=20" },
  "scripts": {
    "dev": "tsx watch src/server.ts",
    "build": "tsc",
    "start": "node dist/server.js",
    "test": "vitest run",
    "test:watch": "vitest",
    "prisma:generate": "prisma generate",
    "prisma:migrate": "prisma migrate dev",
    "prisma:seed": "tsx prisma/seed.ts",
    "db:reset": "prisma migrate reset --force"
  },
  "dependencies": {
    "@fastify/cookie": "^9.3.1",
    "@fastify/cors": "^9.0.1",
    "@fastify/helmet": "^11.1.1",
    "@fastify/jwt": "^8.0.0",
    "@fastify/rate-limit": "^9.1.0",
    "@fastify/raw-body": "^4.3.0",
    "@prisma/client": "^5.14.0",
    "bcryptjs": "^2.4.3",
    "fastify": "^4.27.0",
    "nodemailer": "^6.9.13",
    "pino": "^9.1.0",
    "pino-pretty": "^11.2.1",
    "resend": "^3.5.0",
    "stripe": "^15.5.0",
    "zod": "^3.23.8"
  },
  "devDependencies": {
    "@types/bcryptjs": "^2.4.6",
    "@types/node": "^20.14.0",
    "@types/nodemailer": "^6.4.15",
    "prisma": "^5.14.0",
    "tsx": "^4.15.1",
    "typescript": "^5.4.5",
    "vitest": "^1.6.0"
  }
}
```

- [ ] **Step 2: Create `tsconfig.json`**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "NodeNext",
    "moduleResolution": "NodeNext",
    "outDir": "dist",
    "rootDir": "src",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true,
    "forceConsistentCasingInFileNames": true,
    "resolveJsonModule": true,
    "declaration": false,
    "sourceMap": true
  },
  "include": ["src"],
  "exclude": ["node_modules", "dist", "test"]
}
```

- [ ] **Step 3: Create `vitest.config.ts`**

```ts
import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    setupFiles: ['./test/setup.ts'],
    testTimeout: 15000,
    hookTimeout: 15000,
  },
});
```

- [ ] **Step 4: Create `.gitignore`**

```
node_modules/
dist/
.env
*.log
coverage/
```

- [ ] **Step 5: Create `.env.example`** (copy from spec §11)

```
NODE_ENV=development
PORT=4000
DATABASE_URL=postgresql://postgres:postgres@localhost:5432/morephi_store
DATABASE_URL_TEST=postgresql://postgres:postgres@localhost:5432/morephi_store_test
JWT_SECRET=change-me-to-32-plus-random-bytes
JWT_ACCESS_TTL=900
JWT_REFRESH_TTL=2592000
BCRYPT_COST=12
STRIPE_SECRET_KEY=sk_test_replace_me
STRIPE_WEBHOOK_SECRET=whsec_replace_me
STRIPE_SUCCESS_URL=http://localhost:5173/success?order={ORDER_ID}
STRIPE_CANCEL_URL=http://localhost:5173/cancel
CORS_ORIGIN=http://localhost:5173
RATE_LIMIT_MAX=100
RATE_LIMIT_WINDOW=60000
LOGIN_RATE_LIMIT_MAX=10
LOGIN_RATE_LIMIT_WINDOW=60000
ACTIVATION_RATE_LIMIT_MAX=20
ACTIVATION_RATE_LIMIT_WINDOW=60000
LICENSE_KEY_PREFIX=MP
LICENSE_MAX_ACTIVATIONS=3
EMAIL_PROVIDER=console
EMAIL_FROM=orders@morephi.example
RESEND_API_KEY=
SMTP_HOST=
SMTP_PORT=
SMTP_USER=
SMTP_PASS=
LOG_LEVEL=info
DOWNLOAD_URL_BASE=https://cdn.morephi.example/more-phi/
```

- [ ] **Step 6: Create `src/app.ts`** (Fastify factory — importable by tests without binding a port)

```ts
import Fastify, { type FastifyInstance } from 'fastify';

export async function buildApp(): Promise<FastifyInstance> {
  const app = Fastify({
    logger: { level: process.env.LOG_LEVEL ?? 'info' },
    genReqId: () => crypto.randomUUID(),
  });

  app.get('/health', async () => ({ status: 'ok', db: true }));
  return app;
}
```

- [ ] **Step 7: Create `src/server.ts`**

```ts
import { buildApp } from './app.js';

const app = await buildApp();

try {
  await app.listen({ port: Number(process.env.PORT ?? 4000), host: '0.0.0.0' });
} catch (err) {
  app.log.error(err);
  process.exit(1);
}
```

- [ ] **Step 8: Create `test/setup.ts`**

```ts
// Tests inherit env from the process shell. Add overrides here if needed.
process.env.NODE_ENV ??= 'test';
```

- [ ] **Step 9: Create `test/integration/health.test.ts`**

```ts
import { describe, it, expect, afterEach } from 'vitest';
import { buildApp } from '../../src/app.js';

describe('GET /health', () => {
  let app: Awaited<ReturnType<typeof buildApp>>;
  afterEach(async () => { if (app) await app.close(); });

  it('returns ok', async () => {
    app = await buildApp();
    const res = await app.inject({ method: 'GET', url: '/health' });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ status: 'ok', db: true });
  });
});
```

- [ ] **Step 10: Install deps and run the test**

Run: `cd store-backend && npm install && npm test -- health`
Expected: `1 passed`.

- [ ] **Step 11: Commit**

```bash
cd store-backend && git add -A && git commit -m "feat(store): scaffold Fastify app with health endpoint"
```

---

## Task 2: Config + Prisma schema + client + seed

**Files:**
- Create: `store-backend/src/config/env.ts`, `store-backend/src/db/prisma.ts`, `store-backend/prisma/schema.prisma`, `store-backend/prisma/seed.ts`, `store-backend/test/integration/db.test.ts`

- [ ] **Step 1: Create `src/config/env.ts`**

```ts
import { z } from 'zod';

const schema = z.object({
  NODE_ENV: z.enum(['development', 'test', 'production']).default('development'),
  PORT: z.coerce.number().default(4000),
  DATABASE_URL: z.string().min(1),
  DATABASE_URL_TEST: z.string().min(1).optional(),
  JWT_SECRET: z.string().min(32),
  JWT_ACCESS_TTL: z.coerce.number().default(900),
  JWT_REFRESH_TTL: z.coerce.number().default(2592000),
  BCRYPT_COST: z.coerce.number().default(12),
  STRIPE_SECRET_KEY: z.string().min(1),
  STRIPE_WEBHOOK_SECRET: z.string().min(1),
  STRIPE_SUCCESS_URL: z.string().min(1),
  STRIPE_CANCEL_URL: z.string().min(1),
  CORS_ORIGIN: z.string().default('http://localhost:5173'),
  RATE_LIMIT_MAX: z.coerce.number().default(100),
  RATE_LIMIT_WINDOW: z.coerce.number().default(60000),
  LOGIN_RATE_LIMIT_MAX: z.coerce.number().default(10),
  LOGIN_RATE_LIMIT_WINDOW: z.coerce.number().default(60000),
  ACTIVATION_RATE_LIMIT_MAX: z.coerce.number().default(20),
  ACTIVATION_RATE_LIMIT_WINDOW: z.coerce.number().default(60000),
  LICENSE_KEY_PREFIX: z.string().default('MP'),
  LICENSE_MAX_ACTIVATIONS: z.coerce.number().default(3),
  EMAIL_PROVIDER: z.enum(['console', 'resend', 'smtp']).default('console'),
  EMAIL_FROM: z.string().default('orders@morephi.example'),
  RESEND_API_KEY: z.string().optional(),
  SMTP_HOST: z.string().optional(),
  SMTP_PORT: z.coerce.number().optional(),
  SMTP_USER: z.string().optional(),
  SMTP_PASS: z.string().optional(),
  LOG_LEVEL: z.string().default('info'),
  DOWNLOAD_URL_BASE: z.string().default('https://cdn.morephi.example/more-phi/'),
});

export type Env = z.infer<typeof schema>;

function load(): Env {
  const parsed = schema.safeParse(process.env);
  if (!parsed.success) {
    // eslint-disable-next-line no-console
    console.error('Invalid environment configuration:\n', parsed.error.flatten().fieldErrors);
    throw new Error('Invalid environment configuration');
  }
  return parsed.data;
}

export const env = load();
```

- [ ] **Step 2: Create `prisma/schema.prisma`** (full model from spec §4)

```prisma
generator client {
  provider = "prisma-client-js"
}

datasource db {
  provider = "postgresql"
  url      = env("DATABASE_URL")
}

model Product {
  id             String   @id @default(cuid())
  slug           String   @unique
  name           String
  description    String
  priceCents     Int
  currency       String   @default("usd")
  version        String
  downloadUrl    String
  maxActivations Int      @default(3)
  isActive       Boolean  @default(true)
  createdAt      DateTime @default(now())
  updatedAt      DateTime @updatedAt
  orders         Order[]
  licenses       License[]
}

model Customer {
  id               String         @id @default(cuid())
  email            String         @unique
  passwordHash     String?
  name             String?
  company          String?
  country          String?
  stripeCustomerId String?
  createdAt        DateTime       @default(now())
  updatedAt        DateTime       @updatedAt
  orders           Order[]
  licenses         License[]
  refreshTokens    RefreshToken[]
}

model Order {
  id                      String     @id @default(cuid())
  customerId              String
  customer                Customer   @relation(fields: [customerId], references: [id])
  productId               String
  product                 Product    @relation(fields: [productId], references: [id])
  stripeCheckoutSessionId String     @unique
  stripePaymentIntentId   String?
  amountCents             Int
  currency                String     @default("usd")
  status                  OrderStatus @default(PENDING)
  createdAt               DateTime   @default(now())
  paidAt                  DateTime?
  refundedAt              DateTime?
  licenses                License[]
  paymentEvents           PaymentEvent[]
}

enum OrderStatus {
  PENDING
  PAID
  FAILED
  REFUNDED
}

model License {
  id             String       @id @default(cuid())
  customerId     String
  customer       Customer     @relation(fields: [customerId], references: [id])
  orderId        String
  order          Order        @relation(fields: [orderId], references: [id])
  productId      String
  product        Product      @relation(fields: [productId], references: [id])
  key            String       @unique
  keyHash        String       @unique
  status         LicenseStatus @default(ACTIVE)
  maxActivations Int          @default(3)
  createdAt      DateTime     @default(now())
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
  id              String           @id @default(cuid())
  licenseId       String
  license         License          @relation(fields: [licenseId], references: [id])
  fingerprintHash String
  hostname        String?
  os              String?
  appVersion      String?
  status          ActivationStatus @default(ACTIVE)
  activatedAt     DateTime         @default(now())
  lastSeenAt      DateTime         @default(now())

  @@unique([licenseId, fingerprintHash])
  @@index([fingerprintHash])
}

enum ActivationStatus {
  ACTIVE
  DEACTIVATED
}

model PaymentEvent {
  id            String   @id @default(cuid())
  orderId       String?
  order         Order?   @relation(fields: [orderId], references: [id])
  stripeEventId String   @unique
  type          String
  amountCents   Int?
  status        String
  rawPayload    Json
  createdAt     DateTime @default(now())

  @@index([type])
}

model RefreshToken {
  id         String    @id @default(cuid())
  customerId String
  customer   Customer  @relation(fields: [customerId], references: [id])
  tokenHash  String    @unique
  expiresAt  DateTime
  revokedAt  DateTime?

  @@index([customerId])
}
```

- [ ] **Step 3: Create `src/db/prisma.ts`**

```ts
import { PrismaClient } from '@prisma/client';

const url = process.env.NODE_ENV === 'test' && process.env.DATABASE_URL_TEST
  ? process.env.DATABASE_URL_TEST
  : process.env.DATABASE_URL;

export const prisma = new PrismaClient({ datasources: { db: { url } } });
```

- [ ] **Step 4: Create `prisma/seed.ts`**

```ts
import { PrismaClient } from '@prisma/client';

const prisma = new PrismaClient();

async function main() {
  await prisma.product.upsert({
    where: { slug: 'more-phi' },
    update: {},
    create: {
      slug: 'more-phi',
      name: 'More-Phi',
      description: 'Plugin-morphing VST3/AU host with parameter interpolation, physics modes, and AI mastering.',
      priceCents: 7900,
      currency: 'usd',
      version: '3.3.0',
      downloadUrl: 'https://cdn.morephi.example/more-phi/MorePhi-3.3.0.vst3',
      maxActivations: 3,
      isActive: true,
    },
  });
}

main()
  .then(() => prisma.$disconnect())
  .catch(async (e) => {
    console.error(e);
    await prisma.$disconnect();
    process.exit(1);
  });
```

Add to `package.json`:
```json
  "prisma": { "seed": "tsx prisma/seed.ts" }
```

- [ ] **Step 5: Create the test DBs and run migration**

Run:
```bash
cd store-backend
psql -c "CREATE DATABASE morephi_store;"
psql -c "CREATE DATABASE morephi_store_test;"
DATABASE_URL=postgresql://postgres:postgres@localhost:5432/morephi_store npx prisma migrate dev --name init
DATABASE_URL=postgresql://postgres:postgres@localhost:5432/morephi_store_test npx prisma migrate dev
DATABASE_URL=postgresql://postgres:postgres@localhost:5432/morephi_store npm run prisma:seed
```
Expected: migration applied to both DBs; More-Phi product seeded.

- [ ] **Step 6: Create `test/integration/db.test.ts`**

```ts
import { describe, it, expect, beforeAll } from 'vitest';
import { prisma } from '../../src/db/prisma.js';

describe('prisma client + seed', () => {
  beforeAll(async () => {
    await prisma.product.upsert({
      where: { slug: 'more-phi' },
      update: {},
      create: {
        slug: 'more-phi', name: 'More-Phi', description: 'x',
        priceCents: 7900, version: '3.3.0', downloadUrl: 'u',
      },
    });
  });

  it('connects and reads the seeded product', async () => {
    const product = await prisma.product.findUnique({ where: { slug: 'more-phi' } });
    expect(product).not.toBeNull();
    expect(product!.priceCents).toBe(7900);
    expect(product!.maxActivations).toBe(3);
  });
});
```

- [ ] **Step 7: Run test**

Run: `NODE_ENV=test npm test -- db`
Expected: `1 passed`.

- [ ] **Step 8: Commit**

```bash
cd store-backend && git add -A && git commit -m "feat(store): add config, prisma schema, client, and seed"
```

---

## Task 3: Core plugins — error envelope, request ID, helmet, CORS

**Files:**
- Create: `store-backend/src/lib/logger.ts`, `store-backend/src/plugins/errorHandler.ts`, `store-backend/src/plugins/common.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/errors.test.ts`

- [ ] **Step 1: Create `src/lib/logger.ts`**

```ts
import pino from 'pino';
import { env } from '../config/env.js';

export const logger = pino({
  level: env.LOG_LEVEL,
  redact: { paths: ['req.headers.authorization', 'req.headers.cookie', 'password', '*.password', 'licenseKey', '*.licenseKey'], censor: '[REDACTED]' },
  transport: env.NODE_ENV === 'development' ? { target: 'pino-pretty' } : undefined,
});
```

- [ ] **Step 2: Create `src/plugins/errorHandler.ts`**

```ts
import type { FastifyInstance, FastifyError, FastifyRequest, FastifyReply } from 'fastify';
import { ZodError } from 'zod';

export function errorEnvelope(code: string, message: string, status: number, details?: unknown) {
  return { error: { code, message, ...(details ? { details } : {}) } };
}

export function errorHandler(app: FastifyInstance) {
  app.setErrorHandler((err: FastifyError, req: FastifyRequest, reply: FastifyReply) => {
    if (err instanceof ZodError) {
      req.log.warn({ err: err.flatten() }, 'validation error');
      return reply.code(400).send(errorEnvelope('VALIDATION_ERROR', 'Request validation failed', 400, err.flatten()));
    }
    if (err.statusCode && err.statusCode >= 400 && err.statusCode < 500) {
      req.log.warn({ err }, 'client error');
      const code = err.code ?? 'BAD_REQUEST';
      return reply.code(err.statusCode).send(errorEnvelope(code, err.message, err.statusCode));
    }
    req.log.error({ err }, 'unexpected error');
    const msg = process.env.NODE_ENV === 'production' ? 'Internal server error' : err.message;
    return reply.code(500).send(errorEnvelope('INTERNAL_ERROR', msg, 500));
  });

  app.setNotFoundHandler((req, reply) => {
    reply.code(404).send(errorEnvelope('NOT_FOUND', `Route ${req.method} ${req.url} not found`, 404));
  });
}
```

- [ ] **Step 3: Create `src/plugins/common.ts`** (registers helmet, cors, error handler)

```ts
import type { FastifyInstance } from 'fastify';
import cors from '@fastify/cors';
import helmet from '@fastify/helmet';
import { env } from '../config/env.js';
import { errorHandler } from './errorHandler.js';

export async function registerCommon(app: FastifyInstance) {
  await app.register(helmet);
  await app.register(cors, {
    origin: env.CORS_ORIGIN.split(',').map((o) => o.trim()),
    credentials: true,
  });
  errorHandler(app);
}
```

- [ ] **Step 4: Modify `src/app.ts`** — wire in common plugins

Replace `src/app.ts` with:

```ts
import Fastify, { type FastifyInstance } from 'fastify';
import { registerCommon } from './plugins/common.js';

export async function buildApp(): Promise<FastifyInstance> {
  const app = Fastify({
    logger: { level: process.env.LOG_LEVEL ?? 'info' },
    genReqId: () => crypto.randomUUID(),
    trustProxy: true,
  });

  await registerCommon(app);

  app.get('/health', async () => ({ status: 'ok', db: true }));
  return app;
}
```

- [ ] **Step 5: Create `test/integration/errors.test.ts`**

```ts
import { describe, it, expect, afterEach } from 'vitest';
import { buildApp } from '../../src/app.js';

describe('error envelope', () => {
  let app: Awaited<ReturnType<typeof buildApp>>;
  afterEach(async () => { if (app) await app.close(); });

  it('returns a structured 404 envelope', async () => {
    app = await buildApp();
    const res = await app.inject({ method: 'GET', url: '/nope' });
    expect(res.statusCode).toBe(404);
    expect(res.json()).toEqual({
      error: { code: 'NOT_FOUND', message: 'Route GET /nope not found' },
    });
  });
});
```

- [ ] **Step 6: Run tests**

Run: `npm test -- errors health`
Expected: `2 passed`.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(store): add error envelope, helmet, CORS, request logging"
```

---

## Task 4: License key + hashing utilities (pure TDD)

**Files:**
- Create: `store-backend/src/lib/licenseKey.ts`, `store-backend/src/lib/hash.ts`, `store-backend/test/unit/licenseKey.test.ts`, `store-backend/test/unit/hash.test.ts`

- [ ] **Step 1: Create the failing test `test/unit/licenseKey.test.ts`**

```ts
import { describe, it, expect } from 'vitest';
import { generateLicenseKey } from '../../src/lib/licenseKey.js';

describe('generateLicenseKey', () => {
  it('uses the MP prefix and 4 groups of 4 chars', () => {
    const key = generateLicenseKey();
    expect(key).toMatch(/^MP(-[A-Z2-9]{4}){4}$/);
  });

  it('never contains ambiguous characters 0, 1, O, I', () => {
    for (let i = 0; i < 200; i++) {
      const key = generateLicenseKey();
      expect(key).not.toMatch(/[01OI]/);
    }
  });

  it('honors a custom prefix', () => {
    expect(generateLicenseKey('MPX')).toMatch(/^MPX-/);
  });

  it('produces unique keys across many calls', () => {
    const keys = new Set(Array.from({ length: 1000 }, () => generateLicenseKey()));
    expect(keys.size).toBe(1000);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm test -- licenseKey`
Expected: FAIL — module not found.

- [ ] **Step 3: Create `src/lib/licenseKey.ts`**

```ts
import { randomBytes } from 'node:crypto';
import { env } from '../config/env.js';

// Crockford Base32, ambiguity-stripped: no 0, 1, O, I (exactly 32 symbols → no modulo bias).
const ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';

export function generateLicenseKey(prefix: string = env.LICENSE_KEY_PREFIX, groups = 4, groupSize = 4): string {
  const total = groups * groupSize;
  const buf = randomBytes(total);
  const chars: string[] = [];
  for (let i = 0; i < total; i++) {
    chars.push(ALPHABET[buf[i] % ALPHABET.length]);
  }
  const built: string[] = [];
  for (let g = 0; g < groups; g++) {
    built.push(chars.slice(g * groupSize, (g + 1) * groupSize).join(''));
  }
  return `${prefix}-${built.join('-')}`;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm test -- licenseKey`
Expected: `4 passed`.

- [ ] **Step 5: Create the failing test `test/unit/hash.test.ts`**

```ts
import { describe, it, expect } from 'vitest';
import { sha256Hex } from '../../src/lib/hash.js';

describe('sha256Hex', () => {
  it('is deterministic', () => {
    expect(sha256Hex('abc')).toBe(sha256Hex('abc'));
  });

  it('trims the input', () => {
    expect(sha256Hex('  abc  ')).toBe(sha256Hex('abc'));
  });

  it('differs for different inputs', () => {
    expect(sha256Hex('abc')).not.toBe(sha256Hex('abd'));
  });

  it('returns a 64-char hex string', () => {
    expect(sha256Hex('x')).toMatch(/^[0-9a-f]{64}$/);
  });
});
```

- [ ] **Step 6: Run test to verify it fails**

Run: `npm test -- hash`
Expected: FAIL — module not found.

- [ ] **Step 7: Create `src/lib/hash.ts`**

```ts
import { createHash, randomBytes } from 'node:crypto';

export function sha256Hex(input: string): string {
  return createHash('sha256').update(input.trim()).digest('hex');
}

export function randomToken(bytes = 32): string {
  return randomBytes(bytes).toString('hex');
}
```

- [ ] **Step 8: Run test to verify it passes**

Run: `npm test -- hash licenseKey`
Expected: `8 passed`.

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "feat(store): add license key generator and sha256 helpers"
```

---

## Task 5: Rate-limit plugin

**Files:**
- Create: `store-backend/src/plugins/rateLimit.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/rateLimit.test.ts`

- [ ] **Step 1: Create `src/plugins/rateLimit.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import rateLimit from '@fastify/rate-limit';
import { env } from '../config/env.js';

export const LIMITS = {
  login: { max: env.LOGIN_RATE_LIMIT_MAX, timeWindow: env.LOGIN_RATE_LIMIT_WINDOW },
  activation: { max: env.ACTIVATION_RATE_LIMIT_MAX, timeWindow: env.ACTIVATION_RATE_LIMIT_WINDOW },
} as const;

export async function registerRateLimit(app: FastifyInstance) {
  await app.register(rateLimit, {
    max: env.RATE_LIMIT_MAX,
    timeWindow: env.RATE_LIMIT_WINDOW,
    global: true,
    // Whitelist the Stripe webhook (verified by signature; Stripe retries on 429).
    hook: 'onRequest',
  });
  // Expose a helper to attach per-route limits.
  app.decorate('rateLimits', LIMITS);
}
```

- [ ] **Step 2: Modify `src/app.ts`** — register rate limiting after common plugins

Add after `await registerCommon(app);`:
```ts
import { registerRateLimit } from './plugins/rateLimit.js';
...
await registerCommon(app);
await registerRateLimit(app);
```

- [ ] **Step 3: Create `test/integration/rateLimit.test.ts`** (uses a low-limit test route)

```ts
import { describe, it, expect, afterEach } from 'vitest';
import Fastify from 'fastify';
import rateLimit from '@fastify/rate-limit';

describe('rate limiting', () => {
  let app: ReturnType<typeof Fastify>;
  afterEach(async () => { if (app) await app.close(); });

  it('returns 429 after the limit is exceeded', async () => {
    app = Fastify({ logger: false });
    await app.register(rateLimit, { max: 2, timeWindow: 60000, global: true });
    app.get('/ping', async () => ({ ok: true }));
    const r1 = await app.inject({ method: 'GET', url: '/ping' });
    const r2 = await app.inject({ method: 'GET', url: '/ping' });
    const r3 = await app.inject({ method: 'GET', url: '/ping' });
    expect(r1.statusCode).toBe(200);
    expect(r2.statusCode).toBe(200);
    expect(r3.statusCode).toBe(429);
  });
});
```

- [ ] **Step 4: Run tests**

Run: `npm test -- rateLimit health`
Expected: `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(store): add global + named rate-limit buckets"
```

---

## Task 6: Auth — JWT, password hashing, CustomerService, auth routes

**Files:**
- Create: `store-backend/src/services/CustomerService.ts`, `store-backend/src/plugins/auth.ts`, `store-backend/src/routes/v1/customers.routes.ts`, `store-backend/src/routes/v1/auth.routes.ts`, `store-backend/src/routes/v1/me.routes.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/auth.test.ts`, `store-backend/test/helpers.ts`

- [ ] **Step 1: Create `test/helpers.ts`** (shared: reset DB between tests)

```ts
import { prisma } from '../src/db/prisma.js';

export async function resetDb() {
  const models = ['Activation', 'License', 'PaymentEvent', 'RefreshToken', 'Order', 'Customer'];
  // Keep Product (seeded). Delete in dependency order.
  await prisma.activation.deleteMany();
  await prisma.license.deleteMany();
  await prisma.paymentEvent.deleteMany();
  await prisma.refreshToken.deleteMany();
  await prisma.order.deleteMany();
  await prisma.customer.deleteMany();
}

export function uniqueEmail(prefix = 'user') {
  return `${prefix}-${Date.now()}-${Math.floor(Math.random() * 1e6)}@test.com`;
}
```

- [ ] **Step 2: Create `src/services/CustomerService.ts`**

```ts
import bcrypt from 'bcryptjs';
import type { PrismaClient, Customer } from '@prisma/client';
import { env } from '../config/env.js';
import { prisma } from '../db/prisma.js';

export class CustomerService {
  constructor(private readonly prisma: PrismaClient) {}

  async register(input: { email: string; password: string; name?: string; company?: string; country?: string }): Promise<Customer> {
    const passwordHash = await bcrypt.hash(input.password, env.BCRYPT_COST);
    return this.prisma.customer.create({
      data: {
        email: input.email.toLowerCase(),
        passwordHash,
        name: input.name,
        company: input.company,
        country: input.country,
      },
    });
  }

  findByEmail(email: string) {
    return this.prisma.customer.findUnique({ where: { email: email.toLowerCase() } });
  }

  getById(id: string) {
    return this.prisma.customer.findUnique({ where: { id } });
  }

  async upsertByEmail(email: string, name?: string): Promise<Customer> {
    return this.prisma.customer.upsert({
      where: { email: email.toLowerCase() },
      update: name ? { name } : {},
      create: { email: email.toLowerCase(), name },
    });
  }

  async verifyPassword(customer: Customer, password: string): Promise<boolean> {
    if (!customer.passwordHash) return false;
    return bcrypt.compare(password, customer.passwordHash);
  }

  async setPassword(customerId: string, password: string): Promise<void> {
    const passwordHash = await bcrypt.hash(password, env.BCRYPT_COST);
    await this.prisma.customer.update({ where: { id: customerId }, data: { passwordHash } });
  }
}

export const customerService = new CustomerService(prisma);
```

- [ ] **Step 3: Create `src/plugins/auth.ts`**

```ts
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import jwtPlugin from '@fastify/jwt';
import cookie from '@fastify/cookie';
import type { Customer } from '@prisma/client';
import { env } from '../config/env.js';
import { prisma } from '../db/prisma.js';
import { randomToken, sha256Hex } from '../lib/hash.js';

export interface SessionUser {
  id: string;
  email: string;
}

declare module 'fastify' {
  interface FastifyInstance {
    requireAuth: (req: FastifyRequest, reply: FastifyReply) => Promise<void>;
  }
  interface FastifyRequest {
    user: SessionUser;
  }
}

export async function registerAuth(app: FastifyInstance) {
  await app.register(cookie);
  await app.register(jwtPlugin, {
    secret: env.JWT_SECRET,
    sign: { expiresIn: env.JWT_ACCESS_TTL },
  });

  async function issueSession(reply: FastifyReply, customer: Pick<Customer, 'id' | 'email'>) {
    const accessToken = app.jwt.sign({ sub: customer.id, email: customer.email });
    const rawRefresh = randomToken(32);
    const refreshHash = sha256Hex(rawRefresh);
    await prisma.refreshToken.create({
      data: {
        customerId: customer.id,
        tokenHash: refreshHash,
        expiresAt: new Date(Date.now() + env.JWT_REFRESH_TTL * 1000),
      },
    });
    reply.setCookie('access_token', accessToken, {
      httpOnly: true, secure: env.NODE_ENV === 'production', sameSite: 'strict', path: '/', maxAge: env.JWT_ACCESS_TTL,
    });
    reply.setCookie('refresh_token', rawRefresh, {
      httpOnly: true, secure: env.NODE_ENV === 'production', sameSite: 'strict', path: '/v1/auth', maxAge: env.JWT_REFRESH_TTL,
    });
    return { accessToken };
  }

  app.decorate('issueSession', issueSession);
  app.decorate('requireAuth', async (req: FastifyRequest, reply: FastifyReply) => {
    const header = req.headers.authorization;
    const bearer = header?.startsWith('Bearer ') ? header.slice(7) : undefined;
    const token = bearer ?? req.cookies.access_token;
    if (!token) {
      return reply.code(401).send({ error: { code: 'UNAUTHORIZED', message: 'Authentication required' } });
    }
    try {
      const payload = app.jwt.verify<{ sub: string; email: string }>(token);
      req.user = { id: payload.sub, email: payload.email };
    } catch {
      return reply.code(401).send({ error: { code: 'UNAUTHORIZED', message: 'Invalid or expired token' } });
    }
  });

  // Expose for routes to use in their own preHandler (refresh rotation).
  app.decorate('rotateRefresh', async (reply: FastifyReply, rawRefresh: string): Promise<boolean> => {
    const refreshHash = sha256Hex(rawRefresh);
    const record = await prisma.refreshToken.findUnique({ where: { tokenHash: refreshHash } });
    if (!record || record.revokedAt || record.expiresAt < new Date()) return false;
    await prisma.refreshToken.update({ where: { id: record.id }, data: { revokedAt: new Date() } });
    const customer = await prisma.customer.findUnique({ where: { id: record.customerId } });
    if (!customer) return false;
    await issueSession(reply, customer);
    return true;
  });
}

declare module 'fastify' {
  interface FastifyInstance {
    issueSession: (reply: FastifyReply, customer: Pick<Customer, 'id' | 'email'>) => Promise<{ accessToken: string }>;
    rotateRefresh: (reply: FastifyReply, rawRefresh: string) => Promise<boolean>;
  }
}
```

- [ ] **Step 4: Create `src/routes/v1/customers.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { customerService } from '../../services/CustomerService.js';
import { LIMITS } from '../../plugins/rateLimit.js';

const registerSchema = z.object({
  email: z.string().email(),
  password: z.string().min(8).max(128),
  name: z.string().min(1).max(200).optional(),
  company: z.string().max(200).optional(),
  country: z.string().max(2).optional(),
});

export async function customersRoutes(app: FastifyInstance) {
  app.post('/v1/customers', { config: { rateLimit: LIMITS.login } }, async (req, reply) => {
    const parsed = registerSchema.safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const { email, password, ...rest } = parsed.data;
    const existing = await customerService.findByEmail(email);
    if (existing) {
      return reply.code(409).send({ error: { code: 'EMAIL_TAKEN', message: 'A customer with this email already exists' } });
    }
    const customer = await customerService.register({ email, password, ...rest });
    const { accessToken } = await app.issueSession(reply, customer);
    return reply.code(201).send({ id: customer.id, email: customer.email, accessToken });
  });
}
```

- [ ] **Step 5: Create `src/routes/v1/auth.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { customerService } from '../../services/CustomerService.js';
import { LIMITS } from '../../plugins/rateLimit.js';

const loginSchema = z.object({
  email: z.string().email(),
  password: z.string().min(1).max(128),
});

export async function authRoutes(app: FastifyInstance) {
  app.post('/v1/auth/login', { config: { rateLimit: LIMITS.login } }, async (req, reply) => {
    const parsed = loginSchema.safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const customer = await customerService.findByEmail(parsed.data.email);
    const ok = customer ? await customerService.verifyPassword(customer, parsed.data.password) : false;
    if (!customer || !ok) {
      return reply.code(401).send({ error: { code: 'INVALID_CREDENTIALS', message: 'Invalid email or password' } });
    }
    const { accessToken } = await app.issueSession(reply, customer);
    return reply.send({ accessToken, customer: { id: customer.id, email: customer.email } });
  });

  app.post('/v1/auth/refresh', async (req, reply) => {
    const raw = req.cookies.refresh_token;
    if (!raw) return reply.code(401).send({ error: { code: 'UNAUTHORIZED', message: 'No refresh token' } });
    const ok = await app.rotateRefresh(reply, raw);
    if (!ok) return reply.code(401).send({ error: { code: 'UNAUTHORIZED', message: 'Invalid or expired refresh token' } });
    return reply.send({ ok: true });
  });

  app.post('/v1/auth/logout', async (req, reply) => {
    const raw = req.cookies.refresh_token;
    if (raw) {
      const record = await (await import('../../db/prisma.js')).prisma.refreshToken.findUnique({ where: { tokenHash: (await import('../../lib/hash.js')).sha256Hex(raw) } });
      if (record) await (await import('../../db/prisma.js')).prisma.refreshToken.update({ where: { id: record.id }, data: { revokedAt: new Date() } });
    }
    reply.clearCookie('access_token', { path: '/' });
    reply.clearCookie('refresh_token', { path: '/v1/auth' });
    return reply.send({ ok: true });
  });
}
```

- [ ] **Step 6: Create `src/routes/v1/me.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { prisma } from '../../db/prisma.js';

export async function meRoutes(app: FastifyInstance) {
  app.get('/v1/me', { preHandler: [app.requireAuth] }, async (req) => {
    const customer = await prisma.customer.findUnique({
      where: { id: req.user.id },
      select: {
        id: true, email: true, name: true, company: true, country: true, createdAt: true,
        orders: { select: { id: true, status: true, amountCents: true, currency: true, createdAt: true } },
        licenses: { select: { id: true, key: true, status: true, createdAt: true } },
      },
    });
    return customer;
  });
}
```

- [ ] **Step 7: Modify `src/app.ts`** — register auth + routes

```ts
import Fastify, { type FastifyInstance } from 'fastify';
import { registerCommon } from './plugins/common.js';
import { registerRateLimit } from './plugins/rateLimit.js';
import { registerAuth } from './plugins/auth.js';
import { customersRoutes } from './routes/v1/customers.routes.js';
import { authRoutes } from './routes/v1/auth.routes.js';
import { meRoutes } from './routes/v1/me.routes.js';

export async function buildApp(): Promise<FastifyInstance> {
  const app = Fastify({
    logger: { level: process.env.LOG_LEVEL ?? 'info' },
    genReqId: () => crypto.randomUUID(),
    trustProxy: true,
  });

  await registerCommon(app);
  await registerRateLimit(app);
  await registerAuth(app);

  app.get('/health', async () => ({ status: 'ok', db: true }));
  await app.register(customersRoutes);
  await app.register(authRoutes);
  await app.register(meRoutes);
  return app;
}
```

- [ ] **Step 8: Create `test/integration/auth.test.ts`**

```ts
import { describe, it, expect, beforeAll, beforeEach, afterAll } from 'vitest';
import { buildApp } from '../../src/app.js';
import { resetDb, uniqueEmail } from '../helpers.js';
import { prisma } from '../../src/db/prisma.js';

describe('auth', () => {
  const app = await buildApp();
  beforeAll(async () => {});
  beforeEach(async () => { await resetDb(); });
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('registers a customer and returns an access token + cookie', async () => {
    const email = uniqueEmail();
    const res = await app.inject({
      method: 'POST', url: '/v1/customers',
      payload: { email, password: 'supersecret', name: 'Jane', country: 'US' },
    });
    expect(res.statusCode).toBe(201);
    const body = res.json();
    expect(body.email).toBe(email);
    expect(body.accessToken).toMatch(/^ey/);
    const cookie = res.headers['set-cookie'];
    expect(String(cookie)).toContain('refresh_token');
    expect(String(cookie)).toContain('HttpOnly');
  });

  it('rejects duplicate emails', async () => {
    const email = uniqueEmail();
    await app.inject({ method: 'POST', url: '/v1/customers', payload: { email, password: 'supersecret' } });
    const res = await app.inject({ method: 'POST', url: '/v1/customers', payload: { email, password: 'supersecret' } });
    expect(res.statusCode).toBe(409);
  });

  it('logs in with valid credentials', async () => {
    const email = uniqueEmail();
    await app.inject({ method: 'POST', url: '/v1/customers', payload: { email, password: 'supersecret' } });
    const res = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { email, password: 'supersecret' } });
    expect(res.statusCode).toBe(200);
    expect(res.json().accessToken).toMatch(/^ey/);
  });

  it('rejects invalid credentials', async () => {
    const res = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { email: uniqueEmail(), password: 'wrong' } });
    expect(res.statusCode).toBe(401);
  });

  it('rotates refresh token on /refresh', async () => {
    const email = uniqueEmail();
    const reg = await app.inject({ method: 'POST', url: '/v1/customers', payload: { email, password: 'supersecret' } });
    const cookies = parseCookies(reg.headers['set-cookie']);
    const refresh = cookies['refresh_token'];
    const res = await app.inject({ method: 'POST', url: '/v1/auth/refresh', cookies: { refresh_token: refresh } });
    expect(res.statusCode).toBe(200);
    // Old refresh token should now be revoked.
    const reused = await app.inject({ method: 'POST', url: '/v1/auth/refresh', cookies: { refresh_token: refresh } });
    expect(reused.statusCode).toBe(401);
  });

  it('requires auth for /v1/me', async () => {
    const res = await app.inject({ method: 'GET', url: '/v1/me' });
    expect(res.statusCode).toBe(401);
  });
});

function parseCookies(header: string | string[] | undefined): Record<string, string> {
  const out: Record<string, string> = {};
  const list = Array.isArray(header) ? header : header ? [header] : [];
  for (const h of list) {
    const [pair] = h.split(';');
    const [k, v] = pair.split('=');
    out[k.trim()] = decodeURIComponent(v ?? '');
  }
  return out;
}
```

- [ ] **Step 9: Run tests**

Run: `NODE_ENV=test npm test -- auth`
Expected: `6 passed`. (If rate limiting trips during the test run, raise `RATE_LIMIT_MAX`/`LOGIN_RATE_LIMIT_MAX` in the test shell env, e.g. `LOGIN_RATE_LIMIT_MAX=1000`.)

- [ ] **Step 10: Commit**

```bash
git add -A && git commit -m "feat(store): add JWT auth, customer accounts, login/refresh/logout"
```

---

## Task 7: Products route

**Files:**
- Create: `store-backend/src/routes/v1/products.routes.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/products.test.ts`

- [ ] **Step 1: Create `src/routes/v1/products.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { prisma } from '../../db/prisma.js';

export async function productsRoutes(app: FastifyInstance) {
  app.get('/v1/products/:slug', async (req, reply) => {
    const { slug } = req.params as { slug: string };
    const product = await prisma.product.findUnique({ where: { slug } });
    if (!product || !product.isActive) {
      return reply.code(404).send({ error: { code: 'NOT_FOUND', message: 'Product not found' } });
    }
    return {
      slug: product.slug,
      name: product.name,
      description: product.description,
      priceCents: product.priceCents,
      currency: product.currency,
      version: product.version,
      maxActivations: product.maxActivations,
      licensingTerms: `One perpetual license per purchase; valid on up to ${product.maxActivations} machines.`,
    };
  });
}
```

- [ ] **Step 2: Modify `src/app.ts`** — register `productsRoutes`

Add import and `await app.register(productsRoutes);`.

- [ ] **Step 3: Create `test/integration/products.test.ts`**

```ts
import { describe, it, expect, afterAll } from 'vitest';
import { buildApp } from '../../src/app.js';
import { prisma } from '../../src/db/prisma.js';

describe('GET /v1/products/:slug', () => {
  const app = await buildApp();
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('returns the More-Phi product with licensing terms', async () => {
    const res = await app.inject({ method: 'GET', url: '/v1/products/more-phi' });
    expect(res.statusCode).toBe(200);
    const body = res.json();
    expect(body.slug).toBe('more-phi');
    expect(body.priceCents).toBe(7900);
    expect(body.licensingTerms).toContain('3 machines');
  });

  it('returns 404 for unknown product', async () => {
    const res = await app.inject({ method: 'GET', url: '/v1/products/nope' });
    expect(res.statusCode).toBe(404);
  });
});
```

- [ ] **Step 4: Run tests**

Run: `NODE_ENV=test npm test -- products`
Expected: `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(store): add product info endpoint"
```

---

## Task 8: StripeService + OrderService + checkout route

**Files:**
- Create: `store-backend/src/services/OrderService.ts`, `store-backend/src/services/StripeService.ts`, `store-backend/src/routes/v1/checkout.routes.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/checkout.test.ts`

- [ ] **Step 1: Create `src/services/OrderService.ts`**

```ts
import type { PrismaClient, Order } from '@prisma/client';
import { prisma } from '../db/prisma.js';

export class OrderService {
  constructor(private readonly prisma: PrismaClient) {}

  async createPending(productId: string, customerId: string | null, amountCents: number, currency: string, sessionId: string): Promise<Order> {
    if (!customerId) throw new Error('Order requires a customer id');
    return this.prisma.order.create({
      data: { customerId, productId, amountCents, currency, stripeCheckoutSessionId: sessionId, status: 'PENDING' },
    });
  }

  findBySessionId(sessionId: string) {
    return this.prisma.order.findUnique({ where: { stripeCheckoutSessionId: sessionId } });
  }

  markPaid(orderId: string, paymentIntentId: string | null) {
    return this.prisma.order.update({ where: { id: orderId }, data: { status: 'PAID', paidAt: new Date(), stripePaymentIntentId: paymentIntentId } });
  }

  markFailed(orderId: string) {
    return this.prisma.order.update({ where: { id: orderId }, data: { status: 'FAILED' } });
  }

  markRefunded(orderId: string) {
    return this.prisma.order.update({ where: { id: orderId }, data: { status: 'REFUNDED', refundedAt: new Date() } });
  }
}

export const orderService = new OrderService(prisma);
```

- [ ] **Step 2: Create `src/services/StripeService.ts`**

```ts
import Stripe from 'stripe';
import type { Product } from '@prisma/client';
import { env } from '../config/env.js';

export class StripeService {
  private readonly stripe = new Stripe(env.STRIPE_SECRET_KEY);

  async createCheckoutSession(product: Pick<Product, 'name' | 'priceCents' | 'currency' | 'slug'>, orderId: string, email?: string): Promise<{ url: string; sessionId: string }> {
    const successUrl = env.STRIPE_SUCCESS_URL.replace('{ORDER_ID}', orderId);
    const session = await this.stripe.checkout.sessions.create({
      mode: 'payment',
      payment_method_types: ['card'],
      line_items: [
        {
          quantity: 1,
          price_data: {
            currency: product.currency,
            unit_amount: product.priceCents,
            product_data: { name: product.name },
          },
        },
      ],
      client_reference_id: orderId,
      ...(email ? { customer_email: email } : {}),
      success_url: successUrl,
      cancel_url: env.STRIPE_CANCEL_URL,
    });
    return { url: session.url!, sessionId: session.id };
  }

  constructEvent(rawBody: Buffer, signature: string | undefined): Stripe.Event {
    return this.stripe.webhooks.constructEvent(rawBody, signature ?? '', env.STRIPE_WEBHOOK_SECRET);
  }
}

export const stripeService = new StripeService();
```

- [ ] **Step 3: Create `src/routes/v1/checkout.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { prisma } from '../../db/prisma.js';
import { orderService } from '../../services/OrderService.js';
import { stripeService } from '../../services/StripeService.js';
import { customerService } from '../../services/CustomerService.js';

const checkoutSchema = z.object({
  productSlug: z.string().min(1),
  email: z.string().email().optional(),
});

export async function checkoutRoutes(app: FastifyInstance) {
  app.post('/v1/checkout', async (req, reply) => {
    const parsed = checkoutSchema.safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const { productSlug, email } = parsed.data;

    const product = await prisma.product.findUnique({ where: { slug: productSlug } });
    if (!product || !product.isActive) {
      return reply.code(404).send({ error: { code: 'NOT_FOUND', message: 'Product not found' } });
    }

    const resolvedEmail = email ?? (req.user ? (await customerService.getById(req.user.id))?.email : undefined);
    if (!resolvedEmail) {
      return reply.code(400).send({ error: { code: 'EMAIL_REQUIRED', message: 'Email is required to start checkout' } });
    }

    // Ensure a customer row exists so the order has a foreign key.
    const customer = await customerService.upsertByEmail(resolvedEmail);

    // Create the order first so the Stripe session can carry the real orderId
    // (used in success_url + fulfillment lookup). createPending accepts an
    // empty sessionId, which we stamp once Stripe returns one.
    const order = await orderService.createPending(product.id, customer.id, product.priceCents, product.currency, '');
    const { url, sessionId } = await stripeService.createCheckoutSession(product, order.id, resolvedEmail);
    await prisma.order.update({ where: { id: order.id }, data: { stripeCheckoutSessionId: sessionId } });
    return reply.send({ checkoutUrl: url, orderId: order.id });
  });
}
```

- [ ] **Step 4: Modify `src/app.ts`** — register `checkoutRoutes`

- [ ] **Step 5: Create `test/integration/checkout.test.ts`** (Stripe mocked)

```ts
import { describe, it, expect, beforeEach, afterAll, vi } from 'vitest';
import { buildApp } from '../../src/app.js';
import { resetDb, uniqueEmail } from '../helpers.js';
import { prisma } from '../../src/db/prisma.js';
import { stripeService } from '../../src/services/StripeService.js';

vi.spyOn(stripeService, 'createCheckoutSession').mockResolvedValue({
  url: 'https://checkout.stripe.com/c/test',
  sessionId: 'cs_test_123',
});

describe('POST /v1/checkout', () => {
  const app = await buildApp();
  beforeEach(async () => { await resetDb(); });
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('creates a PENDING order and returns a checkout URL', async () => {
    const res = await app.inject({
      method: 'POST', url: '/v1/checkout',
      payload: { productSlug: 'more-phi', email: uniqueEmail() },
    });
    expect(res.statusCode).toBe(200);
    const body = res.json();
    expect(body.checkoutUrl).toBe('https://checkout.stripe.com/c/test');
    expect(body.orderId).toBeTruthy();
    const order = await prisma.order.findUnique({ where: { id: body.orderId } });
    expect(order?.status).toBe('PENDING');
    expect(order?.stripeCheckoutSessionId).toBe('cs_test_123');
  });

  it('404s for an unknown product', async () => {
    const res = await app.inject({ method: 'POST', url: '/v1/checkout', payload: { productSlug: 'nope', email: uniqueEmail() } });
    expect(res.statusCode).toBe(404);
  });
});
```

- [ ] **Step 6: Run tests**

Run: `NODE_ENV=test npm test -- checkout`
Expected: `2 passed`.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(store): add Stripe checkout session + pending order creation"
```

---

## Task 9: EmailService

**Files:**
- Create: `store-backend/src/services/EmailService.ts`, `store-backend/test/unit/email.test.ts`

- [ ] **Step 1: Create `src/services/EmailService.ts`**

```ts
import { env } from '../config/env.js';
import { logger } from '../lib/logger.js';

export interface LicenseDeliveryPayload {
  licenseKey: string;
  downloadUrl: string;
  productName: string;
  setPasswordUrl?: string;
}

export interface EmailService {
  sendLicenseDelivery(to: string, payload: LicenseDeliveryPayload): Promise<void>;
}

class ConsoleEmailService implements EmailService {
  async sendLicenseDelivery(to: string, payload: LicenseDeliveryPayload) {
    logger.info({ to, key: mask(payload.licenseKey) }, 'license delivery email (console)');
  }
}

function mask(key: string): string {
  // Keep prefix + last group only.
  const parts = key.split('-');
  return parts.length > 1 ? `${parts[0]}-…-${parts[parts.length - 1]}` : '…';
}

class ResendEmailService implements EmailService {
  async sendLicenseDelivery(to: string, payload: LicenseDeliveryPayload) {
    const { Resend } = await import('resend');
    const resend = new Resend(env.RESEND_API_KEY);
    await resend.emails.send({
      from: env.EMAIL_FROM,
      to,
      subject: `Your ${payload.productName} license`,
      html: renderDeliveryHtml(payload),
    });
  }
}

class SmtpEmailService implements EmailService {
  async sendLicenseDelivery(to: string, payload: LicenseDeliveryPayload) {
    const nodemailer = await import('nodemailer');
    const transporter = nodemailer.createTransport({
      host: env.SMTP_HOST, port: env.SMTP_PORT, auth: { user: env.SMTP_USER, pass: env.SMTP_PASS },
    });
    await transporter.sendMail({
      from: env.EMAIL_FROM, to, subject: `Your ${payload.productName} license`,
      html: renderDeliveryHtml(payload),
    });
  }
}

function renderDeliveryHtml(p: LicenseDeliveryPayload): string {
  const setPasswordLine = p.setPasswordUrl
    ? `<p>Set your account password: <a href="${p.setPasswordUrl}">${p.setPasswordUrl}</a></p>`
    : '';
  return `
    <h1>Your ${p.productName} license</h1>
    <p>Thank you for your purchase!</p>
    <p><strong>License key:</strong> <code>${p.licenseKey}</code></p>
    <p><a href="${p.downloadUrl}">Download ${p.productName}</a></p>
    ${setPasswordLine}
  `;
}

export function createEmailService(): EmailService {
  switch (env.EMAIL_PROVIDER) {
    case 'resend': return new ResendEmailService();
    case 'smtp': return new SmtpEmailService();
    default: return new ConsoleEmailService();
  }
}

export const emailService = createEmailService();
```

- [ ] **Step 2: Create `test/unit/email.test.ts`**

```ts
import { describe, it, expect, vi } from 'vitest';
import { ConsoleEmailService } from '../../src/services/EmailService.js';

// Force console provider by constructing directly.
describe('ConsoleEmailService', () => {
  it('resolves without throwing and masks the key in logs', async () => {
    const svc = new ConsoleEmailService();
    const infoSpy = vi.spyOn(console, 'info').mockImplementation(() => {});
    await expect(svc.sendLicenseDelivery('jane@x.com', {
      licenseKey: 'MP-AAAA-BBBB-CCCC-DDDD',
      downloadUrl: 'https://x/dl', productName: 'More-Phi',
    })).resolves.toBeUndefined();
    infoSpy.mockRestore();
  });
});
```

- [ ] **Step 3: Run tests**

Run: `npm test -- email`
Expected: `1 passed`.

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "feat(store): add pluggable email service (console/resend/smtp)"
```

---

## Task 10: LicenseService + WebhookService + Stripe webhook route

**Files:**
- Create: `store-backend/src/services/LicenseService.ts`, `store-backend/src/services/WebhookService.ts`, `store-backend/src/routes/v1/webhooks.stripe.routes.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/webhook.test.ts`

- [ ] **Step 1: Create `src/services/LicenseService.ts`**

```ts
import type { PrismaClient, License } from '@prisma/client';
import { generateLicenseKey } from '../lib/licenseKey.js';
import { sha256Hex } from '../lib/hash.js';
import { prisma } from '../db/prisma.js';

export class LicenseService {
  constructor(private readonly prisma: PrismaClient) {}

  async issue(opts: { customerId: string; orderId: string; productId: string; maxActivations: number }): Promise<License> {
    // Retry on the astronomically-unlikely key/keyHash collision.
    for (let attempt = 0; attempt < 5; attempt++) {
      const key = generateLicenseKey();
      const keyHash = sha256Hex(key);
      try {
        return await this.prisma.license.create({
          data: {
            customerId: opts.customerId, orderId: opts.orderId, productId: opts.productId,
            key, keyHash, status: 'ACTIVE', maxActivations: opts.maxActivations,
          },
        });
      } catch (err: any) {
        if (err?.code === 'P2002') continue; // unique constraint → try another key
        throw err;
      }
    }
    throw new Error('Failed to generate a unique license key');
  }

  async findByKeyHash(licenseKey: string): Promise<License | null> {
    return this.prisma.license.findUnique({ where: { keyHash: sha256Hex(licenseKey) } });
  }

  async revoke(licenseId: string) {
    await this.prisma.$transaction([
      this.prisma.license.update({ where: { id: licenseId }, data: { status: 'REVOKED', revokedAt: new Date() } }),
      this.prisma.activation.updateMany({ where: { licenseId, status: 'ACTIVE' }, data: { status: 'DEACTIVATED' } }),
    ]);
  }

  async verify(licenseKey: string) {
    const license = await this.findByKeyHash(licenseKey);
    if (!license) return { valid: false, status: null, revokedAt: null };
    return { valid: license.status === 'ACTIVE', status: license.status, revokedAt: license.revokedAt };
  }
}

export const licenseService = new LicenseService(prisma);
```

- [ ] **Step 2: Create `src/services/WebhookService.ts`**

```ts
import type { PrismaClient } from '@prisma/client';
import type Stripe from 'stripe';
import { customerService } from './CustomerService.js';
import { orderService } from './OrderService.js';
import { licenseService } from './LicenseService.js';
import { emailService } from './EmailService.js';
import { prisma } from '../db/prisma.js';
import { env } from '../config/env.js';
import { logger } from '../lib/logger.js';

export class WebhookService {
  constructor(private readonly prisma: PrismaClient) {}

  async handleEvent(event: Stripe.Event): Promise<void> {
    // Idempotency: insert the event record first; if it exists, stop.
    try {
      await this.prisma.paymentEvent.create({
        data: {
          stripeEventId: event.id,
          type: event.type,
          status: 'RECEIVED',
          rawPayload: event as any,
        },
      });
    } catch (err: any) {
      if (err?.code === 'P2002') {
        logger.info({ eventId: event.id }, 'duplicate webhook event ignored');
        return; // already processed
      }
      throw err;
    }

    switch (event.type) {
      case 'checkout.session.completed':
        await this.fulfillCheckout(event);
        break;
      case 'payment_intent.payment_failed':
        await this.handlePaymentFailed(event);
        break;
      case 'charge.refunded':
        await this.handleRefund(event);
        break;
      default:
        logger.info({ type: event.type }, 'unhandled webhook event');
    }
  }

  private async fulfillCheckout(event: Stripe.Event) {
    const session = event.data.object as Stripe.Checkout.Session;
    const order = await orderService.findBySessionId(session.id);
    if (!order) {
      logger.warn({ sessionId: session.id }, 'checkout.completed for unknown session');
      return;
    }
    await this.prisma.paymentEvent.updateMany({
      where: { stripeEventId: event.id },
      data: { orderId: order.id, amountCents: order.amountCents, status: 'FULFILLED' },
    });
    const email = session.customer_email ?? session.customer_details?.email;
    if (!email) throw new Error('checkout session missing customer email');

    const customer = await customerService.upsertByEmail(email);
    // Attach the order to the (possibly newly created) customer if needed.
    if (order.customerId !== customer.id) {
      await this.prisma.order.update({ where: { id: order.id }, data: { customerId: customer.id } });
    }
    await orderService.markPaid(order.id, session.payment_intent as string | null);

    const product = await this.prisma.product.findUnique({ where: { id: order.productId } });
    const license = await licenseService.issue({
      customerId: customer.id, orderId: order.id, productId: order.productId,
      maxActivations: product?.maxActivations ?? env.LICENSE_MAX_ACTIVATIONS,
    });

    await emailService.sendLicenseDelivery(email, {
      licenseKey: license.key,
      downloadUrl: product?.downloadUrl ?? env.DOWNLOAD_URL_BASE,
      productName: product?.name ?? 'More-Phi',
      setPasswordUrl: customer.passwordHash ? undefined : `${env.CORS_ORIGIN}/set-password?token=${customer.id}`,
    });
  }

  private async handlePaymentFailed(event: Stripe.Event) {
    const pi = event.data.object as Stripe.PaymentIntent;
    const order = await this.prisma.order.findFirst({ where: { stripePaymentIntentId: pi.id } });
    if (order) await orderService.markFailed(order.id);
    await this.prisma.paymentEvent.updateMany({
      where: { stripeEventId: event.id },
      data: { orderId: order?.id, status: 'FAILED' },
    });
  }

  private async handleRefund(event: Stripe.Event) {
    const charge = event.data.object as Stripe.Charge;
    const order = await this.prisma.order.findFirst({ where: { stripePaymentIntentId: charge.payment_intent as string } });
    if (!order) return;
    await orderService.markRefunded(order.id);
    const licenses = await this.prisma.license.findMany({ where: { orderId: order.id } });
    for (const l of licenses) await licenseService.revoke(l.id);
    await this.prisma.paymentEvent.updateMany({
      where: { stripeEventId: event.id },
      data: { orderId: order.id, status: 'REFUNDED' },
    });
  }
}

export const webhookService = new WebhookService(prisma);
```

- [ ] **Step 3: Create `src/routes/v1/webhooks.stripe.routes.ts`** (raw body + signature verify; exempt from rate limit)

```ts
import type { FastifyInstance } from 'fastify';
import rawBody from '@fastify/raw-body';
import { stripeService } from '../../services/StripeService.js';
import { webhookService } from '../../services/WebhookService.js';
import { errorEnvelope } from '../../plugins/errorHandler.js';

export async function stripeWebhookRoutes(app: FastifyInstance) {
  // Register raw-body capture for signature verification.
  await app.register(rawBody, { runFirst: true });

  app.post('/v1/webhooks/stripe', { config: { rateLimit: false } }, async (req, reply) => {
    const sig = req.headers['stripe-signature'];
    let event;
    try {
      event = stripeService.constructEvent(Buffer.from(req.rawBody as string, 'utf8'), sig as string | undefined);
    } catch {
      return reply.code(400).send(errorEnvelope('INVALID_SIGNATURE', 'Webhook signature verification failed', 400));
    }
    await webhookService.handleEvent(event);
    return reply.send({ received: true });
  });
}
```

- [ ] **Step 4: Modify `src/app.ts`** — register `stripeWebhookRoutes` (before other routes is fine)

- [ ] **Step 5: Create `test/integration/webhook.test.ts`** (construct real signed events with the webhook secret)

```ts
import { describe, it, expect, beforeAll, afterAll, beforeEach } from 'vitest';
import Stripe from 'stripe';
import { buildApp } from '../../src/app.js';
import { resetDb, uniqueEmail } from '../helpers.js';
import { prisma } from '../../src/db/prisma.js';
import { env } from '../../src/config/env.js';
import { customerService } from '../../src/services/CustomerService.js';
import { orderService } from '../../src/services/OrderService.js';

const stripe = new Stripe(env.STRIPE_SECRET_KEY);

async function signedEvent(type: string, object: Record<string, unknown>): Promise<{ payload: string; signature: string }> {
  const event = { id: `evt_${Math.random().toString(36).slice(2)}`, object: 'event', type, data: { object }, created: 0 } as unknown as Stripe.Event;
  const payload = JSON.stringify(event);
  const signature = stripe.webhooks.generateTestHeaderString({ payload, secret: env.STRIPE_WEBHOOK_SECRET });
  return { payload, signature };
}

async function seedPaidOrder(email: string, sessionId: string) {
  const customer = await customerService.upsertByEmail(email);
  const product = await prisma.product.findUniqueOrThrow({ where: { slug: 'more-phi' } });
  const order = await orderService.createPending(product.id, customer.id, product.priceCents, product.currency, sessionId);
  return { order, product };
}

describe('Stripe webhook', () => {
  const app = await buildApp();
  beforeAll(async () => {});
  beforeEach(async () => { await resetDb(); });
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('fulfills checkout: marks order PAID, issues a license', async () => {
    const email = uniqueEmail();
    const { order, product } = await seedPaidOrder(email, 'cs_test_1');
    const { payload, signature } = await signedEvent('checkout.session.completed', {
      id: 'cs_test_1', customer_email: email, payment_intent: 'pi_1',
    });
    const res = await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload, headers: { 'stripe-signature': signature, 'content-type': 'application/json' } });
    expect(res.statusCode).toBe(200);
    const reloaded = await prisma.order.findUniqueOrThrow({ where: { id: order.id } });
    expect(reloaded.status).toBe('PAID');
    const license = await prisma.license.findFirst({ where: { orderId: order.id } });
    expect(license).not.toBeNull();
    expect(license!.maxActivations).toBe(product.maxActivations);
  });

  it('is idempotent: replaying the same event does not create a second license', async () => {
    const email = uniqueEmail();
    const { order } = await seedPaidOrder(email, 'cs_test_2');
    const ev = await signedEvent('checkout.session.completed', { id: 'cs_test_2', customer_email: email, payment_intent: 'pi_2' });
    await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload: ev.payload, headers: { 'stripe-signature': ev.signature, 'content-type': 'application/json' } });
    await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload: ev.payload, headers: { 'stripe-signature': ev.signature, 'content-type': 'application/json' } });
    const count = await prisma.license.count({ where: { orderId: order.id } });
    expect(count).toBe(1);
  });

  it('marks the order FAILED on payment_intent.payment_failed', async () => {
    const email = uniqueEmail();
    const { order } = await seedPaidOrder(email, 'cs_test_3');
    await prisma.order.update({ where: { id: order.id }, data: { stripePaymentIntentId: 'pi_fail' } });
    const ev = await signedEvent('payment_intent.payment_failed', { id: 'pi_fail' });
    const res = await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload: ev.payload, headers: { 'stripe-signature': ev.signature, 'content-type': 'application/json' } });
    expect(res.statusCode).toBe(200);
    const reloaded = await prisma.order.findUniqueOrThrow({ where: { id: order.id } });
    expect(reloaded.status).toBe('FAILED');
  });

  it('revokes the license on charge.refunded', async () => {
    const email = uniqueEmail();
    const { order } = await seedPaidOrder(email, 'cs_test_4');
    await prisma.order.update({ where: { id: order.id }, data: { stripePaymentIntentId: 'pi_refund', status: 'PAID' } });
    const license = await prisma.license.create({ data: { customerId: order.customerId, orderId: order.id, productId: order.productId, key: 'MP-TEST-REFN-DREV-OKE1', keyHash: 'h1', status: 'ACTIVE' } });
    const ev = await signedEvent('charge.refunded', { id: 'ch_1', payment_intent: 'pi_refund' });
    await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload: ev.payload, headers: { 'stripe-signature': ev.signature, 'content-type': 'application/json' } });
    const reloaded = await prisma.license.findUniqueOrThrow({ where: { id: license.id } });
    expect(reloaded.status).toBe('REVOKED');
  });

  it('rejects invalid signatures with 400', async () => {
    const res = await app.inject({ method: 'POST', url: '/v1/webhooks/stripe', payload: '{}', headers: { 'stripe-signature': 't=1,v1=bad', 'content-type': 'application/json' } });
    expect(res.statusCode).toBe(400);
  });
});
```

- [ ] **Step 6: Run tests**

Run: `NODE_ENV=test npm test -- webhook`
Expected: `5 passed`.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(store): add idempotent Stripe webhook fulfillment + license issuance"
```

---

## Task 11: ActivationService + license activation routes

**Files:**
- Create: `store-backend/src/services/ActivationService.ts`, `store-backend/src/routes/v1/licenses.routes.ts`, modify `store-backend/src/app.ts`, `store-backend/test/integration/activation.test.ts`

- [ ] **Step 1: Create `src/services/ActivationService.ts`**

```ts
import type { PrismaClient } from '@prisma/client';
import { sha256Hex } from '../lib/hash.js';
import { licenseService } from './LicenseService.js';

export interface ActivationMeta {
  hostname?: string;
  os?: string;
  appVersion?: string;
}

export class ActivationService {
  constructor(private readonly prisma: PrismaClient) {}

  async activate(licenseKey: string, machineFingerprint: string, meta: ActivationMeta) {
    const license = await licenseService.findByKeyHash(licenseKey);
    if (!license) {
      return { ok: false as const, code: 'LICENSE_NOT_FOUND', status: 404 };
    }
    if (license.status !== 'ACTIVE') {
      return { ok: false as const, code: 'LICENSE_REVOKED', status: 403 };
    }

    const fingerprintHash = sha256Hex(machineFingerprint);

    // Re-activate the same machine without consuming a new slot.
    const existing = await this.prisma.activation.findUnique({
      where: { licenseId_fingerprintHash: { licenseId: license.id, fingerprintHash } },
    });
    if (existing) {
      const updated = await this.prisma.activation.update({
        where: { id: existing.id },
        data: { status: 'ACTIVE', lastSeenAt: new Date(), ...meta },
      });
      return { ok: true as const, activationId: updated.id, licenseId: license.id, ...(await this.counts(license.id, license.maxActivations)) };
    }

    const activeCount = await this.prisma.activation.count({ where: { licenseId: license.id, status: 'ACTIVE' } });
    if (activeCount >= license.maxActivations) {
      return { ok: false as const, code: 'MAX_ACTIVATIONS_REACHED', status: 409, ...this.counts(license.id, license.maxActivations) };
    }

    const activation = await this.prisma.activation.create({
      data: { licenseId: license.id, fingerprintHash, status: 'ACTIVE', ...meta },
    });
    return { ok: true as const, activationId: activation.id, licenseId: license.id, ...(await this.counts(license.id, license.maxActivations)) };
  }

  async deactivate(licenseKey: string, machineFingerprint: string) {
    const license = await licenseService.findByKeyHash(licenseKey);
    if (!license) return { ok: false as const, code: 'LICENSE_NOT_FOUND', status: 404 };
    const fingerprintHash = sha256Hex(machineFingerprint);
    await this.prisma.activation.updateMany({
      where: { licenseId: license.id, fingerprintHash },
      data: { status: 'DEACTIVATED' },
    });
    return { ok: true as const };
  }

  private async counts(licenseId: string, maxActivations: number) {
    const activationsUsed = await this.prisma.activation.count({ where: { licenseId, status: 'ACTIVE' } });
    return { activationsUsed, maxActivations, status: 'ACTIVE' as const };
  }
}

export const activationService = new ActivationService(prisma);
```

- [ ] **Step 2: Create `src/routes/v1/licenses.routes.ts`**

```ts
import type { FastifyInstance } from 'fastify';
import { z } from 'zod';
import { activationService } from '../../services/ActivationService.js';
import { licenseService } from '../../services/LicenseService.js';
import { LIMITS } from '../../plugins/rateLimit.js';

const activateSchema = z.object({
  licenseKey: z.string().min(1).max(64),
  machineFingerprint: z.string().min(8).max(256),
  hostname: z.string().max(128).optional(),
  os: z.string().max(32).optional(),
  appVersion: z.string().max(32).optional(),
});

export async function licensesRoutes(app: FastifyInstance) {
  app.post('/v1/activations', { config: { rateLimit: LIMITS.activation } }, async (req, reply) => {
    const parsed = activateSchema.safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const result = await activationService.activate(parsed.data.licenseKey, parsed.data.machineFingerprint, parsed.data);
    if (!result.ok) {
      const { status, code } = result;
      return reply.code(status).send({ error: { code, message: code } });
    }
    const { ok, ...body } = result;
    return reply.send(body);
  });

  app.post('/v1/activations/deactivate', { config: { rateLimit: LIMITS.activation } }, async (req, reply) => {
    const parsed = activateSchema.pick({ licenseKey: true, machineFingerprint: true }).safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const result = await activationService.deactivate(parsed.data.licenseKey, parsed.data.machineFingerprint);
    if (!result.ok) {
      return reply.code(result.status).send({ error: { code: result.code, message: result.code } });
    }
    return reply.send({ ok: true });
  });

  app.get('/v1/licenses/verify', { config: { rateLimit: LIMITS.activation } }, async (req, reply) => {
    const key = (req.query as { key?: string }).key;
    if (!key) return reply.code(400).send({ error: { code: 'VALIDATION_ERROR', message: 'key is required' } });
    const result = await licenseService.verify(key);
    return reply.send(result);
  });
}
```

- [ ] **Step 3: Modify `src/app.ts`** — register `licensesRoutes`

- [ ] **Step 4: Create `test/integration/activation.test.ts`**

```ts
import { describe, it, expect, afterAll, beforeEach } from 'vitest';
import { buildApp } from '../../src/app.js';
import { resetDb, uniqueEmail } from '../helpers.js';
import { prisma } from '../../src/db/prisma.js';
import { customerService } from '../../src/services/CustomerService.js';
import { licenseService } from '../../src/services/LicenseService.js';

async function seedLicense(maxActivations = 2) {
  const email = uniqueEmail();
  const customer = await customerService.upsertByEmail(email);
  const product = await prisma.product.findUniqueOrThrow({ where: { slug: 'more-phi' } });
  const order = await prisma.order.create({ data: { customerId: customer.id, productId: product.id, amountCents: product.priceCents, currency: 'usd', stripeCheckoutSessionId: uniqueEmail(), status: 'PAID', paidAt: new Date() } });
  const license = await licenseService.issue({ customerId: customer.id, orderId: order.id, productId: product.id, maxActivations });
  return license;
}

describe('activation', () => {
  const app = await buildApp();
  beforeEach(async () => { await resetDb(); });
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('activates a valid license', async () => {
    const license = await seedLicense();
    const res = await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: license.key, machineFingerprint: 'machine-A-1234567890' } });
    expect(res.statusCode).toBe(200);
    const body = res.json();
    expect(body.activationsUsed).toBe(1);
    expect(body.maxActivations).toBe(2);
  });

  it('rejects an unknown license key', async () => {
    const res = await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: 'MP-NOPE-NOPE-NOPE-NOPE', machineFingerprint: 'machine-A-1234567890' } });
    expect(res.statusCode).toBe(404);
  });

  it('re-activating the same machine does not consume a new slot', async () => {
    const license = await seedLicense(1);
    const payload = { licenseKey: license.key, machineFingerprint: 'machine-same-12345678' };
    const r1 = await app.inject({ method: 'POST', url: '/v1/activations', payload });
    const r2 = await app.inject({ method: 'POST', url: '/v1/activations', payload });
    expect(r1.statusCode).toBe(200);
    expect(r2.statusCode).toBe(200);
    expect(r2.json().activationsUsed).toBe(1);
  });

  it('409s when max activations are exceeded', async () => {
    const license = await seedLicense(1);
    await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: license.key, machineFingerprint: 'machine-A-1234567890' } });
    const r2 = await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: license.key, machineFingerprint: 'machine-B-9876543210' } });
    expect(r2.statusCode).toBe(409);
  });

  it('deactivation frees a slot', async () => {
    const license = await seedLicense(1);
    await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: license.key, machineFingerprint: 'machine-A-1234567890' } });
    await app.inject({ method: 'POST', url: '/v1/activations/deactivate', payload: { licenseKey: license.key, machineFingerprint: 'machine-A-1234567890' } });
    const r = await app.inject({ method: 'POST', url: '/v1/activations', payload: { licenseKey: license.key, machineFingerprint: 'machine-B-9876543210' } });
    expect(r.statusCode).toBe(200);
  });

  it('verify reports revoked licenses as invalid', async () => {
    const license = await seedLicense();
    await licenseService.revoke(license.id);
    const res = await app.inject({ method: 'GET', url: `/v1/licenses/verify?key=${encodeURIComponent(license.key)}` });
    expect(res.statusCode).toBe(200);
    expect(res.json().valid).toBe(false);
    expect(res.json().status).toBe('REVOKED');
  });
});
```

- [ ] **Step 5: Run tests**

Run: `NODE_ENV=test npm test -- activation`
Expected: `6 passed`.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(store): add machine-bound license activation + verify"
```

---

## Task 12: Guest set-password flow + /me/licenses returns keys

**Files:**
- Create: `store-backend/src/services/SetPasswordTokenService.ts`, add `POST /v1/auth/set-password` to `store-backend/src/routes/v1/auth.routes.ts`, `store-backend/test/integration/setPassword.test.ts`

- [ ] **Step 1: Create `src/services/SetPasswordTokenService.ts`**

```ts
import { prisma } from '../db/prisma.js';
import { randomToken, sha256Hex } from '../lib/hash.js';
import { env } from '../config/env.js';

// A signed, single-use token bound to the customer id. We reuse the JWT secret
// to avoid an extra table: token = base64url(customerId) + '.' + hmac.
import { createHmac, timingSafeEqual } from 'node:crypto';

function sign(customerId: string): string {
  const sig = createHmac('sha256', env.JWT_SECRET).update(customerId).digest('hex');
  return `${Buffer.from(customerId).toString('base64url')}.${sig}`;
}

export function issueSetPasswordUrl(customerId: string): string {
  return `${env.CORS_ORIGIN}/set-password?token=${sign(customerId)}`;
}

export function verifySetPasswordToken(token: string): string | null {
  const [b64, sig] = token.split('.');
  if (!b64 || !sig) return null;
  const customerId = Buffer.from(b64, 'base64url').toString('utf8');
  const expected = createHmac('sha256', env.JWT_SECRET).update(customerId).digest('hex');
  const a = Buffer.from(sig);
  const b = Buffer.from(expected);
  if (a.length !== b.length || !timingSafeEqual(a, b)) return null;
  return customerId;
}

export const setPasswordTokenService = { issueSetPasswordUrl, verifySetPasswordToken };
```

- [ ] **Step 2: Add `POST /v1/auth/set-password` to `src/routes/v1/auth.routes.ts`**

Append inside `authRoutes`:

```ts
  app.post('/v1/auth/set-password', async (req, reply) => {
    const parsed = z.object({ token: z.string().min(1), password: z.string().min(8).max(128) }).safeParse(req.body);
    if (!parsed.success) throw parsed.error;
    const customerId = (await import('../../services/SetPasswordTokenService.js')).verifySetPasswordToken(parsed.data.token);
    if (!customerId) {
      return reply.code(401).send({ error: { code: 'INVALID_TOKEN', message: 'Invalid or expired token' } });
    }
    const customer = await customerService.getById(customerId);
    if (!customer) return reply.code(404).send({ error: { code: 'NOT_FOUND', message: 'Customer not found' } });
    await customerService.setPassword(customerId, parsed.data.password);
    const { accessToken } = await app.issueSession(reply, customer);
    return reply.send({ ok: true, accessToken });
  });
```

- [ ] **Step 3: Create `test/integration/setPassword.test.ts`**

```ts
import { describe, it, expect, afterAll, beforeEach } from 'vitest';
import { buildApp } from '../../src/app.js';
import { resetDb, uniqueEmail } from '../helpers.js';
import { prisma } from '../../src/db/prisma.js';
import { customerService } from '../../src/services/CustomerService.js';
import { setPasswordTokenService } from '../../src/services/SetPasswordTokenService.js';

describe('set-password (guest account)', () => {
  const app = await buildApp();
  beforeEach(async () => { await resetDb(); });
  afterAll(async () => { await app.close(); await prisma.$disconnect(); });

  it('lets a guest account set a password and log in', async () => {
    const email = uniqueEmail();
    const guest = await customerService.upsertByEmail(email); // no password
    expect(guest.passwordHash).toBeNull();
    const token = setPasswordTokenService.issueSetPasswordUrl(guest.id).split('token=')[1];
    const res = await app.inject({ method: 'POST', url: '/v1/auth/set-password', payload: { token, password: 'newpassword' } });
    expect(res.statusCode).toBe(200);
    const login = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { email, password: 'newpassword' } });
    expect(login.statusCode).toBe(200);
  });

  it('rejects a tampered token', async () => {
    const res = await app.inject({ method: 'POST', url: '/v1/auth/set-password', payload: { token: 'aaa.bbb', password: 'newpassword' } });
    expect(res.statusCode).toBe(401);
  });
});
```

- [ ] **Step 4: Run tests**

Run: `NODE_ENV=test npm test -- setPassword`
Expected: `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(store): add guest set-password flow"
```

---

## Task 13: Docker, README, full suite, build verification

**Files:**
- Create: `store-backend/Dockerfile`, `store-backend/docker-compose.yml`, `store-backend/README.md`, `store-backend/.dockerignore`

- [ ] **Step 1: Create `.dockerignore`**

```
node_modules
dist
.env
test
coverage
*.log
```

- [ ] **Step 2: Create `Dockerfile`**

```dockerfile
# ---- build ----
FROM node:20-bookworm-slim AS build
WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY prisma ./prisma
RUN npx prisma generate
COPY tsconfig.json ./
COPY src ./src
RUN npm run build

# ---- runtime ----
FROM node:20-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production
COPY package*.json ./
RUN npm ci --omit=dev
COPY --from=build /app/node_modules/.prisma ./node_modules/.prisma
COPY --from=build /app/node_modules/@prisma ./node_modules/@prisma
COPY --from=build /app/dist ./dist
COPY prisma ./prisma
EXPOSE 4000
CMD ["sh", "-c", "npx prisma migrate deploy && node dist/server.js"]
```

- [ ] **Step 3: Create `docker-compose.yml`**

```yaml
version: "3.9"
services:
  db:
    image: postgres:15-alpine
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: postgres
      POSTGRES_DB: morephi_store
    ports:
      - "5432:5432"
    volumes:
      - store-db:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres"]
      interval: 5s
      timeout: 5s
      retries: 10

  app:
    build: .
    depends_on:
      db:
        condition: service_healthy
    environment:
      NODE_ENV: production
      PORT: 4000
      DATABASE_URL: postgresql://postgres:postgres@db:5432/morephi_store
      JWT_SECRET: ${JWT_SECRET}
      STRIPE_SECRET_KEY: ${STRIPE_SECRET_KEY}
      STRIPE_WEBHOOK_SECRET: ${STRIPE_WEBHOOK_SECRET}
      STRIPE_SUCCESS_URL: ${STRIPE_SUCCESS_URL}
      STRIPE_CANCEL_URL: ${STRIPE_CANCEL_URL}
      CORS_ORIGIN: ${CORS_ORIGIN}
      EMAIL_PROVIDER: ${EMAIL_PROVIDER:-console}
      EMAIL_FROM: ${EMAIL_FROM}
    ports:
      - "4000:4000"

volumes:
  store-db:
```

- [ ] **Step 4: Create `README.md`** (setup + API docs + deploy)

```markdown
# More-Phi Store Backend

Sells the More-Phi VST3 plugin as a one-time Stripe purchase and issues machine-bound license keys.

## Stack
Node.js 20 · TypeScript · Fastify · Prisma · PostgreSQL · Stripe · Zod · Vitest · Docker

## Quick start (local)

1. Copy `.env.example` to `.env` and fill in `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET`, `JWT_SECRET` (32+ random chars).
2. `npm install`
3. Start Postgres: `docker compose up -d db` (or use a local instance).
4. `npm run prisma:migrate` (creates tables), `npm run prisma:seed` (creates the More-Phi product).
5. `npm run dev`

The server listens on `http://localhost:4000`.

## Stripe webhook (local testing)

```
stripe listen --forward-to localhost:4000/v1/webhooks/stripe
```
Copy the printed `whsec_...` into `STRIPE_WEBHOOK_SECRET`.

## Test

```
NODE_ENV=test npm test
```
Requires a `morephi_store_test` database (run `prisma migrate dev` against `DATABASE_URL_TEST`).

## API

### Products
- `GET /v1/products/:slug` — product + pricing + licensing terms

### Customers & auth
- `POST /v1/customers` — `{email, password, name?, company?, country?}`
- `POST /v1/auth/login` — `{email, password}` → `{accessToken}`
- `POST /v1/auth/refresh`, `POST /v1/auth/logout`, `POST /v1/auth/set-password`
- `GET /v1/me` — profile + orders + licenses (Bearer token)

### Checkout
- `POST /v1/checkout` — `{productSlug, email?}` → `{checkoutUrl, orderId}`

### Plugin-facing
- `POST /v1/activations` — `{licenseKey, machineFingerprint, hostname?, os?, appVersion?}`
- `POST /v1/activations/deactivate` — `{licenseKey, machineFingerprint}`
- `GET /v1/licenses/verify?key=...` — `{valid, status, revokedAt}`

### Internal
- `POST /v1/webhooks/stripe` (Stripe-signed)
- `GET /health`

All responses use `{error:{code,message}}` on failure. Login/checkout/activation are rate-limited.

## Deployment

- Build: `docker compose build && docker compose up -d`.
- Provide managed Postgres in production; set `DATABASE_URL` accordingly.
- In the Stripe dashboard, add a webhook endpoint → `https://<host>/v1/webhooks/stripe`, subscribing to `checkout.session.completed`, `payment_intent.payment_failed`, `charge.refunded`.
- Host the `MorePhi.vst3` binary on object storage (S3/R2); set `Product.downloadUrl` (or `DOWNLOAD_URL_BASE`).
- For multi-instance deploys, back `@fastify/rate-limit` with Redis (`redis` store option).
```

- [ ] **Step 5: Run the full suite**

Run: `NODE_ENV=test npm test`
Expected: all tests pass (health, errors, db, licenseKey, hash, rateLimit, auth, products, checkout, email, webhook, activation, setPassword).

- [ ] **Step 6: Verify the production build compiles**

Run: `npm run build`
Expected: `dist/` populated, no type errors. Fix any `tsc` errors (e.g., the `...this.counts(...)` spread noted in Task 11 must be `...(await this.counts(...))`).

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "feat(store): add Docker, docker-compose, and README"
```

---

## Definition of Done

- All endpoints from spec §5 implemented and tested.
- Stripe webhook verifies signatures and is idempotent; happy / failed / refund paths covered.
- License keys are Crockford Base32, ambiguity-free, unique; fingerprints stored only as hashes.
- Activation enforces `maxActivations`, allows same-machine reactivation, supports deactivation.
- Auth: bcryptjs passwords, JWT access + rotating refresh in httpOnly cookies, rate-limited login.
- Config validated at boot; secrets via env; structured logging with redaction.
- `npm run build` passes; Docker image builds and runs `prisma migrate deploy` + server.
