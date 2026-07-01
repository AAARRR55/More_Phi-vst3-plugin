'use client'

import { useEffect, useState } from 'react'
import Link from 'next/link'
import { clearSession, getStoredCustomer, type Customer } from '@/lib/api'

const NAV = [
  { id: 'nav-demo', label: 'Morph Pad', href: '#morph-demo' },
  { id: 'nav-features', label: 'Engine', href: '#features' },
  { id: 'nav-pricing', label: 'Pricing', href: '#checkout' },
  { id: 'nav-specs', label: 'Specs', href: '#specs' },
]

export function SiteHeader() {
  const [scrolled, setScrolled] = useState(false)
  const [user, setUser] = useState<Customer | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const onScroll = () => setScrolled(window.scrollY > 24)
    onScroll()
    window.addEventListener('scroll', onScroll, { passive: true })
    
    setUser(getStoredCustomer())
    setLoading(false)
    
    return () => {
      window.removeEventListener('scroll', onScroll)
    }
  }, [])

  const handleSignOut = async () => {
    try {
      clearSession()
      setUser(null)
    } catch (error) {
      console.error('Failed to sign out:', error)
    }
  }

  return (
    <header
      id="site-header"
      className={`fixed inset-x-0 top-0 z-50 transition-all duration-500 ${
        scrolled ? 'py-2' : 'py-4'
      }`}
    >
      <div className="mx-auto flex max-w-7xl items-center justify-between px-5">
        <a
          id="brand-logo-link"
          href="#hero"
          data-testid="brand-logo-link"
          className={`flex items-center gap-2.5 rounded-full px-3 py-2 transition-all ${
            scrolled ? 'glass' : ''
          }`}
        >
          <span className="relative flex size-6 items-center justify-center">
            <span className="animate-pulse-ring absolute inset-0 rounded-full bg-cyan/30 blur-[6px]" />
            <span className="relative size-3 rounded-full bg-gradient-to-tr from-cyan to-magenta" />
          </span>
          <span className="font-heading text-sm font-bold tracking-[0.2em] text-foreground">
            MORE&#8211;PHI
          </span>
        </a>

        <nav
          id="primary-nav"
          aria-label="Primary"
          className="glass hidden items-center gap-1 rounded-full px-2 py-1.5 md:flex"
        >
          {NAV.map((item) => (
            <a
              key={item.id}
              id={item.id}
              data-testid={item.id}
              href={item.href}
              className="rounded-full px-4 py-1.5 text-sm text-muted-foreground transition-colors hover:text-foreground"
            >
              {item.label}
            </a>
          ))}
        </nav>

        <div className="flex items-center gap-4">
          {!loading && (
            <>
              {user ? (
                <div className="flex items-center gap-3.5">
                  <span className="hidden text-xs text-muted-foreground sm:inline-block">
                    Hi, <span className="font-medium text-foreground" data-testid="header-customer-name">{user.name || user.email.split('@')[0]}</span>
                  </span>
                  <Link
                    href="/dashboard"
                    className="text-sm font-medium text-cyan transition-colors hover:text-foreground"
                    data-testid="header-dashboard-link"
                  >
                    Dashboard
                  </Link>
                  <button
                    id="header-signout"
                    data-testid="header-signout-button"
                    onClick={handleSignOut}
                    className="text-sm font-medium text-muted-foreground transition-colors hover:text-foreground cursor-pointer"
                  >
                    Sign Out
                  </button>
                </div>
              ) : (
                <Link
                  id="header-signin"
                  href="/signin"
                  data-testid="header-signin-link"
                  className="text-sm font-medium text-muted-foreground transition-colors hover:text-foreground"
                >
                  Sign In
                </Link>
              )}
            </>
          )}
        </div>
      </div>
    </header>
  )
}
