'use client'

import Link from 'next/link'
import { useRouter } from 'next/navigation'
import { useEffect, useState } from 'react'
import { Check, Copy, Download, KeyRound, LogOut, ReceiptText, ShieldCheck } from 'lucide-react'
import { api, clearSession, getStoredCustomer, type Customer, type License, type Order } from '@/lib/api'

export default function DashboardPage() {
  const router = useRouter()
  const [customer, setCustomer] = useState<Customer | null>(null)
  const [licenses, setLicenses] = useState<License[]>([])
  const [orders, setOrders] = useState<Order[]>([])
  const [loading, setLoading] = useState(true)
  const [copied, setCopied] = useState('')
  const [error, setError] = useState('')

  useEffect(() => {
    const stored = getStoredCustomer()
    if (!stored) {
      router.push('/signin')
      return
    }
    setCustomer(stored)
    Promise.all([api.licenses(), api.orders()])
      .then(([licenseList, orderList]) => {
        setLicenses(licenseList)
        setOrders(orderList)
      })
      .catch((err) => setError(err instanceof Error ? err.message : 'Unable to load dashboard'))
      .finally(() => setLoading(false))
  }, [router])

  const copyKey = async (key: string) => {
    await navigator.clipboard.writeText(key)
    setCopied(key)
    window.setTimeout(() => setCopied(''), 1800)
  }

  const signOut = () => {
    clearSession()
    router.push('/')
  }

  return (
    <main className="min-h-screen bg-background px-5 py-8 text-foreground">
      <div className="grid-noise pointer-events-none fixed inset-0 opacity-30" />
      <div className="relative z-10 mx-auto max-w-7xl">
        <header className="flex flex-col justify-between gap-4 border-b border-slate-line pb-6 md:flex-row md:items-center" data-testid="dashboard-header">
          <div>
            <Link href="/" className="font-heading text-sm font-bold tracking-[0.2em]" data-testid="dashboard-home-link">MORE-PHI</Link>
            <h1 className="mt-6 font-heading text-4xl font-bold tracking-tight" data-testid="dashboard-title">Customer control room</h1>
            <p className="mt-2 text-sm text-muted-foreground" data-testid="dashboard-customer-email">{customer?.email}</p>
          </div>
          <button onClick={signOut} className="glass flex items-center justify-center gap-2 rounded-full px-5 py-3 text-sm transition-transform hover:scale-[1.02]" data-testid="dashboard-signout-button">
            <LogOut className="size-4" /> Sign out
          </button>
        </header>

        {error && <p className="mt-6 border border-destructive/30 bg-destructive/10 p-4 text-sm text-destructive" data-testid="dashboard-error-message">{error}</p>}

        <section className="mt-8 grid gap-4 md:grid-cols-3" data-testid="dashboard-summary-grid">
          <Stat icon={<KeyRound className="size-5" />} label="Active licenses" value={licenses.length.toString()} testId="dashboard-license-count" />
          <Stat icon={<ReceiptText className="size-5" />} label="Orders" value={orders.length.toString()} testId="dashboard-order-count" />
          <Stat icon={<ShieldCheck className="size-5" />} label="Activations used" value={licenses.reduce((sum, item) => sum + (item.activations?.length || 0), 0).toString()} testId="dashboard-activation-count" />
        </section>

        <section className="mt-8 grid gap-6 lg:grid-cols-[1.35fr_0.65fr]">
          <div className="border border-slate-line bg-card/60" data-testid="dashboard-licenses-panel">
            <div className="border-b border-slate-line p-5">
              <h2 className="font-heading text-xl font-bold" data-testid="dashboard-licenses-title">License keys</h2>
              <p className="mt-1 text-sm text-muted-foreground">Copy keys for activation in your VST3/AU plugin host.</p>
            </div>
            {loading ? (
              <p className="p-5 text-sm text-muted-foreground" data-testid="dashboard-loading-message">Loading licenses...</p>
            ) : licenses.length === 0 ? (
              <div className="p-8" data-testid="dashboard-empty-licenses">
                <p className="text-sm text-muted-foreground">No license has been issued yet.</p>
                <Link href="/#checkout" className="mt-4 inline-flex rounded-full bg-primary px-5 py-3 text-sm font-bold text-primary-foreground" data-testid="dashboard-buy-link">Buy More-Phi</Link>
              </div>
            ) : (
              <div className="divide-y divide-slate-line">
                {licenses.map((license) => (
                  <article key={license.id} className="p-5" data-testid={`dashboard-license-${license.id}`}>
                    <div className="flex flex-col justify-between gap-4 md:flex-row md:items-center">
                      <div>
                        <p className="font-mono text-xs uppercase tracking-widest text-muted-foreground">{license.product_name || license.product_id}</p>
                        <code className="mt-2 block break-all font-mono text-sm text-cyan" data-testid={`dashboard-license-key-${license.id}`}>{license.license_key}</code>
                      </div>
                      <div className="flex flex-wrap items-center gap-2">
                        {license.download_url && (
                          <a
                            href={license.download_url}
                            target="_blank"
                            rel="noopener noreferrer"
                            className="glass flex items-center justify-center gap-2 rounded-full px-4 py-2 text-sm hover:text-cyan"
                            data-testid={`dashboard-download-license-${license.id}`}
                          >
                            <Download className="size-4" />
                            Download VST3
                          </a>
                        )}
                        <button onClick={() => copyKey(license.license_key)} className="glass flex items-center justify-center gap-2 rounded-full px-4 py-2 text-sm" data-testid={`dashboard-copy-license-${license.id}`}>
                          {copied === license.license_key ? <Check className="size-4 text-cyan" /> : <Copy className="size-4" />}
                          {copied === license.license_key ? 'Copied' : 'Copy'}
                        </button>
                      </div>
                    </div>
                    <div className="mt-4 grid gap-2 text-xs text-muted-foreground sm:grid-cols-3" data-testid={`dashboard-license-meta-${license.id}`}>
                      <span>Status: {license.status}</span>
                      <span>Activation limit: {license.activation_limit}</span>
                      <span>Used: {license.activations?.length || 0}</span>
                    </div>
                  </article>
                ))}
              </div>
            )}
          </div>

          <div className="border border-slate-line bg-card/60" data-testid="dashboard-orders-panel">
            <div className="border-b border-slate-line p-5">
              <h2 className="font-heading text-xl font-bold" data-testid="dashboard-orders-title">Purchase history</h2>
            </div>
            <div className="divide-y divide-slate-line">
              {orders.length === 0 ? (
                <p className="p-5 text-sm text-muted-foreground" data-testid="dashboard-empty-orders">No orders yet.</p>
              ) : orders.map((order) => (
                <article key={order.id} className="p-5" data-testid={`dashboard-order-${order.id}`}>
                  <div className="flex items-center justify-between gap-3">
                    <p className="font-mono text-xs text-muted-foreground">{order.id.slice(0, 8)}</p>
                    <span className="rounded-full border border-slate-line px-3 py-1 text-xs text-cyan" data-testid={`dashboard-order-status-${order.id}`}>{order.status}</span>
                  </div>
                  <p className="mt-3 text-sm" data-testid={`dashboard-order-amount-${order.id}`}>${order.amount.toFixed(2)} {order.currency.toUpperCase()}</p>
                  <p className="mt-1 text-xs text-muted-foreground" data-testid={`dashboard-order-date-${order.id}`}>{new Date(order.created_at).toLocaleString()}</p>
                </article>
              ))}
            </div>
          </div>
        </section>
      </div>
    </main>
  )
}

function Stat({ icon, label, value, testId }: { icon: React.ReactNode; label: string; value: string; testId: string }) {
  return (
    <div className="border border-slate-line bg-card/60 p-5" data-testid={testId}>
      <div className="flex items-center gap-3 text-cyan">{icon}<span className="text-xs uppercase tracking-widest text-muted-foreground">{label}</span></div>
      <p className="mt-5 font-heading text-4xl font-bold">{value}</p>
    </div>
  )
}