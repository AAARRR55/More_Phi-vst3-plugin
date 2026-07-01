'use client'

import { useState } from 'react'

const SPECS = [
  {
    label: 'macOS',
    value: 'Intel & Apple Silicon (M1 / M2 / M3). AU & VST3. macOS 11+.',
  },
  { label: 'Windows', value: 'Windows 10 / 11, 64-bit. VST3.' },
  {
    label: 'DAW Support',
    value: 'Ableton Live, Logic Pro, Reaper, Pro Tools, FL Studio, Cubase, Bitwig.',
  },
  { label: 'Engine', value: 'JUCE 8 · 64-bit · up to 192 kHz · PDC-aware.' },
]

const FAQ = [
  {
    q: 'How does parameter morphing actually work?',
    a: 'More-Phi snapshots the full parameter state of any plugin chain it hosts. As you move the morph node, it interpolates every parameter simultaneously using inverse distance weighting, then applies your chosen physics mode — direct, elastic, or drift — to shape how each value travels.',
  },
  {
    q: 'Is the AI assistant required, or can I work fully offline?',
    a: 'Completely optional. The MCP assistant is an enhancement. The core morphing, physics, breeding, and snapshot systems run entirely offline with zero latency.',
  },
  {
    q: 'Will it work with my hardware synths?',
    a: 'Yes. More-Phi can transmit interpolated values as MIDI CC and NRPN, so any parameter your hardware exposes can be morphed in real time alongside your software instruments.',
  },
  {
    q: 'What does the license cover?',
    a: 'A single purchase covers all your personal machines and includes lifetime updates. No subscription, no annual fees, no online activation after the initial install.',
  },
]

export function SpecsFaq() {
  const [open, setOpen] = useState<number | null>(0)

  return (
    <section
      id="specs"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 py-24"
    >
      <div className="grid gap-12 lg:grid-cols-[0.9fr_1.1fr]">
        {/* Specs */}
        <div>
          <span className="text-xs font-medium tracking-[0.25em] text-cyan uppercase">
            Technical Specifications
          </span>
          <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance">
            Built for every studio.
          </h2>
          <dl className="mt-8 space-y-px overflow-hidden rounded-2xl border border-slate-line">
            {SPECS.map((s) => (
              <div
                key={s.label}
                className="gold-leaf glass flex flex-col gap-1 p-5 transition-all duration-300 hover:bg-foreground/[0.02] sm:flex-row sm:gap-6"
              >
                <dt className="w-32 shrink-0 font-heading text-sm font-bold tracking-wide text-primary">
                  {s.label}
                </dt>
                <dd className="text-sm text-muted-foreground">{s.value}</dd>
              </div>
            ))}
          </dl>
        </div>

        {/* FAQ */}
        <div>
          <span className="text-xs font-medium tracking-[0.25em] text-primary uppercase">
            Questions
          </span>
          <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance">
            Everything you need to know.
          </h2>
          <div className="mt-8 space-y-3">
            {FAQ.map((item, i) => {
              const isOpen = open === i
              return (
                <div
                  key={item.q}
                  className="glass overflow-hidden rounded-2xl transition-all"
                >
                  <button
                    id={`faq-toggle-${i}`}
                    type="button"
                    aria-expanded={isOpen}
                    onClick={() => setOpen(isOpen ? null : i)}
                    className="flex w-full items-center justify-between gap-4 p-5 text-left"
                  >
                    <span className="text-sm font-medium text-foreground">
                      {item.q}
                    </span>
                    <span
                      className={`flex size-6 shrink-0 items-center justify-center rounded-full border border-slate-line transition-transform duration-300 ${
                        isOpen ? 'rotate-45 text-gold' : 'text-muted-foreground'
                      }`}
                    >
                      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" aria-hidden="true">
                        <path d="M12 5v14M5 12h14" />
                      </svg>
                    </span>
                  </button>
                  <div
                    className="grid transition-all duration-300 ease-out"
                    style={{
                      gridTemplateRows: isOpen ? '1fr' : '0fr',
                      opacity: isOpen ? 1 : 0,
                    }}
                  >
                    <div className="overflow-hidden">
                      <p className="px-5 pb-5 text-sm leading-relaxed text-muted-foreground">
                        {item.a}
                      </p>
                    </div>
                  </div>
                </div>
              )
            })}
          </div>
        </div>
      </div>
    </section>
  )
}

export function SiteFooter() {
  return (
    <footer
      id="site-footer"
      className="relative mx-auto w-full max-w-7xl px-5 pt-12 pb-16"
    >
      <div className="glass-strong overflow-hidden rounded-3xl px-6 py-12 text-center sm:py-16">
        <h2 className="font-heading text-2xl font-bold tracking-tight text-balance sm:text-3xl">
          The physics of synthesis,{' '}
          <span className="text-gradient-morph">in your hands.</span>
        </h2>
        <a
          id="footer-cta-acquire"
          href="#checkout"
          className="shimmer-sweep group relative mx-auto mt-7 inline-block overflow-hidden rounded-full p-px"
        >
          <span className="aurora-border-fill absolute inset-0 rounded-full opacity-80 transition-opacity group-hover:opacity-100" />
          <span className="glass-strong relative flex items-center gap-2 rounded-full px-8 py-3.5 text-sm font-semibold">
            Get More&#8211;Phi &mdash;{' '}
            <span className="text-primary">$79</span>
          </span>
        </a>
      </div>

      <div className="mt-8 flex flex-col items-center justify-between gap-4 text-xs text-muted-foreground sm:flex-row">
        <div className="flex items-center gap-2">
          <span className="size-2.5 rounded-full bg-gradient-to-tr from-cyan to-magenta" />
          <span className="font-heading tracking-[0.2em]">MORE&#8211;PHI</span>
        </div>
        <p>&copy; {new Date().getFullYear()} More-Phi Audio. All rights reserved.</p>
      </div>
    </footer>
  )
}
