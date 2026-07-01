'use client'

import { useEffect, useRef, useState } from 'react'
import { Reveal } from './reveal'

export function FeaturesGrid() {
  return (
    <section
      id="features"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 py-24"
    >
      <div className="mx-auto mb-14 max-w-2xl text-center">
        <span className="text-xs font-medium tracking-[0.25em] text-primary uppercase">
          What you can do
        </span>
        <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance sm:text-4xl">
          Four ways to find{' '}
          <span className="text-gradient-aurora animate-gradient-pan">sounds</span>{' '}
          no preset gives you.
        </h2>
        <p className="mt-4 text-pretty text-muted-foreground">
          Every sound you make flows through a physics core, a genetic breeder,
          an AI co-pilot, and a 12-slot memory bank. Less dialing, more
          discovering.
        </p>
      </div>

      <div className="grid gap-5 md:grid-cols-2">
        <Reveal delay={0}>
          <PhysicsCard />
        </Reveal>
        <Reveal delay={90}>
          <BreedingCard />
        </Reveal>
        <Reveal delay={0}>
          <McpCard />
        </Reveal>
        <Reveal delay={90}>
          <SnapshotBankCard />
        </Reveal>
      </div>
    </section>
  )
}

function CardShell({
  id,
  index,
  title,
  desc,
  children,
}: {
  id: string
  index: string
  title: string
  desc: string
  children: React.ReactNode
}) {
  return (
    <article
      id={id}
      className="gold-leaf glass group relative h-full overflow-hidden rounded-3xl p-6 transition-all duration-500 hover:halo-cyan hover:-translate-y-1 sm:p-8"
    >
      <div className="flex items-start justify-between gap-4">
        <div>
          <span className="font-mono text-xs text-cyan">{index}</span>
          <h3 className="mt-1 font-heading text-lg font-bold tracking-tight">
            {title}
          </h3>
        </div>
      </div>
      <p className="mt-3 max-w-sm text-pretty text-sm leading-relaxed text-muted-foreground">
        {desc}
      </p>
      <div className="mt-6">{children}</div>
    </article>
  )
}

/* 1. Physics engine — a dot in Direct / Elastic / Drift modes */
function PhysicsCard() {
  const [mode, setMode] = useState<'direct' | 'elastic' | 'drift'>('elastic')
  const dotRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const dot = dotRef.current
    if (!dot) return
    let raf = 0
    let t = 0
    // state
    let x = 50
    let v = 0
    let target = 80
    let lastFlip = 0

    const step = () => {
      t += 0.016
      if (mode === 'direct') {
        // snaps quickly toward target, switching ends
        if (t - lastFlip > 1.1) {
          target = target > 50 ? 20 : 80
          lastFlip = t
        }
        x += (target - x) * 0.25
      } else if (mode === 'elastic') {
        if (t - lastFlip > 1.6) {
          target = target > 50 ? 18 : 82
          lastFlip = t
        }
        const k = 0.02
        const damp = 0.86
        v += (target - x) * k
        v *= damp
        x += v
      } else {
        // drift — smooth pseudo-perlin wander
        x =
          50 +
          Math.sin(t * 0.8) * 22 +
          Math.sin(t * 1.93 + 1.3) * 10 +
          Math.sin(t * 0.37) * 6
      }
      dot.style.left = `${Math.max(6, Math.min(94, x))}%`
      raf = requestAnimationFrame(step)
    }
    raf = requestAnimationFrame(step)
    return () => cancelAnimationFrame(raf)
  }, [mode])

  return (
    <CardShell
      id="feature-physics"
      index="01"
      title="Physics Engine"
      desc="Make your sound breathe. Set the node in motion and let it drift, bounce, or settle toward a target — every parameter moves with real momentum. Choose Direct for instant snaps, Elastic for spring-damper overshoot, or Drift for organic, hands-off wander."
    >
      <div className="relative mb-4 h-16 overflow-hidden rounded-xl border border-slate-line bg-background/40">
        <div className="absolute top-1/2 right-4 left-4 h-px -translate-y-1/2 bg-slate-line" />
        <div
          ref={dotRef}
          className="absolute top-1/2 size-4 -translate-x-1/2 -translate-y-1/2 rounded-full bg-gradient-to-tr from-cyan to-magenta shadow-[0_0_18px_-2px_var(--color-cyan)]"
          style={{ left: '50%' }}
        />
      </div>
      <div className="flex gap-2">
        {(['direct', 'elastic', 'drift'] as const).map((m) => (
          <button
            key={m}
            id={`physics-mode-${m}`}
            type="button"
            onClick={() => setMode(m)}
            className={`rounded-full px-3 py-1.5 text-xs font-medium capitalize transition-all ${
              mode === m
                ? 'bg-cyan/15 text-cyan'
                : 'glass text-muted-foreground hover:text-foreground'
            }`}
          >
            {m}
          </button>
        ))}
      </div>
    </CardShell>
  )
}

