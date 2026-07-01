'use client'

import { Reveal } from './reveal'

/**
 * Luxury prestige strip — sits directly under the hero.
 *
 * NOTE: uses generic discipline/category labels (not fabricated artist names,
 * brand logos, or quotes), so it carries no false-endorsement / FTC risk.
 * Replace the CRAFTS array with whatever real framing you want; keep it to
 * categories rather than implied endorsements.
 */

const CRAFTS = [
  'Sound Design',
  'Film & TV Scoring',
  'Electronic Production',
  'Mixing & Mastering',
  'Live Performance',
  'Ambient & Experimental',
]

export function TrustBar() {
  return (
    <section
      id="trust-bar"
      aria-label="Built for serious studios"
      className="relative mx-auto w-full max-w-7xl px-5 pt-6"
    >
      <Reveal className="glass gold-leaf flex flex-col items-center gap-4 rounded-2xl px-6 py-5 sm:flex-row sm:justify-between sm:gap-6">
        <div className="flex items-center gap-3">
          <span className="size-1.5 rounded-full bg-gold shadow-[0_0_12px_0_var(--color-gold)]" />
          <span className="font-heading text-xs font-medium tracking-[0.25em] text-primary uppercase">
            Crafted for serious studios
          </span>
        </div>

        <ul className="flex flex-wrap items-center justify-center gap-x-5 gap-y-2">
          {CRAFTS.map((c) => (
            <li
              key={c}
              className="flex items-center gap-1.5 text-xs font-medium tracking-wide text-muted-foreground"
            >
              <span className="size-1 rounded-full bg-gradient-to-tr from-cyan to-magenta opacity-80" />
              {c}
            </li>
          ))}
        </ul>
      </Reveal>
    </section>
  )
}
