import Link from 'next/link'

const endpoints = [
  ['GET', '/api/product', 'Retrieve plugin metadata, price, licensing terms, and supported platforms.'],
  ['POST', '/api/auth/register', 'Create a customer profile and return a JWT bearer token.'],
  ['POST', '/api/auth/login', 'Authenticate a customer with rate limiting.'],
  ['POST', '/api/payments/checkout', 'Create a Stripe Checkout session using server-side pricing only.'],
  ['GET', '/api/payments/checkout/status/{session_id}', 'Poll Stripe status, reconcile transaction, and provision license once.'],
  ['POST', '/api/webhook/stripe', 'Verify Stripe webhook events and idempotently generate licenses.'],
  ['GET', '/api/licenses', 'Return active customer licenses and activation records.'],
  ['POST', '/api/licenses/activate', 'Register a machine activation against an owned license.'],
]

export default function DeveloperDocsPage() {
  return (
    <main className="min-h-screen bg-background px-5 py-10 text-foreground">
      <div className="mx-auto max-w-6xl">
        <Link href="/" className="text-sm text-cyan hover:underline" data-testid="developer-docs-home-link">Back to More-Phi</Link>
        <section className="mt-10 border border-slate-line bg-card/70 p-8" data-testid="developer-docs-hero">
          <p className="font-mono text-xs uppercase tracking-[0.25em] text-primary">Production API</p>
          <h1 className="mt-4 font-heading text-4xl font-bold tracking-tight">Purchasing, payments, licensing.</h1>
          <p className="mt-5 max-w-3xl text-sm leading-7 text-muted-foreground">
            FastAPI + MongoDB backend with JWT accounts, Stripe Checkout, webhook reconciliation, license delivery, payment-event logging, activation tracking, and rate limits on sensitive endpoints.
          </p>
        </section>

        <section className="mt-8 grid border border-slate-line" data-testid="developer-docs-endpoints">
          {endpoints.map(([method, path, description]) => (
            <div key={path} className="grid gap-3 border-b border-slate-line p-4 last:border-b-0 md:grid-cols-[100px_1fr_1.4fr]" data-testid={`developer-docs-endpoint-${path.replaceAll('/', '-').replace(/[{}]/g, '')}`}>
              <span className="font-mono text-xs font-bold text-primary">{method}</span>
              <code className="font-mono text-sm text-cyan">{path}</code>
              <p className="text-sm text-muted-foreground">{description}</p>
            </div>
          ))}
        </section>

        <section className="mt-8 grid gap-6 lg:grid-cols-2">
          <div className="border border-slate-line bg-card/70 p-6" data-testid="developer-docs-register-example">
            <h2 className="font-heading text-xl font-bold">Example: register</h2>
            <pre className="mt-4 overflow-x-auto bg-black/40 p-4 font-mono text-xs text-muted-foreground">{`POST /api/auth/register
{
  "name": "Ada Producer",
  "email": "ada@example.com",
  "password": "securePass123",
  "company": "Studio A",
  "country": "US"
}`}</pre>
          </div>
          <div className="border border-slate-line bg-card/70 p-6" data-testid="developer-docs-checkout-example">
            <h2 className="font-heading text-xl font-bold">Example: checkout</h2>
            <pre className="mt-4 overflow-x-auto bg-black/40 p-4 font-mono text-xs text-muted-foreground">{`POST /api/payments/checkout
Authorization: Bearer <token>
{
  "origin_url": "https://your-store.example"
}

Response: { "url": "https://checkout.stripe.com/..." }`}</pre>
          </div>
        </section>
      </div>
    </main>
  )
}