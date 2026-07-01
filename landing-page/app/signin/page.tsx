'use client'

import { useEffect, useState } from 'react'
import Link from 'next/link'
import { useRouter } from 'next/navigation'
import { Mail, Lock, ArrowLeft, Eye, EyeOff } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { api, setSession } from '@/lib/api'

export default function SignInPage() {
  const router = useRouter()
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [showPassword, setShowPassword] = useState(false)
  const [error, setError] = useState('')
  const [isLoading, setIsLoading] = useState(false)
  const [registered, setRegistered] = useState(false)
  const [checkoutFlow, setCheckoutFlow] = useState(false)

  useEffect(() => {
    const params = new URLSearchParams(window.location.search)
    const isCheckout = params.get('checkout') === '1'
    setCheckoutFlow(isCheckout)
    if (!isCheckout) {
      window.localStorage.removeItem('more_phi_pending_checkout')
    }
    if (params.get('registered') === '1') {
      setRegistered(true)
    }
  }, [])

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setError('')

    if (!email || !password) {
      setError('Please fill in all fields.')
      return
    }

    setIsLoading(true)

    try {
      const result = await api.login({ email, password })
      setSession(result.access_token, result.customer)
      const continueToCheckout = checkoutFlow || window.localStorage.getItem('more_phi_pending_checkout') === '1'
      if (continueToCheckout) {
        window.localStorage.removeItem('more_phi_pending_checkout')
        try {
          const checkout = await api.createCheckout({ productSlug: 'more-phi', email: result.customer.email })
          window.location.href = checkout.url
          return
        } catch (checkoutErr) {
          router.push('/')
          return
        }
      }
      router.push('/')
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to sign in. Please check your credentials.')
    } finally {
      setIsLoading(false)
    }
  }

  return (
    <div className="relative min-h-screen flex items-center justify-center p-5 overflow-hidden bg-background">
      {/* Background Gradients */}
      <div className="pointer-events-none absolute inset-0 grid-noise opacity-40" />
      <div className="pointer-events-none absolute top-[-10%] left-1/2 -translate-x-1/2 w-[80%] h-[50%] rounded-full bg-cyan/10 blur-[120px]" />
      <div className="pointer-events-none absolute bottom-[-10%] right-[10%] w-[40%] h-[40%] rounded-full bg-magenta/5 blur-[100px]" />

      {/* Back to Home Button */}
      <Link
        href="/"
        data-testid="signin-back-home-link"
        className="glass absolute top-6 left-6 flex items-center gap-2 rounded-full px-4 py-2 text-xs font-medium text-muted-foreground transition-all hover:text-foreground hover:scale-[1.02]"
      >
        <ArrowLeft className="size-3.5" />
        Back to Home
      </Link>

      {/* Card */}
      <div className="glass-strong relative w-full max-w-md rounded-3xl p-8 md:p-10 z-10">
        {/* Glowing Logo */}
        <div className="flex flex-col items-center text-center mb-8">
          <Link href="/" className="flex items-center gap-2.5 mb-4 group">
            <span className="relative flex size-8 items-center justify-center transition-transform group-hover:scale-110">
              <span className="absolute inset-0 rounded-full bg-cyan/30 blur-[8px]" />
              <span className="relative size-4 rounded-full bg-gradient-to-tr from-cyan to-magenta" />
            </span>
            <span className="font-heading text-base font-bold tracking-[0.25em] text-foreground">
              MORE&#8211;PHI
            </span>
          </Link>
          <h1 className="font-display text-2xl font-bold tracking-tight text-foreground">
            Welcome Back
          </h1>
          <p className="mt-2 text-sm text-muted-foreground">
            Access your parameter morphing license and presets
          </p>
        </div>

        {/* Form */}
        <form onSubmit={handleSubmit} className="space-y-5" data-testid="signin-form">
          {registered && (
            <div className="rounded-xl border border-cyan/20 bg-cyan/10 p-3 text-xs text-cyan text-center" data-testid="signin-registered-message">
              Account created. Please sign in.
            </div>
          )}

          {error && (
            <div className="rounded-xl border border-destructive/20 bg-destructive/10 p-3 text-xs text-destructive text-center" data-testid="signin-error-message">
              {error}
            </div>
          )}

          {/* Email */}
          <div className="space-y-1.5">
            <label htmlFor="email" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
              Email Address
            </label>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <Mail className="size-4" />
              </span>
              <input
                id="email"
                data-testid="signin-email-input"
                type="email"
                placeholder="name@example.com"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                className="w-full h-11 pl-10 pr-4 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                disabled={isLoading}
              />
            </div>
          </div>

          {/* Password */}
          <div className="space-y-1.5">
            <div className="flex items-center justify-between">
              <label htmlFor="password" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
                Password
              </label>
              <a href="#" className="text-xs text-cyan hover:underline">
                Forgot password?
              </a>
            </div>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <Lock className="size-4" />
              </span>
              <input
                id="password"
                data-testid="signin-password-input"
                type={showPassword ? 'text' : 'password'}
                placeholder="••••••••"
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                className="w-full h-11 pl-10 pr-10 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                disabled={isLoading}
              />
              <button
                type="button"
                onClick={() => setShowPassword(!showPassword)}
                data-testid="signin-toggle-password-button"
                className="absolute inset-y-0 right-0 flex items-center pr-3.5 text-muted-foreground hover:text-foreground"
                disabled={isLoading}
              >
                {showPassword ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
              </button>
            </div>
          </div>

          {/* Submit */}
          <Button
            type="submit"
            data-testid="signin-submit-button"
            className="w-full h-11 rounded-xl bg-gradient-to-r from-cyan to-magenta hover:shadow-[0_0_24px_-4px_rgba(6,182,212,0.55)] text-foreground font-semibold tracking-wide border-none transition-all duration-300 cursor-pointer"
            disabled={isLoading}
          >
            {isLoading ? (
              <span className="flex items-center justify-center gap-2">
                <span className="size-4 animate-spin rounded-full border-2 border-foreground border-t-transparent" />
                Signing In...
              </span>
            ) : (
              'Sign In'
            )}
          </Button>
        </form>

        {/* Footer info */}
        <div className="mt-8 text-center text-sm text-muted-foreground">
          Don't have an account?{' '}
          <Link href={checkoutFlow ? '/signup?checkout=1' : '/signup'} className="text-cyan font-medium hover:underline" data-testid="signin-signup-link">
            Sign Up
          </Link>
        </div>
      </div>
    </div>
  )
}
