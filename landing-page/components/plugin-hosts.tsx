import { Reveal } from './reveal'

const PLUGIN_GROUPS = [
  {
    category: 'Synthesizers',
    plugins: [
      'Serum 2',
      'Massive X',
      'Omnisphere 2',
      'Pigments 6',
      'Vital',
      'Sylenth1',
      'Spire',
      'Diva',
      'Repro-5',
      'Phase Plant',
      'Hive 2',
      'Nexus 5',
      'Avenger 2',
      'Zebra2 / Zebra3',
    ],
  },
  {
    category: 'Effects & Dynamics',
    plugins: [
      'FabFilter Pro-Q 4',
      'FabFilter Pro-C 2',
      'FabFilter Pro-L 2',
      'Soothe2',
      'Valhalla VintageVerb',
      'OTT',
      'Kickstart 2',
      'ShaperBox 3',
      'Gullfoss',
      'Saturn 2',
      'Decapitator',
      'EchoBoy',
    ],
  },
  {
    category: 'Mixing & Mastering',
    plugins: [
      'Waves CLA-2A',
      'Waves SSL E-Channel',
      'iZotope Ozone 11',
      'iZotope Neutron 5',
      'UAD LA-2A',
      'UAD 1176',
      'Slate Digital VMR',
      'Plugin Alliance bx_console',
      'Sonnox Oxford',
      'DMG Audio EQuilibrium',
    ],
  },
  {
    category: 'Instruments & Samplers',
    plugins: [
      'Kontakt 7',
      'Native Instruments Komplete',
      'Spitfire LABS',
      'EastWest Opus',
      'Spectrasonics Keyscape',
      'Spectrasonics Trilian',
      'Arturia V Collection',
      'UVI Falcon',
      'Output Arcade',
      'XLN Addictive Drums 2',
    ],
  },
]

export function PluginHosts() {
  return (
    <section
      id="hosts"
      className="relative mx-auto w-full max-w-7xl scroll-mt-24 px-5 py-24"
    >
      <div className="mx-auto max-w-2xl text-center">
        <span className="text-xs font-medium tracking-[0.25em] text-cyan uppercase">
          Works with what you own
        </span>
        <h2 className="mt-4 font-heading text-3xl font-bold tracking-tight text-balance sm:text-4xl">
          It already speaks your{' '}
          <span className="text-gradient-aurora animate-gradient-pan">plugins' language.</span>
        </h2>
        <p className="mx-auto mt-5 max-w-xl text-pretty text-sm leading-relaxed text-muted-foreground">
          More&#8211;Phi hosts any VST3 instrument or effect directly inside its
          engine, snapshots the full parameter state, and morphs across it in
          real time. Here are a few of the 40+ it runs seamlessly today.
        </p>
      </div>

      <div className="mt-14 grid gap-4 sm:grid-cols-2">
        {PLUGIN_GROUPS.map((group, gi) => (
          <Reveal key={group.category} delay={gi * 80}>
            <div className="gold-leaf glass group h-full rounded-2xl border border-slate-line p-6 transition-all duration-300 hover:-translate-y-1 hover:border-cyan/40 hover:halo-cyan">
              <div className="flex items-center gap-3">
                <span className="relative flex size-7 items-center justify-center rounded-lg border border-gold/30 bg-gold/10">
                  <span className="absolute inset-0 rounded-lg bg-gold/10 blur-md" />
                  <span className="relative size-2 rounded-full bg-gradient-to-tr from-cyan via-gold to-magenta" />
                </span>
                <h3 className="font-heading text-sm font-bold tracking-wide text-primary">
                  {group.category}
                </h3>
              </div>
              <ul className="mt-5 flex flex-wrap gap-2">
                {group.plugins.map((name) => (
                  <li key={name}>
                    <span className="shimmer-sweep inline-flex items-center gap-1.5 rounded-full border border-slate-line bg-foreground/[0.02] px-3 py-1.5 text-xs font-medium text-muted-foreground transition-all hover:border-cyan/50 hover:bg-foreground/[0.04] hover:text-foreground">
                      <span className="size-1 rounded-full bg-cyan/70" />
                      {name}
                    </span>
                  </li>
                ))}
              </ul>
            </div>
          </Reveal>
        ))}
      </div>

      <p className="mx-auto mt-10 max-w-2xl text-center text-xs leading-relaxed text-muted-foreground/70">
        Plugin names and trademarks are the property of their respective owners
        and are listed for compatibility reference only. More&#8211;Phi is not
        affiliated with or endorsed by these manufacturers.
      </p>
    </section>
  )
}
