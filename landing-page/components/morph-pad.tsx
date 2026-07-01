'use client'

import { useCallback, useEffect, useRef, useState } from 'react'

/**
 * Live 2D morph pad. Four corner snapshots each drive a small synth voice.
 * The draggable node's position determines per-voice gain via Inverse Distance
 * Weighting (IDW). Surrounding dials reflect interpolated parameter values.
 */

type Corner = {
  id: string
  label: string
  sub: string
  pos: { x: number; y: number } // normalized 0..1
  type: OscillatorType
  base: number
  notes: number[]
  cutoff: number
  q: number
  detune: number
  hue: number
}

const CORNERS: Corner[] = [
  {
    id: 'corner-a',
    label: 'A',
    sub: 'Ambient Pad',
    pos: { x: 0, y: 0 },
    type: 'triangle',
    base: 110,
    notes: [110, 164.81, 220], // A2, E3, A3 (Warm Open Fifth)
    cutoff: 750,
    q: 1.0,
    detune: 8,
    hue: 200, // brand cyan
  },
  {
    id: 'corner-b',
    label: 'B',
    sub: 'Gritty Bass',
    pos: { x: 1, y: 0 },
    type: 'triangle', // Triangle for warm, saturated sub-bass growl
    base: 55,
    notes: [55, 110], // A1, A2 (Sub & Bass octaves)
    cutoff: 300,
    q: 4.0,
    detune: 12,
    hue: 330, // brand magenta
  },
  {
    id: 'corner-c',
    label: 'C',
    sub: 'Plucky Lead',
    pos: { x: 0, y: 1 },
    type: 'triangle',
    base: 220,
    notes: [440, 523.25, 659.25], // A4, C5, E5 (High minor triad)
    cutoff: 1000,
    q: 2.5,
    detune: 6,
    hue: 45, // brand gold
  },
  {
    id: 'corner-d',
    label: 'D',
    sub: 'Glitch Arp',
    pos: { x: 1, y: 1 },
    type: 'sawtooth', // Sawtooth for classic analog synth pluck
    base: 220,
    notes: [220, 329.63, 440], // A3, E4, A4 (Suspended arp)
    cutoff: 1400,
    q: 6.0,
    detune: 18,
    hue: 165, // teal — bridges cyan↔gold harmonically
  },
]

const DIALS = ['Cutoff', 'Resonance', 'Detune', 'Drive', 'Pitch', 'Air'] as const

function makeDistortionCurve(amount = 20) {
  const n_samples = 44100
  const curve = new Float32Array(n_samples)
  for (let i = 0; i < n_samples; ++i) {
    const x = (i * 2) / n_samples - 1
    curve[i] = Math.tanh(x * amount)
  }
  return curve
}

type Engine = {
  ctx: AudioContext
  master: GainNode
  voices: {
    oscs: { osc: OscillatorNode; osc2: OscillatorNode }[]
    filter: BiquadFilterNode
    gain: GainNode
    shaper?: WaveShaperNode
    gateGain?: GainNode
  }[]
  delayL: DelayNode
  delayR: DelayNode
  shaper: WaveShaperNode
  masterFilter: BiquadFilterNode
}

function idwWeights(x: number, y: number) {
  const p = 2.4
  const raw = CORNERS.map((c) => {
    const d = Math.hypot(x - c.pos.x, y - c.pos.y)
    if (d < 0.0001) return 1e6
    return 1 / Math.pow(d, p)
  })
  const sum = raw.reduce((a, b) => a + b, 0)
  return raw.map((r) => r / sum)
}

