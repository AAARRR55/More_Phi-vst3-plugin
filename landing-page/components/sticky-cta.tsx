'use client'

import { useEffect, useState } from 'react'
import { api, getStoredCustomer, getToken } from '@/lib/api'

/**
 * Bottom-fixed purchase bar, mobile only (hidden at >= 640px). Appears once the
 * user scrolls past the hero, and hides again when the real #checkout section
 * is on screen so it never duplicates the primary buy button.
 *
 * Reuses the same auth → Stripe checkout flow as hero.tsx / checkout.tsx.
 */
export function StickyCta() {
  const [show, setShow] = useState(false)
  const [loading, setLoading] = useState(false)

  useEffect(() => {
    const hero = document.getElementById('hero')
    const checkout = document.getElementById('checkout')

    const update = () => {
      const heroBottom = hero?.getBoundingClientRect().bottom ?? 0
      const checkoutTop = checkout?.getBoundingClientRect().top ?? Infinity
      const viewportH = window.innerHeight
      // show once hero is scrolled past; hide when checkout is within viewport
      const pastHero = heroBottom < viewportH * 0.5
      const checkoutVisible = checkoutTop < viewportH * 0.8
      setShow(pastHero && !checkoutVisible)
    }

    update()
    window.addEventListener('scroll', update, { passive: true })
    window.addEventListener('resize', update)
    return () => {
      window.removeEventListener('scroll', update)
      window.removeEventListener('resize', update)
    }
  }, [])

  const startPurchase = async () => {
    if (!getToken()) {
      window.localStorage.setItem('more_phi_pending_checkout', '1')
      window.location.href = '/signup?checkout=1'
      return
    }
    setLoading(true)
    try {
      const customer = getStoredCustomer()
      if (!customer?.email) {
        window.location.href = '/signin?checkout=1'
        return
      }
      const session = await api.createCheckout({
        productSlug: 'more-phi',
        email: customer.email,
      })
      window.location.href = session.url
    } catch {
      setLoading(false)
    }
  }

  return (
    <div
      className={`fixed inset-x-0 bottom-0 z-40 sm:hidden ${
        show ? 'pointer-events-auto' : 'pointer-events-none'
      }`}
      aria-hidden={!show}
    >
      <div
        className={`mx-3 mb-3 transition-all duration-500 ${
          show ? 'translate-y-0 opacity-100' : 'translate-y-24 opacity-0'
        }`}
      >
        <div className="glass-strong gold-leaf flex items-center justify-between gap-3 rounded-2xl p-2 pl-4 shadow-[0_-8px_40px_-12px_oklch(0_0_0/0.6)]">
          <div className="flex flex-col leading-tight">
            <span className="font-heading text-sm font-bold text-foreground">
              Get More&#8211;Phi
            </span>
            <span className="text-[10px] text-muted-foreground">
              30-day refund &middot; lifetime updates
            </span>
          </div>
          <button
            id="sticky-cta-acquire"
            type="button"
            onClick={startPurchase}
            disabled={loading}
            aria-label="Get More-Phi for $79"
            className="group relative overflow-hidden rounded-full p-px"
          >
            <span className="aurora-border-fill absolute inset-0 rounded-full opacity-90 transition-opacity group-hover:opacity-100" />
            <span className="glass-strong relative flex items-center gap-1.5 rounded-full px-4 py-2.5 text-sm font-semibold text-foreground">
              {loading ? 'Opening...' : 'Get —'}
              <span className="text-primary">$79</span>
            </span>
          </button>
        </div>
      </div>
    </div>
  )
}