/* 2. Genetic breeding engine — DNA strands + mutate */
function BreedingCard() {
  const [gen, setGen] = useState(1)
  const [flash, setFlash] = useState(false)

  const mutate = () => {
    setFlash(true)
    setGen((g) => g + 1)
    setTimeout(() => setFlash(false), 700)
  }

  const dna = Array.from({ length: 10 })

  return (
    <CardShell
      id="feature-breeding"
      index="02"
      title="Genetic Breeding Engine"
      desc="Cross-breed two sounds and discover a third you'd never dial in by hand. Treat snapshots as parents, splice their parameter genomes, and let mutation surface the happy accidents for you."
    >
      <div
        className={`relative mb-4 flex h-16 items-center justify-center gap-1.5 overflow-hidden rounded-xl border border-slate-line bg-background/40 transition-all ${
          flash ? 'halo-gold' : ''
        }`}
      >
        {dna.map((_, i) => {
          const phase = (i / dna.length) * Math.PI * 2
          const h = 6 + Math.abs(Math.sin(phase + gen)) * 22
          return (
            <span key={i} className="flex flex-col items-center justify-center gap-0.5">
              <span
                className="w-1 rounded-full bg-cyan transition-all duration-500"
                style={{ height: h, opacity: flash ? 1 : 0.7 }}
              />
              <span
                className="w-1 rounded-full bg-magenta transition-all duration-500"
                style={{ height: 28 - h, opacity: flash ? 1 : 0.7 }}
              />
            </span>
          )
        })}
      </div>
      <div className="flex items-center gap-3">
        <button
          id="breeding-mutate-btn"
          type="button"
          onClick={mutate}
          className="glass halo-gold rounded-full px-4 py-1.5 text-xs font-semibold text-primary transition-transform hover:scale-105"
        >
          Mutate &amp; Breed
        </button>
        <span className="font-mono text-xs text-muted-foreground">
          Generation {gen}
        </span>
      </div>
    </CardShell>
  )
}

