# More-Phi VST3 Purchasing System PRD

## Original Problem Statement
Build a production-ready purchasing system for a VST3 plugin with product metadata, customer accounts, orders, license generation/delivery, Stripe Checkout/webhooks, payment transaction logging, license activations, API documentation, database schema, environment templates, and setup instructions.

## Architecture Decisions
- Frontend: existing Next.js landing page extended with secure checkout CTA, backend auth pages, dashboard, success polling page, and developer docs.
- Backend: FastAPI + MongoDB using Motor, JWT auth, passlib password hashing, strict CORS from environment, request logging, and rate limiting on login/register/checkout.
- Payments: Stripe Checkout via emergentintegrations; backend owns price/amount; frontend only sends origin_url; webhook and polling both idempotently provision licenses.
- Database: MongoDB collections for customers, orders, licenses, payment_transactions, payment_events, license_activations, email_events, and rate_limits.
- Email: SMTP-based production delivery path for license keys, with every attempt logged in email_events.

## Implemented
- Fixed purchase CTA so logged-out users are sent to signup/signin and then automatically redirected to Stripe Checkout.
- Updated Stripe environment to use the provided test Stripe account and added publishable key configuration for future embedded Stripe UI.
- REST API endpoints: health, product, register/login/me, orders, licenses, activation, checkout create/status, Stripe webhook, schema, docs text.
- Customer-facing UI: signup/signin, Stripe checkout initiation, checkout success polling, customer dashboard with orders/licenses and copy key UI.
- Developer output: README, env templates, schema endpoint, /developer-docs page, backend regression tests.
- Security: PCI-safe hosted checkout, no raw card handling, server-side pricing, JWT protection, idempotent provisioning, strict backend CORS, validation, rate limiting.

## Verification
- Added expanded QA scenarios and automated backend regression coverage; current suite passes 16/16.
- yarn build passes for frontend.
- backend py_compile passes.
- Backend regression suite: 11/11 tests passed.
- Browser checks verified landing, signin/signup, dashboard, and developer docs.

## Backlog
### P0
- Replace development JWT_SECRET before real production use.
- Configure SMTP provider credentials for live license emails.
- Configure real Stripe account/webhook endpoint and verify live webhook signing.

### P1
- Add admin reconciliation view for payment_events and failed email retries.
- Add customer self-service deactivation/reset for license activations.
- Add invoice/receipt download link from Stripe session metadata.

### P2
- Add coupon/launch pricing campaign management.
- Add analytics around checkout conversion and dashboard usage.
- Add DAW-specific activation telemetry charts.
