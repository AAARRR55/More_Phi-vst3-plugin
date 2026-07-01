'use client'

import Link from 'next/link'
import { useRouter, useSearchParams } from 'next/navigation'
import { Suspense, useEffect, useState } from 'react'
import { CheckCircle2, Loader2, XCircle } from 'lucide-react'
import { api } from '@/lib/api'

function CheckoutSuccessContent() {
  const params = useSearchParams()
  const router = useRouter()
  const sessionId = params.get('session_id')
  const [status, setStatus] = useState('Checking payment status...')
  const [licenseKey, setLicenseKey] = useState('')
  const [done, setDone] = useState(false)
  const [failed, setFailed] = useState(false)

  // Auth note: the browser arrives here via a top-level redirect FROM Stripe
  // (a different site). The backend sets auth as `sameSite: "strict"` httpOnly
  // cookies, which browsers do NOT attach on cross-site navigations, so the
  // mp_auth cookie is absent on this initial page load. The status poll works
  // ONLY because the JWT is also kept in localStorage and lib/api.ts sends it
  // as `Authorization: Bearer` on every request (in addition to
  // `credentials: 'include'`). Do NOT refactor this to a cookie-only flow or
  // the order-status lookup will 401 for users coming back from Stripe.
  useEffect(() => {
    if (!sessionId) {
      setFailed(true)
      setStatus('Missing checkout session. Please return to pricing and try again.')
      return
    }

    let cancelled = false
    const poll = async (attempt = 0) => {
      try {
        const result = await api.checkoutStatus(sessionId)
        if (cancelled) return
        if (result.payment_status === 'paid') {
          setLicenseKey(result.license?.license_key || '')
          setStatus('Payment confirmed. Your license is ready.')
          setDone(true)
          return
        }
        if (result.status === 'expired' || attempt >= 5) {
          setFailed(true)
          setStatus('Payment was not completed. You can safely try checkout again.')
          return
        }
        setStatus('Payment is processing. Confirming with Stripe...')
        window.setTimeout(() => poll(attempt + 1), 2000)
      } catch (error) {
        if (cancelled) return
        const message = error instanceof Error ? error.message : 'Unable to verify payment.'
        if (message.includes('token')) {
          router.push('/signin')
        } else {
          setFailed(true)
          setStatus(message)
        }
      }
    }
    poll()
    return () => {
      cancelled = true
    }
  }, [router, sessionId])

  return (
    <main className="relative flex min-h-screen items-center justify-center overflow-hidden bg-background px-5 py-24">
      <div className="grid-noise pointer-events-none absolute inset-0 opacity-40" />
      <section className="glass-strong relative z-10 w-full max-w-2xl rounded-3xl p-8 text-center md:p-12" data-testid="checkout-success-panel">
        <div className="mx-auto mb-6 flex size-16 items-center justify-center rounded-full border border-slate-line bg-background/60" data-testid="checkout-success-icon">
          {done ? <CheckCircle2 className="size-8 text-cyan" /> : failed ? <XCircle className="size-8 text-destructive" /> : <Loader2 className="size-8 animate-spin text-primary" />}
        </div>
        <p className="text-xs font-semibold uppercase tracking-[0.25em] text-primary" data-testid="checkout-success-kicker">Stripe Checkout</p>
        <h1 className="mt-4 font-heading text-3xl font-bold tracking-tight text-foreground" data-testid="checkout-success-title">
          {done ? 'License provisioned' : failed ? 'Checkout needs attention' : 'Confirming payment'}
        </h1>
        <p className="mx-auto mt-4 max-w-md text-sm leading-relaxed text-muted-foreground" data-testid="checkout-success-status">{status}</p>
        {licenseKey && (
          <div className="mt-8 rounded-2xl border border-slate-line bg-black/30 p-4" data-testid="checkout-success-license-block">
            <p className="mb-2 text-xs uppercase tracking-widest text-muted-foreground">License Key</p>
            <code className="break-all font-mono text-sm text-cyan" data-testid="checkout-success-license-key">{licenseKey}</code>
          </div>
        )}
        <div className="mt-8 flex flex-col justify-center gap-3 sm:flex-row">
          <Link href="/dashboard" className="rounded-full bg-primary px-6 py-3 text-sm font-bold text-primary-foreground transition-transform hover:scale-[1.02]" data-testid="checkout-success-dashboard-link">
            Open Dashboard
          </Link>
          <Link href="/" className="glass rounded-full px-6 py-3 text-sm font-medium text-foreground transition-transform hover:scale-[1.02]" data-testid="checkout-success-home-link">
            Back to Home
          </Link>
        </div>
      </section>
    </main>
  )
}

export default function CheckoutSuccessPage() {
  return (
    <Suspense fallback={<main className="min-h-screen bg-background" data-testid="checkout-success-loading" />}>
      <CheckoutSuccessContent />
    </Suspense>
  )
}