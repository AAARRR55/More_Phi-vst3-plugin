import { Analytics } from '@vercel/analytics/next'
import type { Metadata, Viewport } from 'next'
import { Geist, Geist_Mono, Outfit, Syncopate } from 'next/font/google'
import './globals.css'

const geistSans = Geist({ variable: '--font-geist-sans', subsets: ['latin'] })
const geistMono = Geist_Mono({
  variable: '--font-geist-mono',
  subsets: ['latin'],
})
const outfit = Outfit({
  variable: '--font-outfit',
  subsets: ['latin'],
  weight: ['300', '400', '500', '600', '700'],
})
const syncopate = Syncopate({
  variable: '--font-syncopate',
  subsets: ['latin'],
  weight: ['400', '700'],
})

export const metadata: Metadata = {
  title: 'More-Phi — The Physics of Synthesis | Parameter Morphing VST3/AU Plugin',
  description:
    'More-Phi is an advanced physics-based VST3/AU audio parameter morphing plugin. Morph seamlessly between snapshots with a 2D pad, a spring-damper physics engine, a genetic breeding engine, and a built-in MCP AI assistant. Acquire for $129.',
  generator: 'v0.app',
  keywords: [
    'VST3 plugin',
    'AU plugin',
    'parameter morphing',
    'audio morphing',
    'synthesizer plugin',
    'physics audio engine',
    'More-Phi',
  ],
  openGraph: {
    title: 'More-Phi — The Physics of Synthesis',
    description:
      'Physics-based parameter morphing for VST3/AU. Morph, breed, and evolve your sound.',
    type: 'website',
  },
}

export const viewport: Viewport = {
  themeColor: '#08080A',
  width: 'device-width',
  initialScale: 1,
}

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode
}>) {
  return (
    <html
      lang="en"
      className={`dark bg-background ${geistSans.variable} ${geistMono.variable} ${outfit.variable} ${syncopate.variable}`}
    >
      <body className="font-sans antialiased">
        {children}
        {process.env.NODE_ENV === 'production' && <Analytics />}
      </body>
    </html>
  )
}
