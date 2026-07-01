import Link from 'next/link'
import { XCircle } from 'lucide-react'

export const metadata = {
  title: 'Checkout cancelled — More-Phi',
}

export default function CheckoutCancelPage() {
  return (
    <main className="relative flex min-h-screen items-center justify-center overflow-hidden bg-background px-5 py-24">
      <div className="grid-noise pointer-events-none absolute inset-0 opacity-40" />
      <section
        className="glass-strong relative z-10 w-full max-w-2xl rounded-3xl p-8 text-center md:p-12"
        data-testid="checkout-cancel-panel"
      >
        <div
          className="mx-auto mb-6 flex size-16 items-center justify-center rounded-full border border-slate-line bg-background/60"
          data-testid="checkout-cancel-icon"
        >
          <XCircle className="size-8 text-muted-foreground" />
        </div>
        <p
          className="text-xs font-semibold uppercase tracking-[0.25em] text-muted-foreground"
          data-testid="checkout-cancel-kicker"
        >
          Stripe Checkout
        </p>
        <h1
          className="mt-4 font-heading text-3xl font-bold tracking-tight text-foreground"
          data-testid="checkout-cancel-title"
        >
          Checkout cancelled
        </h1>
        <p
          className="mx-auto mt-4 max-w-md text-sm leading-relaxed text-muted-foreground"
          data-testid="checkout-cancel-status"
        >
          Your payment was not completed and your card was not charged. You can
          try again whenever you are ready.
        </p>
        <div className="mt-8 flex flex-col justify-center gap-3 sm:flex-row">
          <Link
            href="/#checkout"
            className="rounded-full bg-primary px-6 py-3 text-sm font-bold text-primary-foreground transition-transform hover:scale-[1.02]"
            data-testid="checkout-cancel-retry-link"
          >
            Try again
          </Link>
          <Link
            href="/"
            className="glass rounded-full px-6 py-3 text-sm font-medium text-foreground transition-transform hover:scale-[1.02]"
            data-testid="checkout-cancel-home-link"
          >
            Back to Home
          </Link>
        </div>
      </section>
    </main>
  )
}
