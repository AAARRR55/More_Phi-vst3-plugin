'use client'

import { useState } from 'react'
import { useRouter } from 'next/navigation'
import { api, getStoredCustomer, getToken } from '@/lib/api'

const FEATURES = [
  'VST3 & AU — drop it into any modern DAW',
  'Runs heavy sessions without breaking a sweat — sub-2% CPU at 96 kHz',
  '12 snapshots you can cross-breed into sounds no preset offers',
  'A plain-language AI co-pilot that moves the knobs for you',
  'Lifetime updates, no subscription — ever',
  'One purchase covers every personal machine you own',
]

export function Checkout() {
  const router = useRouter()
  const [hovering, setHovering] = useState(false)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')

  const startCheckout = async () => {
    setError('')
    if (!getToken()) {
      window.localStorage.setItem('more_phi_pending_checkout', '1')
      window.location.href = '/signup?checkout=1'
      return
    }
    setLoading(true)
    try {
      const customer = getStoredCustomer()
      if (!customer?.email) {
        setError('Please sign in to continue checkout.')
        setLoading(false)
        return
      }
      const session = await api.createCheckout({ productSlug: 'more-phi', email: customer.email })
      window.location.href = session.url
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Unable to start checkout')
    } finally {
      setLoading(false)
    }
  }

  return (
    <section
      id="checkout"
      data-testid="checkout-section"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 py-24"
    >
      <div className="mx-auto mb-12 max-w-2xl text-center">
        <span className="text-xs font-medium tracking-[0.25em] text-primary uppercase">
          One Price. Forever.
        </span>
        <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance sm:text-4xl">
          Own the instrument.
        </h2>
      </div>

      <div className="gold-leaf glass-strong shimmer-sweep mx-auto grid max-w-4xl overflow-hidden rounded-3xl md:grid-cols-2">
        {/* Left: value */}
        <div className="border-b border-slate-line p-8 md:border-r md:border-b-0">
          <div
            className="inline-flex cursor-default items-baseline gap-2 transition-transform duration-300"
            onMouseEnter={() => setHovering(true)}
            onMouseLeave={() => setHovering(false)}
            style={{ transform: hovering ? 'scale(1.05)' : 'scale(1)' }}
          >
            <span
              className={`font-heading text-5xl font-bold transition-all ${
                hovering ? 'text-gradient-gold animate-gradient-pan' : 'text-foreground'
              }`}
            >
              $79
            </span>
            <span className="text-sm text-muted-foreground line-through">
              $129
            </span>
          </div>
          <p className="mt-2 text-sm text-muted-foreground">
            One-time purchase &middot; Launch pricing
          </p>

          <ul className="mt-8 space-y-3" id="checkout-feature-list">
            {FEATURES.map((f) => (
              <li key={f} className="flex items-start gap-3 text-sm">
                <span className="mt-0.5 flex size-4 shrink-0 items-center justify-center rounded-full bg-cyan/15">
                  <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="var(--color-cyan)" strokeWidth="3" aria-hidden="true">
                    <path d="M20 6 9 17l-5-5" />
                  </svg>
                </span>
                <span className="text-foreground/90">{f}</span>
              </li>
            ))}
          </ul>
        </div>

        {/* Right: mock checkout */}
        <div className="p-8">
          <h3 className="font-heading text-sm font-bold tracking-wide">
            Secure Checkout
          </h3>

          <div
            id="checkout-form"
            className="mt-5 space-y-4"
            data-testid="checkout-form"
          >
            <div className="rounded-2xl border border-slate-line bg-background/40 p-4" data-testid="checkout-pci-notice">
              <p className="text-xs font-semibold uppercase tracking-widest text-cyan">PCI-safe Stripe Checkout</p>
              <p className="mt-2 text-sm leading-relaxed text-muted-foreground">
                Create or sign in to your account, then Stripe opens automatically. More-Phi never handles raw card data.
              </p>
            </div>
            {error && (
              <p className="rounded-xl border border-destructive/20 bg-destructive/10 p-3 text-center text-xs text-destructive" data-testid="checkout-error-message">
                {error}
              </p>
            )}
            <button
              id="checkout-pay-btn"
              data-testid="checkout-pay-button"
              type="button"
              onClick={startCheckout}
              disabled={loading}
              className="shimmer-sweep group relative mt-2 w-full overflow-hidden rounded-full p-px"
            >
              <span className="aurora-border-fill absolute inset-0 rounded-full opacity-80 transition-opacity group-hover:opacity-100" />
              <span className="glass-strong relative flex items-center justify-center gap-2 rounded-full px-6 py-3 text-sm font-semibold text-foreground">
                {loading ? 'Opening Stripe...' : 'Get instant access — $79'}
              </span>
            </button>

            <p className="flex items-center justify-center gap-1.5 pt-1 text-center text-[11px] text-primary/80" data-testid="checkout-guarantee-copy">
              <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
                <path d="M12 2 4 5v6c0 5 3.5 8.5 8 11 4.5-2.5 8-6 8-11V5l-8-3Z" />
              </svg>
              30-day refund &middot; Lifetime updates &middot; No subscription
            </p>

            <p className="flex items-center justify-center gap-1.5 pt-2 text-center text-[11px] text-muted-foreground" data-testid="checkout-security-copy">
              <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
                <rect x="3" y="11" width="18" height="11" rx="2" />
                <path d="M7 11V7a5 5 0 0 1 10 0v4" />
              </svg>
              Secure redirect. Instant dashboard license after webhook confirmation.
            </p>
          </div>
        </div>
      </div>
    </section>
  )
}
