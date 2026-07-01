export type Customer = {
  id: string
  name: string
  email: string
  company?: string | null
  country?: string | null
  created_at: string
}

export type License = {
  id: string
  license_key: string
  product_id: string
  product_name?: string | null
  product_slug?: string | null
  download_url?: string | null
  status: string
  activation_limit: number
  created_at: string
  activations?: Array<{ id: string; machine_id: string; daw?: string; activated_at: string; status: string }>
}

export type Order = {
  id: string
  product_id: string
  amount: number
  currency: string
  status: string
  created_at: string
  checkout_session_id?: string
}

const API_URL = process.env.NEXT_PUBLIC_API_URL

export function getToken() {
  if (typeof window === 'undefined') return null
  return window.localStorage.getItem('more_phi_token')
}

export function setSession(token: string, customer: Customer) {
  window.localStorage.setItem('more_phi_token', token)
  window.localStorage.setItem('more_phi_customer', JSON.stringify(customer))
}

export function clearSession() {
  window.localStorage.removeItem('more_phi_token')
  window.localStorage.removeItem('more_phi_customer')
}

export function getStoredCustomer(): Customer | null {
  if (typeof window === 'undefined') return null
  const raw = window.localStorage.getItem('more_phi_customer')
  if (!raw) return null
  try {
    return JSON.parse(raw)
  } catch {
    return null
  }
}

async function request<T>(path: string, options: RequestInit = {}): Promise<T> {
  if (!API_URL) throw new Error('NEXT_PUBLIC_API_URL is not configured')

  const token = getToken()
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...(options.headers as Record<string, string>),
  }
  if (token) headers.Authorization = `Bearer ${token}`
  const response = await fetch(`${API_URL}${path}`, { ...options, headers, credentials: 'include' })

  const text = await response.text()
  const data = text ? JSON.parse(text) : null

  if (!response.ok) {
    const isAuthRoute = path === '/v1/auth/login' || path === '/v1/customers' || path.startsWith('/v1/auth/')
    if (response.status === 401 && typeof window !== 'undefined' && !isAuthRoute) {
      clearSession()
      window.location.href = '/signin'
    }
    const fieldErrors = data?.error?.details?.fieldErrors as Record<string, string[]> | undefined
    const formErrors = data?.error?.details?.formErrors as string[] | undefined
    let detail = ''
    if (fieldErrors && Object.keys(fieldErrors).length > 0) {
      detail = Object.entries(fieldErrors)
        .map(([field, errors]) => `${field}: ${errors.join(', ')}`)
        .join('; ')
    } else if (formErrors && formErrors.length > 0) {
      detail = formErrors.join('; ')
    }
    const message =
      detail ||
      data?.detail ||
      data?.message ||
      data?.error?.message ||
      data?.error?.detail ||
      'Request failed'
    throw new Error(message)
  }
  return data as T
}

function toCustomer(raw: Record<string, unknown>): Customer {
  return {
    id: String(raw.id ?? ''),
    name: String(raw.name ?? ''),
    email: String(raw.email ?? ''),
    company: raw.company != null ? String(raw.company) : null,
    country: raw.country != null ? String(raw.country) : null,
    created_at: raw.createdAt ? new Date(String(raw.createdAt)).toISOString() : '',
  }
}