export function MorphPad() {
  const padRef = useRef<HTMLDivElement>(null)
  const engineRef = useRef<Engine | null>(null)
  const [pos, setPos] = useState({ x: 0.5, y: 0.5 })
  const [playing, setPlaying] = useState(false)
  const [dragging, setDragging] = useState(false)
  const posRef = useRef(pos)
  posRef.current = pos

  const weights = idwWeights(pos.x, pos.y)

  // Interpolated parameter readouts for the dials (0..1).
  const interp = useCallback(
    (key: 'cutoff' | 'q' | 'detune' | 'base') => {
      let v = 0
      CORNERS.forEach((c, i) => {
        v += c[key] * weights[i]
      })
      return v
    },
    [weights],
  )

  const dialValues = {
    Cutoff: Math.min(1, interp('cutoff') / 3000),
    Resonance: Math.min(1, interp('q') / 14),
    Detune: Math.min(1, interp('detune') / 24),
    Drive: Math.min(1, (interp('q') / 14) * 0.8 + 0.1),
    Pitch: Math.min(1, interp('base') / 360),
    Air: Math.min(1, interp('cutoff') / 3000) * 0.7 + 0.15,
  } as Record<(typeof DIALS)[number], number>

  const buildEngine = useCallback(() => {
    const Ctx =
      window.AudioContext ||
      (window as unknown as { webkitAudioContext: typeof AudioContext })
        .webkitAudioContext
    const ctx = new Ctx()
    
    // Master gain
    const master = ctx.createGain()
    master.gain.value = 0.0001
    
    // Wave shaper for drive (saturation)
    const shaper = ctx.createWaveShaper()
    shaper.curve = makeDistortionCurve(10)
    shaper.oversample = '4x'

    // Master lowpass filter for analog warmth (rolls off harsh high frequencies)
    const masterFilter = ctx.createBiquadFilter()
    masterFilter.type = 'lowpass'
    masterFilter.frequency.value = 7500
    
    // Stereo Delay Nodes with Damping Filters
    const delayL = ctx.createDelay(1.0)
    const delayR = ctx.createDelay(1.0)
    const delayFeedbackL = ctx.createGain()
    const delayFeedbackR = ctx.createGain()
    const delayFilterL = ctx.createBiquadFilter()
    const delayFilterR = ctx.createBiquadFilter()
    const delayGainL = ctx.createGain()
    const delayGainR = ctx.createGain()

    delayL.delayTime.value = 0.375 // dotted eighth
    delayR.delayTime.value = 0.500 // quarter
    delayFeedbackL.gain.value = 0.38
    delayFeedbackR.gain.value = 0.38
    
    // Warm lowpass filter for delay lines
    delayFilterL.type = 'lowpass'
    delayFilterL.frequency.value = 1000
    delayFilterR.type = 'lowpass'
    delayFilterR.frequency.value = 1000

    delayGainL.gain.value = 0.12 // lower wet mix
    delayGainR.gain.value = 0.12

    // Feedback loops with damping and ping-pong cross-coupling
    delayL.connect(delayFeedbackL)
    delayFeedbackL.connect(delayFilterL)
    delayFilterL.connect(delayL)
    delayFilterL.connect(delayR)

    delayR.connect(delayFeedbackR)
    delayFeedbackR.connect(delayFilterR)
    delayFilterR.connect(delayR)
    delayFilterR.connect(delayL)

    // Routing: master -> shaper -> masterFilter -> destination
    master.connect(shaper)
    shaper.connect(masterFilter)
    masterFilter.connect(ctx.destination)

    // Delay sends from master
    master.connect(delayL)
    master.connect(delayR)
    
    delayL.connect(delayGainL)
    delayR.connect(delayGainR)
    delayGainL.connect(ctx.destination)
    delayGainR.connect(ctx.destination)

    const voices = CORNERS.map((c) => {
      const filter = ctx.createBiquadFilter()
      filter.type = 'lowpass'
      filter.frequency.value = c.cutoff
      filter.Q.value = c.q

      const gain = ctx.createGain()
      gain.gain.value = 0

      // Create oscillators for each note in the chord
      const oscs = c.notes.map((freq) => {
        const osc = ctx.createOscillator()
        const osc2 = ctx.createOscillator()
        osc.type = c.type
        osc2.type = c.type
        osc.frequency.value = freq
        osc2.frequency.value = freq
        osc2.detune.value = c.detune

        osc.connect(filter)
        osc2.connect(filter)
        
        osc.start()
        osc2.start()
        
        return { osc, osc2 }
      })

      // Voice specific enhancements
      let lastNode: AudioNode = filter

      // 1. Slow LFO for Corner A (Ambient Pad) filter cutoff modulation
      if (c.id === 'corner-a') {
        const lfo = ctx.createOscillator()
        lfo.frequency.value = 0.15 // 0.15 Hz
        const lfoGain = ctx.createGain()
        lfoGain.gain.value = 350 // +/- 350 Hz sweep
        lfo.connect(lfoGain)
        lfoGain.connect(filter.frequency)
        lfo.start()
      }

      // 2. Waveshaper for Corner B (Gritty Bass) to add warmth/growl
      let voiceShaper: WaveShaperNode | undefined
      if (c.id === 'corner-b') {
        voiceShaper = ctx.createWaveShaper()
        voiceShaper.curve = makeDistortionCurve(25) // rich saturation
        voiceShaper.oversample = '4x'
        lastNode.connect(voiceShaper)
        lastNode = voiceShaper
      }

      // 3. Gate LFO (pulsing tremolo) for Corner C (Plucky Lead)
      let gateGain: GainNode | undefined
      if (c.id === 'corner-c') {
        gateGain = ctx.createGain()
        gateGain.gain.value = 0.5
        
        const gateLfo = ctx.createOscillator()
        gateLfo.type = 'sine' // smooth sine tremolo
        gateLfo.frequency.value = 3.0 // 3 Hz pulsing lead
        
        const gateLfoGain = ctx.createGain()
        gateLfoGain.gain.value = 0.45 // modulate volume smoothly
        
        gateLfo.connect(gateLfoGain)
        gateLfoGain.connect(gateGain.gain)
        gateLfo.start()

        lastNode.connect(gateGain)
        lastNode = gateGain
      }

      // 4. Glitch Modulation for Corner D (Glitch Arp)
      if (c.id === 'corner-d') {
        // Fast filter sweep chopper
        const filterLfo = ctx.createOscillator()
        filterLfo.type = 'triangle'
        filterLfo.frequency.value = 6.0 // 6 Hz pulsing filter
        const filterLfoGain = ctx.createGain()
        filterLfoGain.gain.value = 500 // +/- 500 Hz sweep
        filterLfo.connect(filterLfoGain)
        filterLfoGain.connect(filter.frequency)
        filterLfo.start()

        // Subtle vibrato (pitch sweep) to keep it in tune but alive
        const vibratoLfo = ctx.createOscillator()
        vibratoLfo.type = 'sine'
        vibratoLfo.frequency.value = 5.0 // 5 Hz vibrato
        const vibratoLfoGain = ctx.createGain()
        vibratoLfoGain.gain.value = 10 // 10 cents vibrato
        vibratoLfo.connect(vibratoLfoGain)
        
        oscs.forEach((o) => {
          vibratoLfoGain.connect(o.osc.detune)
          vibratoLfoGain.connect(o.osc2.detune)
        })
        vibratoLfo.start()
      }

      // Connect the final node of the voice processing chain to the gain node
      lastNode.connect(gain)
      gain.connect(master)

      return { oscs, filter, gain, shaper: voiceShaper, gateGain }
    })

    engineRef.current = { ctx, master, voices, delayL, delayR, shaper, masterFilter }
  }, [])

  const applyWeights = useCallback((x: number, y: number) => {
    const eng = engineRef.current
    if (!eng) return
    const w = idwWeights(x, y)
    const t = eng.ctx.currentTime
    
    // 1. Set voice gains
    eng.voices.forEach((v, i) => {
      v.gain.gain.setTargetAtTime(w[i] * 0.07, t, 0.05)
    })

    // 2. Interpolate parameters for global dial modulation!
    let interpCutoff = 0
    let interpQ = 0
    let interpDetune = 0
    let interpBase = 0
    
    CORNERS.forEach((c, i) => {
      interpCutoff += c.cutoff * w[i]
      interpQ += c.q * w[i]
      interpDetune += c.detune * w[i]
      interpBase += c.base * w[i]
    })

    const dialCutoffVal = Math.min(1, interpCutoff / 3000)
    const dialResonanceVal = Math.min(1, interpQ / 14)
    const dialDetuneVal = Math.min(1, interpDetune / 24)

    // 3. Multipliers based on dial values (scales relative to base values)
    const cutoffMultiplier = 0.35 + dialCutoffVal * 2.65 // 0.35x to 3.0x
    const resonanceMultiplier = 0.5 + dialResonanceVal * 2.0 // 0.5x to 2.5x
    const detuneOffset = dialDetuneVal * 25 // 0 to 25 cents

    // 4. Apply modulated parameters to all active voices relative to their base values!
    eng.voices.forEach((v, idx) => {
      const baseCutoff = CORNERS[idx].cutoff
      const baseQ = CORNERS[idx].q
      const baseDetune = CORNERS[idx].detune

      const targetCutoff = Math.max(20, Math.min(20000, baseCutoff * cutoffMultiplier))
      v.filter.frequency.setTargetAtTime(targetCutoff, t, 0.1)
      v.filter.Q.setTargetAtTime(baseQ * resonanceMultiplier, t, 0.1)
      
      v.oscs.forEach((o) => {
        o.osc2.detune.setTargetAtTime(baseDetune + detuneOffset, t, 0.1)
      })
    })

    // 5. Apply Drive curve amount (waveshaper distortion amount)
    const driveAmount = (interpQ / 14) * 0.8 + 0.1
    eng.shaper.curve = makeDistortionCurve(5 + driveAmount * 25)

    // 6. Update delay times slightly based on Air/Pitch
    const airAmount = (interpCutoff / 3000) * 0.7 + 0.15
    eng.delayL.delayTime.setTargetAtTime(0.3 + airAmount * 0.15, t, 0.2)
    eng.delayR.delayTime.setTargetAtTime(0.4 + airAmount * 0.2, t, 0.2)
  }, [])

  const toggle = useCallback(async () => {
    if (!engineRef.current) buildEngine()
    const eng = engineRef.current
    if (!eng) return
    if (eng.ctx.state === 'suspended') await eng.ctx.resume()
    if (!playing) {
      eng.master.gain.setTargetAtTime(0.85, eng.ctx.currentTime, 0.1)
      applyWeights(posRef.current.x, posRef.current.y)
      setPlaying(true)
    } else {
      eng.master.gain.setTargetAtTime(0.0001, eng.ctx.currentTime, 0.1)
      setPlaying(false)
    }
  }, [playing, buildEngine, applyWeights])

  const updateFromEvent = useCallback(
    (clientX: number, clientY: number) => {
      const pad = padRef.current
      if (!pad) return
      const rect = pad.getBoundingClientRect()
      const x = Math.min(1, Math.max(0, (clientX - rect.left) / rect.width))
      const y = Math.min(1, Math.max(0, (clientY - rect.top) / rect.height))
      setPos({ x, y })
      applyWeights(x, y)
    },
    [applyWeights],
  )

  // Pointer drag handling on window so the node keeps tracking outside the pad.
  useEffect(() => {
    if (!dragging) return
    const move = (e: PointerEvent) => updateFromEvent(e.clientX, e.clientY)
    const up = () => setDragging(false)
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', up)
    return () => {
      window.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', up)
    }
  }, [dragging, updateFromEvent])

  useEffect(() => {
    return () => {
      engineRef.current?.ctx.close()
    }
  }, [])

  return (
    <section
      id="morph-demo"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 py-24"
    >
      <div className="mx-auto mb-12 max-w-2xl text-center">
        <span className="text-xs font-medium tracking-[0.25em] text-cyan uppercase">
          Interactive Experience
        </span>
        <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance sm:text-4xl">
          Drag the node. Hear the{' '}
          <span className="text-gradient-aurora animate-gradient-pan">morph.</span>
        </h2>
        <p className="mt-4 text-pretty text-muted-foreground">
          The real engine, running live in your browser. Each corner is a sound
          &mdash; drag the node and hear them blend into something new.
        </p>
      </div>

      <div className="gold-leaf glass-strong shimmer-sweep mx-auto grid max-w-5xl gap-8 rounded-3xl p-6 lg:grid-cols-[1fr_auto] lg:p-8">
        {/* Pad + dials */}
        <div className="flex flex-col items-center gap-6">
          <div className="flex w-full items-center justify-between">
            <button
              id="morph-play-toggle"
              type="button"
              onClick={toggle}
              className={`glass flex items-center gap-2 rounded-full px-4 py-2 text-sm font-medium transition-all ${
                playing ? 'halo-cyan text-cyan' : 'text-foreground'
              }`}
              aria-pressed={playing}
            >
              <span
                className={`flex size-5 items-center justify-center rounded-full ${
                  playing ? 'bg-cyan/20' : 'bg-muted'
                }`}
              >
                {playing ? (
                  <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
                    <rect x="6" y="5" width="4" height="14" rx="1" />
                    <rect x="14" y="5" width="4" height="14" rx="1" />
                  </svg>
                ) : (
                  <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
                    <path d="M8 5v14l11-7z" />
                  </svg>
                )}
              </span>
              {playing ? 'Stop Audio' : 'Start Audio'}
            </button>
            <span className="font-mono text-xs text-muted-foreground">
              X {pos.x.toFixed(2)} &middot; Y {(1 - pos.y).toFixed(2)}
            </span>
          </div>

          {/* The pad */}
          <div
            ref={padRef}
            id="morph-pad-surface"
            role="application"
            aria-label="2D morph pad. Drag the node to blend between four sound snapshots."
            onPointerDown={(e) => {
              e.preventDefault()
              if (!playing) void toggle()
              setDragging(true)
              updateFromEvent(e.clientX, e.clientY)
            }}
            className="gold-leaf grid-noise relative aspect-square w-full max-w-md cursor-crosshair touch-none overflow-hidden rounded-2xl border border-slate-line bg-[radial-gradient(circle_at_50%_50%,oklch(0.2_0.03_280),oklch(0.14_0.005_280))] shadow-[inset_0_0_60px_-20px_oklch(0.82_0.16_200/0.25)]"
          >
            {/* gradient field reacting to position */}
            <div
              className="pointer-events-none absolute inset-0 opacity-70 transition-[background] duration-75"
              style={{
                background: `radial-gradient(circle at ${pos.x * 100}% ${
                  pos.y * 100
                }%, oklch(0.7 0.2 ${
                  200 + (pos.x - pos.y) * 80
                } / 0.45), transparent 55%)`,
              }}
            />

            {/* corner nodes (visually inset from the edges; IDW still uses true corners) */}
            {CORNERS.map((c) => {
              const inset = 9 // percent inset from each edge
              const leftPct = inset + c.pos.x * (100 - inset * 2)
              const topPct = inset + c.pos.y * (100 - inset * 2)
              return (
              <div
                key={c.id}
                id={c.id}
                className="absolute flex -translate-x-1/2 -translate-y-1/2 flex-col items-center gap-1"
                style={{
                  left: `${leftPct}%`,
                  top: `${topPct}%`,
                }}
              >
                <span
                  className="flex size-8 items-center justify-center rounded-full border text-xs font-bold transition-transform"
                  style={{
                    borderColor: `hsl(${c.hue} 90% 60% / 0.6)`,
                    color: `hsl(${c.hue} 90% 70%)`,
                    background: `hsl(${c.hue} 90% 50% / 0.12)`,
                    transform: `scale(${0.85 + weights[CORNERS.indexOf(c)] * 0.9})`,
                  }}
                >
                  {c.label}
                </span>
                <span className="text-[10px] whitespace-nowrap text-muted-foreground">
                  {c.sub}
                </span>
              </div>
            )})}

            {/* draggable node */}
            <div
              className="pointer-events-none absolute size-7 -translate-x-1/2 -translate-y-1/2 rounded-full"
              style={{
                left: `${pos.x * 100}%`,
                top: `${pos.y * 100}%`,
              }}
            >
              <span className="absolute inset-0 animate-ping rounded-full bg-cyan/30" />
              <span className="animate-pulse-ring absolute -inset-2 rounded-full border border-cyan/40" />
              <span className="absolute inset-0 rounded-full border border-foreground/80 bg-gradient-to-tr from-cyan to-magenta shadow-[0_0_24px_-2px_var(--color-cyan)]" />
            </div>
          </div>

          {/* dials */}
          <div className="grid w-full max-w-md grid-cols-6 gap-2">
            {DIALS.map((d) => (
              <Dial key={d} label={d} value={dialValues[d]} />
            ))}
          </div>
        </div>

        {/* Snapshot legend */}
        <div className="flex min-w-[200px] flex-col gap-3">
          <h3 className="font-heading text-sm font-bold tracking-wide text-foreground">
            Active Blend
          </h3>
          {CORNERS.map((c, i) => (
            <div key={c.id} className="glass rounded-xl p-3">
              <div className="flex items-center justify-between">
                <span
                  className="flex items-center gap-2 text-sm font-medium"
                  style={{ color: `hsl(${c.hue} 90% 72%)` }}
                >
                  <span className="font-heading">{c.label}</span>
                  {c.sub}
                </span>
                <span className="font-mono text-xs text-muted-foreground">
                  {Math.round(weights[i] * 100)}%
                </span>
              </div>
              <div className="mt-2 h-1.5 overflow-hidden rounded-full bg-muted">
                <div
                  className="h-full rounded-full transition-[width] duration-75"
                  style={{
                    width: `${weights[i] * 100}%`,
                    background: `hsl(${c.hue} 90% 60%)`,
                  }}
                />
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  )
}

function Dial({ label, value }: { label: string; value: number }) {
  // Map 0..1 to -135..135 degrees.
  const angle = -135 + value * 270
  return (
    <div className="flex flex-col items-center gap-1.5">
      <div className="relative size-11">
        <svg viewBox="0 0 44 44" className="size-full -rotate-0" aria-hidden="true">
          <circle
            cx="22"
            cy="22"
            r="18"
            fill="none"
            stroke="var(--slate-line)"
            strokeWidth="2"
          />
          <circle
            cx="22"
            cy="22"
            r="18"
            fill="none"
            stroke="var(--color-cyan)"
            strokeWidth="2.5"
            strokeLinecap="round"
            strokeDasharray={`${value * 84} 200`}
            transform="rotate(135 22 22)"
            style={{ transition: 'stroke-dasharray 75ms linear' }}
          />
        </svg>
        <span
          className="absolute top-1/2 left-1/2 h-3.5 w-0.5 origin-bottom rounded-full bg-foreground"
          style={{
            transform: `translate(-50%, -100%) rotate(${angle}deg)`,
            transformOrigin: '50% 100%',
            transition: 'transform 75ms linear',
          }}
        />
      </div>
      <span className="text-[9px] tracking-wide text-muted-foreground uppercase">
        {label}
      </span>
    </div>
  )
}
