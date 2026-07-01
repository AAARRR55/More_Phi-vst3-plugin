'use client'

import { useEffect, useRef, useState, type ElementType, type ReactNode } from 'react'

/**
 * Lightweight scroll-reveal wrapper. Adds `.animate-fade-up` (or `.animate-fade-in`)
 * and toggles `.is-visible` once the element enters the viewport, then unobserves.
 * No animation library, reduced-motion safe (CSS global media query forces the
 * element visible regardless).
 *
 * Usage:
 *   <Reveal as="h2">…</Reveal>
 *   <Reveal delay={120}>…</Reveal>
 *   <Reveal variant="fade-in">…</Reveal>
 */
export function Reveal({
  children,
  as,
  className = '',
  delay = 0,
  variant = 'fade-up',
  threshold = 0.15,
  once = true,
}: {
  children: ReactNode
  as?: ElementType
  className?: string
  /** Stagger delay in ms. */
  delay?: number
  variant?: 'fade-up' | 'fade-in'
  threshold?: number
  once?: boolean
}) {
  const Tag = (as ?? 'div') as ElementType
  const ref = useRef<HTMLElement | null>(null)
  const [visible, setVisible] = useState(false)

  useEffect(() => {
    const el = ref.current
    if (!el) return

    // SSR / no-IO fallback: just show it.
    if (typeof IntersectionObserver === 'undefined') {
      setVisible(true)
      return
    }

    const observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            setVisible(true)
            if (once) observer.unobserve(entry.target)
          } else if (!once) {
            setVisible(false)
          }
        }
      },
      { threshold, rootMargin: '0px 0px -8% 0px' },
    )

    observer.observe(el)
    return () => observer.disconnect()
  }, [threshold, once])

  const variantClass = variant === 'fade-in' ? 'animate-fade-in' : 'animate-fade-up'

  return (
    <Tag
      ref={ref}
      className={`${variantClass} ${visible ? 'is-visible' : ''} ${className}`}
      style={delay ? { transitionDelay: `${delay}ms` } : undefined}
    >
      {children}
    </Tag>
  )
}
