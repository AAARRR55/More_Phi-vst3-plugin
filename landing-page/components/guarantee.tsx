/**
 * Risk-reversal strip — sits directly above the checkout section to remove
 * the last purchase anxieties (refund, subscription trap, online-DRM, future
 * cost). Pure trust signal, no CTA of its own.
 *
 * IMPORTANT: every claim here must be backed by a real policy. Update the
 * numbers/terms below to match the actual license terms before launch.
 */

const GUARANTEES = [
  {
    title: '30-day refund',
    desc: 'No questions, no friction. If it doesn\'t spark, write in and we\'ll refund you.',
    accent: 'cyan' as const,
    icon: (
      <path d="M3 12a9 9 0 1 0 3-6.7M3 4v4h4" />
    ),
  },
  {
    title: 'Lifetime updates',
    desc: 'Buy once. Every future improvement lands in your dashboard at no extra cost.',
    accent: 'gold' as const,
    icon: (
      <>
        <path d="M21 12a9 9 0 1 1-3-6.7" />
        <path d="M21 4v4h-4" />
      </>
    ),
  },
  {
    title: 'No subscription, ever',
    desc: 'One price, yours forever. We will never move this to a recurring model.',
    accent: 'gold' as const,
    icon: (
      <>
        <rect x="3" y="5" width="18" height="14" rx="2" />
        <path d="M3 10h18" />
      </>
    ),
  },
  {
    title: 'Runs fully offline',
    desc: 'Morphing, physics, and breeding work with zero internet. No always-on DRM.',
    accent: 'cyan' as const,
    icon: (
      <>
        <path d="M12 3a9 9 0 1 0 9 9" />
        <path d="m9 12 2 2 4-4" />
      </>
    ),
  },
]

const ACCENT_CLASSES: Record<'cyan' | 'gold', { badge: string; halo: string }> = {
  cyan: {
    badge: 'bg-cyan/12 text-cyan',
    halo: 'hover:halo-cyan',
  },
  gold: {
    badge: 'bg-gold/12 text-gold',
    halo: 'hover:halo-gold',
  },
}

export function Guarantee() {
  return (
    <section
      id="guarantee"
      aria-label="Purchase guarantee"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 pb-4"
    >
      <div className="gold-leaf glass-strong grid gap-px overflow-hidden rounded-3xl border border-slate-line sm:grid-cols-2 lg:grid-cols-4">
        {GUARANTEES.map((g) => {
          const accent = ACCENT_CLASSES[g.accent]
          return (
            <div
              key={g.title}
              className={`group flex items-start gap-3 p-5 transition-all duration-300 hover:-translate-y-0.5 sm:p-6 ${accent.halo}`}
              data-testid={`guarantee-${g.title.toLowerCase().replace(/[^a-z]+/g, '-')}`}
            >
              <span
                className={`mt-0.5 flex size-9 shrink-0 items-center justify-center rounded-full transition-transform duration-300 group-hover:scale-110 ${accent.badge}`}
              >
                <svg
                  width="18"
                  height="18"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  strokeWidth="2"
                  strokeLinecap="round"
                  strokeLinejoin="round"
                  aria-hidden="true"
                >
                  {g.icon}
                </svg>
              </span>
              <div>
                <h3 className="font-heading text-sm font-bold tracking-wide text-foreground">
                  {g.title}
                </h3>
                <p className="mt-1 text-xs leading-relaxed text-muted-foreground">
                  {g.desc}
                </p>
              </div>
            </div>
          )
        })}
      </div>
    </section>
  )
}