function toLicense(raw: Record<string, unknown>): License {
  const activations = Array.isArray(raw.activations)
    ? raw.activations.map((a: Record<string, unknown>) => ({
        id: String(a.id ?? ''),
        machine_id: String(a.fingerprintHash ?? a.machine_id ?? ''),
        daw: a.os != null ? String(a.os) : a.daw != null ? String(a.daw) : undefined,
        activated_at: a.activatedAt
          ? new Date(String(a.activatedAt)).toISOString()
          : a.activated_at
            ? new Date(String(a.activated_at)).toISOString()
            : '',
        status: String(a.status ?? 'ACTIVE'),
      }))
    : []
  const product = raw.product && typeof raw.product === 'object' ? (raw.product as Record<string, unknown>) : null
  return {
    id: String(raw.id ?? ''),
    license_key: String(raw.license_key ?? raw.key ?? ''),
    product_id: String(raw.product_id ?? raw.productId ?? ''),
    product_name: product?.name != null ? String(product.name) : null,
    product_slug: product?.slug != null ? String(product.slug) : null,
    download_url: product?.downloadUrl != null ? String(product.downloadUrl) : null,
    status: String(raw.status ?? ''),
    activation_limit: Number(raw.activation_limit ?? raw.maxActivations ?? 0),
    created_at: raw.createdAt
      ? new Date(String(raw.createdAt)).toISOString()
      : raw.created_at
        ? new Date(String(raw.created_at)).toISOString()
        : '',
    activations,
  }
}

function toOrder(raw: Record<string, unknown>): Order {
  return {
    id: String(raw.id ?? ''),
    product_id: String(raw.product_id ?? raw.productId ?? ''),
    amount: Number(raw.amount ?? raw.amountCents ? (Number(raw.amountCents) / 100) : 0),
    currency: String(raw.currency ?? ''),
    status: String(raw.status ?? ''),
    created_at: raw.createdAt
      ? new Date(String(raw.createdAt)).toISOString()
      : raw.created_at
        ? new Date(String(raw.created_at)).toISOString()
        : '',
    checkout_session_id: raw.checkout_session_id != null
      ? String(raw.checkout_session_id)
      : raw.stripeCheckoutSessionId != null
        ? String(raw.stripeCheckoutSessionId)
        : undefined,
  }
}

export const api = {
  product: (slug = 'more-phi') =>
    request<Record<string, unknown>>(`/v1/products/${slug}`),

  register: async (body: { name: string; email: string; password: string; company?: string; country?: string }) => {
    await request<Record<string, unknown>>('/v1/customers', { method: 'POST', body: JSON.stringify(body) })
    const login = await api.login({ email: body.email, password: body.password })
    return login
  },

  login: async (body: { email: string; password: string }) => {
    const data = await request<{
      accessToken?: string
      access_token?: string
      customer?: Record<string, unknown>
    }>('/v1/auth/login', { method: 'POST', body: JSON.stringify(body) })
    const token = String(data.accessToken ?? data.access_token ?? '')
    const customer = data.customer ? toCustomer(data.customer) : null
    if (!token || !customer) {
      throw new Error('Invalid login response')
    }
    return { access_token: token, customer }
  },

  me: async () => {
    const data = await request<{ customer?: Record<string, unknown> }>('/v1/me')
    if (!data.customer) throw new Error('Invalid profile response')
    return toCustomer(data.customer)
  },

  createCheckout: (input: { productSlug: string; email: string }) =>
    request<{ checkoutUrl: string; orderId: string; sessionId: string }>('/v1/checkout', {
      method: 'POST',
      body: JSON.stringify(input),
    }).then((res) => ({ url: res.checkoutUrl, session_id: res.sessionId, order_id: res.orderId })),

  checkoutStatus: (sessionId: string) =>
    request<{
      status: string
      payment_status: string
      order?: Record<string, unknown>
      license?: Record<string, unknown>
      amount_total?: number
      currency?: string
    }>(`/v1/checkout/status/${sessionId}`).then((res) => ({
      status: res.status,
      payment_status: res.payment_status,
      order: res.order ? toOrder(res.order) : undefined,
      license: res.license ? toLicense(res.license) : undefined,
      amount_total: res.amount_total,
      currency: res.currency,
    })),

  orders: async () => {
    const data = await request<{ orders?: Record<string, unknown>[] }>('/v1/me')
    return (data.orders ?? []).map(toOrder)
  },

  licenses: async () => {
    const data = await request<{ licenses?: Record<string, unknown>[] }>('/v1/me')
    return (data.licenses ?? []).map(toLicense)
  },
}
