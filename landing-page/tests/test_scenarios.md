# More-Phi QA Test Scenarios

## Automated Backend Regression

- `AUTH-01` signup creates a JWT customer account.
- `AUTH-02` duplicate signup returns `409`.
- `AUTH-03` login and `/api/auth/me` work with JWT.
- `AUTH-04` wrong password returns `401`.
- `AUTH-05` protected endpoints reject missing tokens.
- `AUTH-06` repeated failed login attempts return `429`.
- `PROD-01` `/api/product` returns More-Phi metadata, `$129`, `usd`, and license terms.
- `DOC-01` `/api/schema` returns customer/order/license/payment collection schema.
- `DOC-02` `/api/docs-text` returns human-readable API examples.
- `PAY-01` checkout creates a Stripe session and pending order.
- `PAY-02` checkout ignores frontend amount tampering and keeps server-side `$129`.
- `PAY-03` payment status endpoint requires authentication.
- `LIC-01` `/api/licenses` returns a customer-scoped list.
- `LIC-02` fake license activation is rejected.
- `SEC-01` customer API responses do not expose passwords or password hashes.
- `SEC-02` allowed CORS preflight succeeds.
- `SEC-03` disallowed CORS origin is rejected on the local backend.

Run:

```bash
NEXT_PUBLIC_API_URL=http://localhost:8001 CORS_TEST_ORIGIN=http://localhost:3000 pytest -q backend/tests/test_purchasing_api.py
```

## Manual Browser Scenarios

- Click header **Acquire — $129** while logged out → signup opens with checkout intent.
- Complete signup from checkout intent → Stripe Checkout opens automatically.
- Click hero **Acquire More-Phi $129** while logged out → signup opens with checkout intent.
- Click checkout card **Pay $129 with Stripe** while logged out → signup opens with checkout intent.
- Login first, then click any purchase button → Stripe Checkout opens directly.
- Open `/developer-docs` → API documentation visible.
- Open `/dashboard` logged out → redirected to signin.