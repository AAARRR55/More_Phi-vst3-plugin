'use client'

import Image from 'next/image'
import { useEffect, useRef } from 'react'
import { FluidBackground } from './fluid-background'

export function Hero() {
  const mockupRef = useRef<HTMLDivElement>(null)

  // Subtle parallax tilt on the floating mockup following the cursor.
  useEffect(() => {
    const el = mockupRef.current
    if (!el) return
    let raf = 0
    const onMove = (e: MouseEvent) => {
      cancelAnimationFrame(raf)
      raf = requestAnimationFrame(() => {
        const rx = (e.clientY / window.innerHeight - 0.5) * -8
        const ry = (e.clientX / window.innerWidth - 0.5) * 10
        el.style.transform = `perspective(1200px) rotateX(${rx}deg) rotateY(${ry}deg)`
      })
    }
    window.addEventListener('mousemove', onMove)
    return () => {
      window.removeEventListener('mousemove', onMove)
      cancelAnimationFrame(raf)
    }
  }, [])

  return (
    <section
      id="hero"
      className="relative isolate flex min-h-screen flex-col items-center justify-center overflow-hidden px-5 pt-28 pb-16"
    >
      <FluidBackground />
      <div className="grid-noise pointer-events-none absolute inset-0 opacity-40" />

      <div className="relative z-10 mx-auto grid w-full max-w-7xl items-center gap-12 lg:grid-cols-[1.05fr_0.95fr]">
        {/* Copy */}
        <div className="text-center lg:text-left">
          <span className="glass mx-auto mb-6 inline-flex items-center gap-2 rounded-full px-3.5 py-1.5 text-xs tracking-wide text-muted-foreground lg:mx-0">
            <span className="size-1.5 animate-pulse rounded-full bg-cyan" />
            JUCE 8 Engine &middot; VST3 &middot; AU
          </span>

          <h1 className="font-heading text-4xl leading-[1.05] font-bold tracking-tight text-balance sm:text-5xl lg:text-6xl">
            Every sound{' '}
            <span className="text-gradient-aurora animate-gradient-pan">between</span>{' '}
            your favorite presets.
          </h1>

          <p className="mx-auto mt-6 max-w-xl text-pretty font-display text-base leading-relaxed text-muted-foreground lg:mx-0 lg:text-lg">
            Drag one node across a 2D pad and hear your plugins transform in
            real time. Spring physics, genetic breeding, and an on-board AI
            co-pilot do the tedious work &mdash; you just chase the sounds you
            love.
          </p>

          <div className="mt-9 flex flex-col items-center gap-3 sm:flex-row lg:justify-start">
            <a
              id="hero-cta-experience"
              href="#morph-demo"
              className="glass flex w-full items-center justify-center gap-2 rounded-full px-7 py-3.5 text-sm font-medium text-foreground transition-all hover:halo-cyan sm:w-auto"
            >
              Hear it live
              <svg
                width="16"
                height="16"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
                aria-hidden="true"
              >
                <path d="M12 5v14M5 12l7 7 7-7" />
              </svg>
            </a>
          </div>

          <dl className="mt-12 grid max-w-md grid-cols-3 gap-4 text-center lg:mx-0 lg:text-left">
            {[
              ['12', 'Snapshots, infinite blends'],
              ['<2%', 'CPU at 96 kHz'],
              ['40+', 'Plugins you already own'],
            ].map(([n, l]) => (
              <div key={l}>
                <dt className="font-heading text-2xl font-bold text-foreground">
                  {n}
                </dt>
                <dd className="mt-1 text-xs text-muted-foreground">{l}</dd>
              </div>
            ))}
          </dl>

          <p className="mt-6 flex flex-wrap items-center justify-center gap-x-2 gap-y-1 text-xs text-muted-foreground lg:justify-start">
            <span className="inline-flex items-center gap-1.5">
              <span className="size-1 rounded-full bg-gold shadow-[0_0_8px_0_var(--color-gold)]" />
              Trusted by producers in 40+ studios
            </span>
            <span className="text-slate-line">&middot;</span>
            <span className="text-primary/80">Launch pricing &mdash; limited</span>
          </p>
        </div>

        {/* Floating mockup */}
        <div className="relative mx-auto w-full max-w-xl">
          <div className="animate-float-slow">
            <div
              ref={mockupRef}
              className="gold-leaf halo-cyan glass-strong shimmer-sweep relative overflow-hidden rounded-3xl p-2 transition-transform duration-300 ease-out will-change-transform"
            >
              <Image
                src="/morphi-plugin.png"
                alt="More-Phi plugin interface showing the 2D morph pad surrounded by rotary dials and a 12-slot snapshot ring"
                width={900}
                height={720}
                priority
                className="rounded-2xl"
              />
            </div>
          </div>
          <div className="pointer-events-none absolute -inset-10 -z-20 rounded-full bg-magenta/10 blur-3xl" />
        </div>
      </div>
    </section>
  )
}
