'use client'

import { useEffect, useState } from 'react'
import Link from 'next/link'
import { useRouter } from 'next/navigation'
import { Building2, Globe2, Mail, Lock, User, ArrowLeft, Eye, EyeOff } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { api } from '@/lib/api'

export default function SignUpPage() {
  const router = useRouter()
  const [name, setName] = useState('')
  const [email, setEmail] = useState('')
  const [company, setCompany] = useState('')
  const [country, setCountry] = useState('')
  const [password, setPassword] = useState('')
  const [confirmPassword, setConfirmPassword] = useState('')
  const [showPassword, setShowPassword] = useState(false)
  const [showConfirmPassword, setShowConfirmPassword] = useState(false)
  const [agreeTerms, setAgreeTerms] = useState(false)
  const [error, setError] = useState('')
  const [isLoading, setIsLoading] = useState(false)
  const [checkoutFlow, setCheckoutFlow] = useState(false)

  useEffect(() => {
    const params = new URLSearchParams(window.location.search)
    const isCheckout = params.get('checkout') === '1'
    setCheckoutFlow(isCheckout)
    if (!isCheckout) {
      window.localStorage.removeItem('more_phi_pending_checkout')
    }
  }, [])

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setError('')

    if (!name || !email || !password || !confirmPassword) {
      setError('Please fill in all fields.')
      return
    }

    if (password !== confirmPassword) {
      setError('Passwords do not match.')
      return
    }

    if (!agreeTerms) {
      setError('You must agree to the Terms of Service and Privacy Policy.')
      return
    }

    setIsLoading(true)

    try {
      await api.register({ name, email, password, company, country })
      if (checkoutFlow) {
        router.push('/signin?checkout=1')
        return
      }
      router.push('/signin?registered=1')
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to create an account. Please try again.')
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
        data-testid="signup-back-home-link"
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
            Create Account
          </h1>
          <p className="mt-2 text-sm text-muted-foreground">
            Start morphing, evolutionary breeding, and evolving your sound
          </p>
        </div>

        {/* Form */}
        <form onSubmit={handleSubmit} className="space-y-4" data-testid="signup-form">
          {error && (
            <div className="rounded-xl border border-destructive/20 bg-destructive/10 p-3 text-xs text-destructive text-center" data-testid="signup-error-message">
              {error}
            </div>
          )}

          {/* Full Name */}
          <div className="space-y-1">
            <label htmlFor="name" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
              Full Name
            </label>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <User className="size-4" />
              </span>
              <input
                id="name"
                data-testid="signup-name-input"
                type="text"
                placeholder="John Doe"
                value={name}
                onChange={(e) => setName(e.target.value)}
                className="w-full h-11 pl-10 pr-4 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                disabled={isLoading}
              />
            </div>
          </div>

          {/* Email */}
          <div className="space-y-1">
            <label htmlFor="email" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
              Email Address
            </label>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <Mail className="size-4" />
              </span>
              <input
                id="email"
                data-testid="signup-email-input"
                type="email"
                placeholder="name@example.com"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                className="w-full h-11 pl-10 pr-4 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                disabled={isLoading}
              />
            </div>
          </div>

          <div className="grid gap-3 sm:grid-cols-2">
            <div className="space-y-1">
              <label htmlFor="company" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
                Company
              </label>
              <div className="relative">
                <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                  <Building2 className="size-4" />
                </span>
                <input
                  id="company"
                  data-testid="signup-company-input"
                  type="text"
                  placeholder="Studio"
                  value={company}
                  onChange={(e) => setCompany(e.target.value)}
                  className="w-full h-11 pl-10 pr-4 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                  disabled={isLoading}
                />
              </div>
            </div>
            <div className="space-y-1">
              <label htmlFor="country" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
                Country
              </label>
              <div className="relative">
                <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                  <Globe2 className="size-4" />
                </span>
                <input
                  id="country"
                  data-testid="signup-country-input"
                  type="text"
                  placeholder="US"
                  value={country}
                  onChange={(e) => setCountry(e.target.value)}
                  className="w-full h-11 pl-10 pr-4 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                  disabled={isLoading}
                />
              </div>
            </div>
          </div>

          {/* Password */}
          <div className="space-y-1">
            <label htmlFor="password" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
              Password
            </label>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <Lock className="size-4" />
              </span>
              <input
                id="password"
                data-testid="signup-password-input"
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
                data-testid="signup-toggle-password-button"
                className="absolute inset-y-0 right-0 flex items-center pr-3.5 text-muted-foreground hover:text-foreground"
                disabled={isLoading}
              >
                {showPassword ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
              </button>
            </div>
          </div>

          {/* Confirm Password */}
          <div className="space-y-1">
            <label htmlFor="confirmPassword" className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
              Confirm Password
            </label>
            <div className="relative">
              <span className="absolute inset-y-0 left-0 flex items-center pl-3.5 pointer-events-none text-muted-foreground">
                <Lock className="size-4" />
              </span>
              <input
                id="confirmPassword"
                data-testid="signup-confirm-password-input"
                type={showConfirmPassword ? 'text' : 'password'}
                placeholder="••••••••"
                value={confirmPassword}
                onChange={(e) => setConfirmPassword(e.target.value)}
                className="w-full h-11 pl-10 pr-10 rounded-xl border border-slate-line bg-background/40 text-sm text-foreground placeholder-muted-foreground transition-all outline-none focus:border-cyan focus:ring-1 focus:ring-cyan"
                disabled={isLoading}
              />
              <button
                type="button"
                onClick={() => setShowConfirmPassword(!showConfirmPassword)}
                data-testid="signup-toggle-confirm-password-button"
                className="absolute inset-y-0 right-0 flex items-center pr-3.5 text-muted-foreground hover:text-foreground"
                disabled={isLoading}
              >
                {showConfirmPassword ? <EyeOff className="size-4" /> : <Eye className="size-4" />}
              </button>
            </div>
          </div>

          {/* Terms Agreement */}
          <div className="flex items-start gap-2.5 pt-1">
            <input
              id="agreeTerms"
              data-testid="signup-terms-checkbox"
              type="checkbox"
              checked={agreeTerms}
              onChange={(e) => setAgreeTerms(e.target.checked)}
              className="size-4 rounded border-slate-line bg-background/40 text-cyan accent-cyan focus:ring-cyan focus:ring-opacity-25 mt-0.5 cursor-pointer"
              disabled={isLoading}
            />
            <label htmlFor="agreeTerms" className="text-xs text-muted-foreground leading-normal">
              I agree to the{' '}
              <a href="#" className="text-cyan hover:underline">
                Terms of Service
              </a>{' '}
              and{' '}
              <a href="#" className="text-cyan hover:underline">
                Privacy Policy
              </a>
            </label>
          </div>

          {/* Submit */}
          <Button
            type="submit"
            data-testid="signup-submit-button"
            className="w-full h-11 rounded-xl bg-gradient-to-r from-cyan to-magenta hover:shadow-[0_0_24px_-4px_rgba(6,182,212,0.55)] text-foreground font-semibold tracking-wide border-none transition-all duration-300 cursor-pointer mt-2"
            disabled={isLoading}
          >
            {isLoading ? (
              <span className="flex items-center justify-center gap-2">
                <span className="size-4 animate-spin rounded-full border-2 border-foreground border-t-transparent" />
                Creating Account...
              </span>
            ) : (
              'Sign Up'
            )}
          </Button>
        </form>

        {/* Footer info */}
        <div className="mt-8 text-center text-sm text-muted-foreground">
          Already have an account?{' '}
          <Link href={checkoutFlow ? '/signin?checkout=1' : '/signin'} className="text-cyan font-medium hover:underline" data-testid="signup-signin-link">
            Sign In
          </Link>
        </div>
      </div>
    </div>
  )
}
