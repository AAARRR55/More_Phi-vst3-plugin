import { Checkout } from '@/components/checkout'
import { FeaturesGrid } from '@/components/features-grid'
import { Guarantee } from '@/components/guarantee'
import { Hero } from '@/components/hero'
import { MorphPad } from '@/components/morph-pad'
import { PluginHosts } from '@/components/plugin-hosts'
import { SiteHeader } from '@/components/site-header'
import { SiteFooter, SpecsFaq } from '@/components/specs-faq'
import { StickyCta } from '@/components/sticky-cta'
import { TrustBar } from '@/components/trust-bar'

export default function Page() {
  return (
    <>
      <SiteHeader />
      <main>
        <Hero />
        <TrustBar />
        <MorphPad />
        <FeaturesGrid />
        <PluginHosts />
        <Guarantee />
        <Checkout />
        <SpecsFaq />
      </main>
      <SiteFooter />
      <StickyCta />
    </>
  )
}
