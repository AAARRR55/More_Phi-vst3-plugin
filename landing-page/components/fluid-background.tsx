'use client'

import { useEffect, useRef } from 'react'

/**
 * Fluid particle-web background representing physics-based interpolation.
 * Particles drift, connect to neighbors, and are attracted toward the cursor.
 * Rendered on a single HTML5 canvas, animation capped to the display refresh.
 */
export function FluidBackground() {
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    let width = 0
    let height = 0
    let dpr = Math.min(window.devicePixelRatio || 1, 2)
    const mouse = { x: -9999, y: -9999, active: false }

    type P = { x: number; y: number; vx: number; vy: number; r: number; hue: number }
    let particles: P[] = []

    const init = () => {
      width = canvas.clientWidth
      height = canvas.clientHeight
      dpr = Math.min(window.devicePixelRatio || 1, 2)
      canvas.width = width * dpr
      canvas.height = height * dpr
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
      const count = Math.min(110, Math.floor((width * height) / 14000))
      particles = Array.from({ length: count }, () => ({
        x: Math.random() * width,
        y: Math.random() * height,
        vx: (Math.random() - 0.5) * 0.25,
        vy: (Math.random() - 0.5) * 0.25,
        r: Math.random() * 1.6 + 0.6,
        hue: Math.random() > 0.5 ? 190 : 310,
      }))
    }

    const onResize = () => init()
    const onMove = (e: MouseEvent) => {
      const rect = canvas.getBoundingClientRect()
      mouse.x = e.clientX - rect.left
      mouse.y = e.clientY - rect.top
      mouse.active = true
    }
    const onLeave = () => {
      mouse.active = false
      mouse.x = -9999
      mouse.y = -9999
    }

    let raf = 0
    const linkDist = 140

    const render = () => {
      ctx.clearRect(0, 0, width, height)

      for (const p of particles) {
        // gentle attraction toward cursor
        if (mouse.active) {
          const dx = mouse.x - p.x
          const dy = mouse.y - p.y
          const dist = Math.hypot(dx, dy)
          if (dist < 220 && dist > 0.01) {
            const force = (1 - dist / 220) * 0.04
            p.vx += (dx / dist) * force
            p.vy += (dy / dist) * force
          }
        }
        p.x += p.vx
        p.y += p.vy
        p.vx *= 0.985
        p.vy *= 0.985
        // wrap
        if (p.x < -20) p.x = width + 20
        if (p.x > width + 20) p.x = -20
        if (p.y < -20) p.y = height + 20
        if (p.y > height + 20) p.y = -20
      }

      // links
      for (let i = 0; i < particles.length; i++) {
        const a = particles[i]
        for (let j = i + 1; j < particles.length; j++) {
          const b = particles[j]
          const dx = a.x - b.x
          const dy = a.y - b.y
          const d = Math.hypot(dx, dy)
          if (d < linkDist) {
            const alpha = (1 - d / linkDist) * 0.18
            const hue = (a.hue + b.hue) / 2
            ctx.strokeStyle = `hsla(${hue}, 90%, 60%, ${alpha})`
            ctx.lineWidth = 0.6
            ctx.beginPath()
            ctx.moveTo(a.x, a.y)
            ctx.lineTo(b.x, b.y)
            ctx.stroke()
          }
        }
      }

      // nodes
      for (const p of particles) {
        ctx.beginPath()
        ctx.fillStyle = `hsla(${p.hue}, 95%, 65%, 0.7)`
        ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2)
        ctx.fill()
      }

      // cursor halo
      if (mouse.active) {
        const grad = ctx.createRadialGradient(
          mouse.x,
          mouse.y,
          0,
          mouse.x,
          mouse.y,
          180,
        )
        grad.addColorStop(0, 'hsla(200, 95%, 60%, 0.08)')
        grad.addColorStop(1, 'hsla(310, 95%, 60%, 0)')
        ctx.fillStyle = grad
        ctx.fillRect(mouse.x - 180, mouse.y - 180, 360, 360)
      }

      raf = requestAnimationFrame(render)
    }

    init()
    render()
    window.addEventListener('resize', onResize)
    window.addEventListener('mousemove', onMove)
    canvas.addEventListener('mouseleave', onLeave)

    return () => {
      cancelAnimationFrame(raf)
      window.removeEventListener('resize', onResize)
      window.removeEventListener('mousemove', onMove)
      canvas.removeEventListener('mouseleave', onLeave)
    }
  }, [])

  return (
    <canvas
      ref={canvasRef}
      id="fluid-bg-canvas"
      aria-hidden="true"
      className="pointer-events-none absolute inset-0 h-full w-full"
    />
  )
}