/* 3. MCP AI assistant — mock chat */
function McpCard() {
  const [stage, setStage] = useState(0)
  // stage 0: idle, 1: user asked, 2: ai thinking, 3: ai responded + dials move

  const run = () => {
    setStage(1)
    setTimeout(() => setStage(2), 500)
    setTimeout(() => setStage(3), 1500)
  }

  return (
    <CardShell
      id="feature-mcp"
      index="03"
      title="AI Co-Pilot"
      desc="Describe the sound in plain words — “make this brighter,” “less mud in the low mids” — and the built-in assistant moves the right knobs for you. No menu diving, no manual parameter hunting."
    >
      <div className="mb-4 space-y-2 rounded-xl border border-slate-line bg-background/40 p-3">
        <div className="flex justify-end">
          <span className="max-w-[80%] rounded-2xl rounded-br-sm bg-cyan/15 px-3 py-1.5 text-xs text-foreground">
            This pad sounds muddy in the low mids.
          </span>
        </div>
        {stage >= 2 && (
          <div className="flex justify-start">
            <span className="max-w-[85%] rounded-2xl rounded-bl-sm bg-muted px-3 py-1.5 text-xs text-muted-foreground">
              {stage === 2 ? (
                <span className="flex gap-1">
                  <Dot /> <Dot delay={150} /> <Dot delay={300} />
                </span>
              ) : (
                <>
                  Cutting <span className="text-primary">300&ndash;500 Hz</span>{' '}
                  by 4 dB and easing resonance. Applied.
                </>
              )}
            </span>
          </div>
        )}
      </div>
      <div className="mb-4 flex gap-4">
        {['Low Mid', 'Resonance', 'Cutoff'].map((l, i) => (
          <div key={l} className="flex flex-col items-center gap-1">
            <div className="relative size-8">
              <span
                className="absolute top-1/2 left-1/2 h-2.5 w-0.5 origin-bottom rounded-full bg-primary transition-transform duration-700"
                style={{
                  transformOrigin: '50% 100%',
                  transform: `translate(-50%,-100%) rotate(${
                    stage >= 3 ? -60 + i * 50 : 30
                  }deg)`,
                }}
              />
              <span className="absolute inset-0 rounded-full border border-slate-line" />
            </div>
            <span className="text-[9px] text-muted-foreground">{l}</span>
          </div>
        ))}
      </div>
      <button
        id="mcp-ask-btn"
        type="button"
        onClick={run}
        className="glass rounded-full px-4 py-1.5 text-xs font-semibold text-cyan transition-transform hover:scale-105 hover:halo-cyan"
      >
        Ask the assistant
      </button>
    </CardShell>
  )
}

function Dot({ delay = 0 }: { delay?: number }) {
  return (
    <span
      className="inline-block size-1.5 animate-bounce rounded-full bg-muted-foreground"
      style={{ animationDelay: `${delay}ms` }}
    />
  )
}

/* 4. 12-slot snapshot bank — clock layout */
function SnapshotBankCard() {
  const [active, setActive] = useState(2)

  useEffect(() => {
    const t = setInterval(() => setActive((a) => (a + 1) % 12), 1400)
    return () => clearInterval(t)
  }, [])

  return (
    <CardShell
      id="feature-snapshot-bank"
      index="04"
      title="12-Slot Snapshot Bank"
      desc="Twelve full patch states, one gesture away. Each slot holds a complete snapshot — morph freely between any of them, live, right in the mix."
    >
      <div className="relative mx-auto h-40 w-40">
        <div className="absolute inset-4 rounded-full border border-slate-line" />
        {Array.from({ length: 12 }).map((_, i) => {
          const angle = (i / 12) * Math.PI * 2 - Math.PI / 2
          const r = 64
          const x = Math.round((80 + Math.cos(angle) * r) * 100) / 100
          const y = Math.round((80 + Math.sin(angle) * r) * 100) / 100
          const isActive = i === active
          return (
            <span
              key={i}
              className="absolute flex size-7 -translate-x-1/2 -translate-y-1/2 items-center justify-center rounded-full border text-[10px] font-medium transition-all duration-500"
              style={{
                left: x,
                top: y,
                borderColor: isActive
                  ? 'var(--color-gold)'
                  : 'var(--slate-line)',
                color: isActive ? 'var(--color-gold)' : 'var(--muted-foreground)',
                background: isActive
                  ? 'color-mix(in oklch, var(--color-gold) 15%, transparent)'
                  : 'transparent',
                boxShadow: isActive
                  ? '0 0 20px -2px var(--color-gold)'
                  : 'none',
              }}
            >
              {i + 1}
            </span>
          )
        })}
        <span className="absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 font-heading text-xs font-bold tracking-widest text-muted-foreground">
          BANK
        </span>
      </div>
    </CardShell>
  )
}
