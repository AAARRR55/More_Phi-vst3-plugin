# More-Phi × XPRIZE "Build with Gemini" — Entry Design

**Date:** 2026-06-30
**Status:** Approved (brainstorming complete) → awaiting implementation plan
**Challenge:** [Build with Gemini XPRIZE](https://xprize.devpost.com/) — build a real business powered by Gemini + Google Cloud, judged on Business Viability, AI-Native Operations, and Category Impact.
**Build window:** May 19 – Aug 17, 2026 (submission deadline Aug 17, 1:00 PM PDT). **Time remaining as of 2026-06-30: ~48 days (~7 weeks).** This is a compressed go-to-market, not a full 90-day runway — see §10 for the adjusted distribution timeline.

---

## 1. Product & Positioning

**Product.** More-Phi remains a desktop VST3/AU plugin (the hero product). The XPRIZE entry adds a **license-account-tied cloud morph-bank**: users sync their 12 snapshot slots across machines, and each bank receives a Gemini-generated descriptive label (genre / character / intent) that powers search and a "Featured Presets" surface in the landing dashboard.

**Customer.** Professional audio engineers, producers, and small studios delivering client work (mastering, mix prep, sound design). The professional is the paying customer, consistent with a paid-license revenue model.

**Category.** **Professional Services.** Mastering is literally a professional service; the product maps cleanly onto billable client work, and Category Impact is measurable (time-to-master, tracks/day, turnaround).

**Hard-requirement coverage (the two that disqualify if missed):**

| Requirement | Resolution |
|---|---|
| Gemini API call in the deployed app | The plugin's `RestLlmClient` calls **Gemini via Vertex AI** (OpenAI-compatible endpoint) for bank labeling and on-demand mix coaching. The call is load-bearing — see §3. |
| ≥1 Google Cloud product in critical path | **Firestore** (bank metadata), **Cloud Storage** (binary bank chunks), **Vertex AI** (Gemini), authenticated via **Firebase Auth** (already present in `landing-page/`). Four real GCP products, none vestigial. |

**Pre-existing-work disclosure (mandatory).** More-Phi is a mature v3.4.1 plugin that predates the hackathon. XPRIZE rules permit pre-existing work **if disclosed**. The disclosure line:

- **Pre-existing (disclosed):** plugin DSP, morph/physics/genetic engines, embedded ONNX mastering model, multi-agent runtime, MCP server.
- **New since May 19, 2026 (the entry's center):** the Gemini integration, the cloud morph-bank sync, the license-account business, Stripe checkout, the evidence-capture infrastructure.

Misrepresenting maturity is a disqualifier; honest disclosure with a strong new-work story is permitted and normal. The narrative centers on the new AI-native mastering *service*, with More-Phi as the disclosed foundation.

---

## 2. Architecture & Data Flow

### Components

| Component | Status | Role |
|---|---|---|
| `MorePhiProcessor` (plugin) | Exists | Adds cloud-sync client; triggers Vertex AI Gemini labeling on bank sync |
| `RestLlmClient` (`src/AI/Agents/Llm/`) | Exists | Add `kVertexAi_Gemini` provider path (OpenAI-compatible endpoint). Reuses the `ILlmClient` seam — no new abstraction. |
| `PresetSerializer` / `PresetSerializerV2` (`src/Preset/`) | Exists (1,427 LOC) | Serialize the 12-slot morph bank to the existing V2 format. Reuse, no new serializer. |
| **Cloud-sync client** (`src/Cloud/`, new) | **New** | Thin REST client: auth → upload/list/download banks. Carries a Firebase JWT; never touches the audio thread. |
| **Cloud Run service** (`tools/cloud_sync/`, new) | **New** | Containerized endpoint. Bank CRUD against Firestore + Cloud Storage. Calls Vertex AI Gemini to label each uploaded bank. Holds the Gemini API key server-side (never shipped to users). |
| Firebase Auth | Exists (`landing-page/lib/firebase.ts`) | License-account identity shared by web + plugin |
| Landing dashboard (`landing-page/app/dashboard/`) | Exists as a route | Add "My Banks" + "Featured Presets" views (read Firestore) |
| License/checkout (`landing-page/app/checkout/`) | Exists | Unchanged in shape; wire to Stripe (§4) |

### Data flow — upload a morph bank

```
[Plugin: user hits "Sync to Cloud"]   (message thread / worker — NEVER audio thread)
   → PresetSerializer::serialize(bank)            // existing V2 format
   → Cloud-sync client (Firebase Auth JWT)
   → POST /banks  (Cloud Run)
        → store binary chunk → Cloud Storage
        → write metadata doc → Firestore (userId, name, hash, createdAt)
        → Vertex AI Gemini: summarize(bank metadata + genre + tonal-balance + LUFS prior)
              → descriptive label + tags
        → persist label into Firestore doc
   → return bankId
```

### Data flow — pull on a new machine

```
[Plugin: user logs in → "Pull from Cloud"]
   → GET /banks (Cloud Run, scoped to userId via JWT)
   → Firestore metadata + Cloud Storage signed URL
   → download chunk → PresetSerializer::deserialize → load into 12 slots
```

### Threading discipline (non-negotiable, per AGENTS.md)

All cloud and Gemini calls execute on the **message thread or a dedicated worker** — never the audio thread. Any parameter changes resulting from a synced bank load are enqueued through the existing `LockFreeQueue` → audio-thread drain, exactly like the MCP/agent path. No allocations, no locks, no network on the audio thread. The cloud-sync client is a message-thread-only object.

### Why Cloud Run (not Cloud Functions / GCE)

Cloud Run is the simplest containerized deployment that can hold a Vertex AI session, scale to zero between calls (cost-efficient for a sparse sync workload), and serve both the plugin's REST client and the landing dashboard from one image. Cloud Functions would fragment the surface; GCE is overkill.

---

## 3. Gemini's Role & AI-Native Defense

Judges score AI-Native Operations on whether AI is "fundamentally at the core of operations." A vanity ping fails. So Gemini is **load-bearing** — remove it and the product gets meaningfully worse. Three calls:

**1. Bank labeling (core, load-bearing — runs on every sync).**
Each synced bank carries metadata More-Phi already computes: `GenreClassifier` output, the 8-band `TonalBalanceExtractor` residual, target-LUFS prior, per-slot parameter deltas. Gemini ingests these and returns:
- A **human-readable label** — e.g. *"Warm indie-rock master — controlled low-mids, airy top, −9 LUFS target."*
- **Searchable tags** — genre, character, intensity.

This label populates the dashboard's "My Banks" and "Featured Presets" lists. Without Gemini the user sees raw slot dumps — meaningfully worse. **This is the call that makes AI-Native defensible.**

**2. Mix coaching (secondary, on-demand, user-triggered).**
"Analyze my mix" sends the genuine BS.1770 telemetry + spectral snapshot (already computed by `SonicMasterAnalysisEngine`) to Gemini, which returns 2–3 plain-English coaching notes ("low-mids muddy around 250 Hz; try snapshot 4"). Optional, not in the audio path.

**3. Conductor goal decomposition (already exists, repointed).**
`ConductorAgent::decomposeGoal` already calls `ILlmClient`. Pointing it at Gemini gives the agent layer a real LLM for natural-language mastering goals ("make this louder and brighter"). Pure reuse of existing infrastructure.

**Model choice.** **`gemini-2.5-flash-lite`** (Vertex AI) for labels and coaching — `$0.10 / $0.40` per 1M input/output tokens (Google's cheapest current model). Note: `gemini-2.0-flash` was **shut down June 1, 2026** (today) and must not be used; 2.5 Flash-Lite is the successor for low-reasoning summarization/labeling workloads. Labeling and coaching do not need heavy reasoning, so Flash-Lite (not full 2.5 Flash at `$0.50 / $2–3.50`) is the cost-correct pick. The embedded ONNX mastering model stays local (it is the DSP-grade real-time path; Gemini is the reasoning/labeling layer on top, not the DSP).

**Honest scoping for the narrative.** The real-time mastering *intelligence* is the embedded ONNX model + agent runtime; Gemini is the natural-language reasoning layer (labeling, coaching, goal decomposition). Stated plainly to avoid AI-washing — judges penalize overclaiming harder than honest scoping.

---

## 4. Business Viability & Revenue (1/3 of score — the binding constraint)

No code produces paying customers. This section splits into **what the owner runs** and **what code captures the evidence**.

### Revenue model

License-key purchase (one-time). Cloud sync is **included** with the license — not a separate tier — to keep the value proposition sharp and the model simple.

**Introductory launch pricing (chosen 2026-06-30):**

| Window | Price | Rationale |
|---|---|---|
| **Intro:** May 19 – Aug 17, 2026 (hackathon window) | **$29** | Maximizes user volume — the metric that makes "real users" and Category Impact land in the narrative. Stays above per-user cost (Stripe fees + GCP ≈ $1–3) to preserve visible margin. |
| **Regular:** post-Aug 17 | **$99** | Restores pro-tool margin and credibility signal for ongoing sales after the launch. |

That is a ~70% launch discount — genuine, standard launch economics, not metric-gaming. One-time-license mechanics keep post-deadline clean: window buyers keep their license indefinitely; future buyers pay $99. No grandfathering, no renewals.

**Narrative line for the submission:** *"introductory launch pricing during our launch window to build the initial user base and gather feedback; regular pricing ($99) takes effect post-launch."*

**Why not cheaper ($9 range):** under ~$25 the price undermines the Professional Services positioning (pro audio has a strong price-equals-quality reflex — Ozone is ~$250+) and the margin barely clears per-user cost, weakening the viability story. $29 is the floor of the credible band.

### Distribution motion (OWNER's parallel work — the actual battle)

**Compressed 48-day timeline (as of 2026-06-30):**

1. **Beta + outreach sprint (Days 1–14, Jun 30 – Jul 14).** Recruit 10–20 engineers/producers from r/audioengineering, producer Discords, Gearspace. **In parallel**, launch YouTube creator outreach (15–25 targets, prioritizing responsive small/mid channels). Free beta for testimonials + usage data. Seeds "real users" evidence.
2. **Paid launch + first reviews (Days 15–30, Jul 15 – Jul 30).** Open the existing `checkout/` flow. Target **20–50 paying customers** — at $29 intro, $580–$1,450 documented revenue. Aim for 3–5 creator reviews/mentions live by end of this window (the realistic yield from 48-day outreach).
3. **Case studies + final push (Days 31–47, Jul 31 – Aug 16).** 1–2 engineers who used More-Phi on real client work, with before/after + time-saved metrics (Category Impact evidence). Final revenue push + submission-artifact compilation.

**Why the compression matters:** a 90-day plan allows long-lead creator reviews to land before paid launch. 48 days does not — outreach and beta must run *concurrently*, and we should expect only **3–5** reviews/mentions from 15–25 contacted (creator response rates are ~20–30%, and video turnaround eats 2–3 weeks). See §10 for the full marketing plan.

### Evidence-capture code (buildable — closes the submission-artifact loop)

- **Revenue evidence:** wire the existing `checkout/` routes to **Stripe** (consistent with the Firebase + Next.js stack). Each transaction records amount, timestamp, customer, cost basis. Produces the exact artifact XPRIZE wants: total/monthly revenue, costs, marketing spend.
- **User evidence:** Firebase Auth signups are already the user ledger. Add lightweight, opt-in usage telemetry (banks synced, sessions, Gemini calls) to Firestore so "real users" is data-backed, not screenshot-backed.
- **AI execution logs:** every Gemini call through Cloud Run logs request / response / latency / model to **Cloud Logging**. Directly satisfies the "production evidence & AI execution logs" submission requirement.

### Go/no-go signal

If by **Aug 1** there are fewer than ~10 paying users, the entry is weak on Business Viability regardless of code quality. Set this checkpoint now.

---

## 5. Submission Artifacts & Owners

| Artifact | Owner | Source |
|---|---|---|
| Source code repository | Code (me) + Owner (public release decision) | Exists; cloud-sync code added with clean commits |
| 3-minute video pitch | **Owner** | Must demo real product + real customer + real revenue. No code substitutes. Script outline draftable. |
| Written narrative (500–1,000 words) | Draft (me) + Truth-check (Owner) | Written against the three judging pillars; Owner verifies revenue/user claims |
| Revenue + user + cost evidence | Owner (runs business) + Code (capture infra) | Stripe receipts + Firestore user ledger + Cloud Logging |
| AI execution logs | Code (infra) | Cloud Logging on every Vertex AI Gemini call — auto-generated |

---

## 6. Scope Boundaries (YAGNI)

Explicitly **out of scope** for the hackathon window (deferred unless the business proves out):

- Community preset marketplace (browse/share/download others' banks). Adds a discovery loop but also moderation/population burden — not worth the focus cost in 90 days.
- Cloud-side mastering reference (upload a track, cloud returns a mastered reference). Strongest AI-Native story but creeps toward SaaS and pulls the plugin off-center.
- Subscription tier. License-key keeps the model simple; recurring revenue can come later.

These are noted as post-hackathon extensions, not v1 work.

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Thin GCP story reads as bolted-on to judges | Gemini labeling is load-bearing (§3); four real GCP products in the critical path (§1) |
| Real revenue doesn't materialize by Aug 17 | Go/no-go checkpoint at Aug 1 (§4); beta cohort seeds early momentum |
| AI-washing perception (ONNX does the DSP, Gemini is "just" labeling) | Honest scoping in narrative — Gemini is the reasoning layer, ONNX is the DSP layer. Both real. |
| Pre-existing-work disclosure rejected | Disclosure is explicit and rules-permitted; entry centers on genuinely new Gemini+GCP+business work |
| Gemini API key leakage | Key lives server-side in Cloud Run only; plugin carries a Firebase JWT, never the Gemini key |
| Audio-thread violation from cloud calls | Cloud-sync client is message-thread/worker-only; results flow through the existing `LockFreeQueue` (§2) |

---

## 8. Open Items Before Implementation

1. **License pricing** — **DECIDED:** $29 intro (May 19 – Aug 17) → $99 regular. Reflected in §4.
2. **Stripe vs. alternative payment processor** — Stripe recommended (Firebase+Next.js native fit); confirm.
3. **GCP project + billing account** provisioning (owner) — needed before Cloud Run/Firestore/Vertex AI can be wired.
4. **Gemini model ID** — **DECIDED:** `gemini-2.5-flash-lite` (2.0 Flash deprecated June 1, 2026). Reflected in §3.
5. **Public repo decision** — XPRIZE wants a source-code repository; owner decides private-during-build vs. public.
6. **GCP account status** (owner) — confirm whether the GCP account is new (eligible for the $300/90-day free trial) or existing. Affects the cost model in §9.
7. **Windows code-signing certificate** (owner decision) — see §9 caveat. Not counted in the cost model; flagged for legitimacy vs cost.

---

## 9. Cost Model & Marginal Economics

Validated 2026-06-30 against live pricing. This section doubles as the source for the submission's **"costs" evidence artifact**.

### Cost drivers (what we actually pay)

| Driver | Normal price | What we pay | Why |
|---|---|---|---|
| Domain (`more-phi.tech`) | ~$15/yr | **$0** | Owner-owned |
| Stripe fees | 2.9% + $0.30/sale | **$0 on first $1,000 revenue**, then standard | GitHub Student Pack Stripe benefit ("Waived transaction fees on first $1,000 in revenue processed") |
| Gemini (2.5 Flash-Lite) | $0.10 / $0.40 per M tokens | **~$0–$1 / window** | Calls are small (structured metadata → short label); low call volume |
| Firestore + Cloud Storage + Cloud Run | pay-as-you-go | **$0** | Free tiers cover our volume; GCP new-account $300/90-day credit covers the whole window |
| Landing-page hosting | ~$20/mo | **$0** | Vercel free tier (Next.js app) |

### Marginal cost per customer (after the $1k Stripe waiver expires)

- Stripe fee: 2.9% × $29 + $0.30 = **$1.14 / sale**
- GCP per customer (entire window): ~5 bank syncs × ~$0.00011/call ≈ **$0**
- **Marginal cost per customer ≈ $1.14** (≈3.9% of the $29 price)

### Scenarios (90-day window, $29 intro price)

| Customers sold | Revenue | Stripe fees | GCP | **Total expenses** | **Net profit** | **Margin** |
|---|---|---|---|---|---|---|
| 25 | $725 | $0 (under $1k waiver) | $0 | **~$0** | $725 | ~100% |
| 50 | $1,450 | $18 (only the $450 past $1k) | $0 | **~$18** | $1,432 | ~99% |
| 100 | $2,900 | $75 (the $1,900 past $1k) | ~$1 | **~$76** | $2,824 | ~97% |

### Caveats (these matter — read before launch)

1. **Stripe waiver claiming bug.** Multiple users report trouble activating the GitHub Student Pack Stripe benefit (GitHub discussion #171064). **Verify the waiver activates before launch** — link the Pack to Stripe early and confirm the fee credit appears. If it fails, the 100-customer scenario rises from ~$76 to ~$171 in fees (still cheap, but confirm).
2. **Firebase Storage requires the Blaze (billing) plan** since Oct 1, 2025 — even though the free tier keeps it $0. GCP billing must be enabled. **Set the storage location to `us-west1`** to stay on the "Always Free" tier.
3. **GCP $300 free trial** is one-time, 90 days, new accounts only. Aligns with the hackathon window. If the owner's GCP account has prior usage, the trial may not apply — confirm (§8 item 6).
4. **Rate-limit Gemini.** "Analyze my mix" is user-triggered — cap it (e.g., 10 calls/day/user) so one user can't spike costs. Risk is small at Flash-Lite prices but the rate-limit is mandatory defensive engineering.
5. **No GCP credit in the GitHub Student Pack.** The pack includes Azure ($100), DigitalOcean ($200), Heroku, MongoDB — but **not GCP**. GCP is the only place paid out-of-pocket, mitigated by the free trial + free tiers above.
6. **Windows code-signing certificate (optional, owner decision).** Without it, Windows SmartScreen shows a warning on the installer. OV cert ~$100–200/yr; EV ~$300/yr. Many launch-time devs skip it initially (users bypass). **Not counted in the table.** Legitimacy-vs-cost tradeoff.

### What this means for the score

Near-zero marginal cost and ~97–100% margin is genuinely a Business Viability strength. The submission's "costs" artifact should document transparent Stripe + GCP invoices against the revenue ledger — clean, auditable, near-pure margin.

---

## 10. Marketing & Distribution Plan (48-Day Compressed GTM)

Full version in `docs/xprize-submission/marketing-plan.md`. Summary here.

### Positioning & target audience

- **Positioning:** "The AI-native morphing mastering studio for working engineers — morph between parameter snapshots with physics-based interpolation, guided by Gemini." Hero = plugin. Differentiator = morph + AI labeling (not "another AI master button").
- **Primary audience:** working engineers/producers delivering client work (Professional Services category).
- **Secondary audience:** serious hobbyists/sound designers who'll adopt the morph workflow.

### Key messages (3 pillars, one per judging criterion)

1. **Business Viability:** "Real tool, real price ($29 launch), real users." Documented Stripe + GCP invoices.
2. **AI-Native:** "Gemini labels every morph bank; the ONNX model does real-time DSP." Honest scoping.
3. **Category Impact:** "Time saved per client master" — quantified in case studies.

### Channel budget

**~$150–250 total marketing spend** (consistent with near-zero cost structure, §9):
- Giveaway prizes (5 × $29 licenses + 1 × $99 future): **~$0 hard cost** (digital licenses), framed at retail value ~$245.
- YouTube creator licenses (20 × NFR perpetual): **$0 hard cost**.
- Optional boosted posts / r/audioengineering contest prizes: **~$50–150**.
- All其余 distribution is earned (creator reviews, community posts) — $0.

### Giveaway strategy

- **Mechanic:** "5 lifetime licenses + 1 $99 voucher" giveaway, entry = email signup + follow + (optional) share for extra entries. Run **Days 8–22** (one mid-window burst, not sustained — 48 days doesn't allow multiple cycles).
- **Distribution:** landing-page signup (already exists) → email list; announce on r/audioengineering (with moderator permission), producer Discords, YouTube creator collaborations.
- **Projected reach:** 200–500 email signups; convert 3–8% post-giveaway → 6–40 paid. *Low-confidence projection — adjust after Days 1–14 beta signals.*

### YouTube outreach (15–25 creators)

**Tiered by reachability, not just size** (critical for 48-day window):
- **Tier A (anchor, 2–3):** mid-size (50K–300K) channels known to review plugins — e.g. *In The Mix* (FL Studio, hundreds of thousands of subs), *Reid Stefan* (annual plugin awards), *Music By Mattie*. Lower response rate but high payoff.
- **Tier B (workhorses, 8–12):** small/growing (5K–50K) channels — *Akayo*, *Jonsine*, *Synthet*, *Sanjay C*, *AutomaticGainsay/Marc Doty* (~52K). **Highest response rate; primary yield source.**
- **Tier C (long-shots, 5–10):** larger channels (300K+) — outreach but don't depend on them.

**Offer:** free NFR (not-for-resale) perpetual license + early access + named credit. **Ask:** honest review (no paid placement — disclosure matters for trust + XPRIZE integrity).

**Realistic yield:** expect **3–5 reviews/mentions** from 20 contacted at ~20–30% response rate, with 2–3 week video turnaround. Plan the paid launch to *not* depend on reviews landing.

### Social & community

- **Platforms:** Reddit (r/audioengineering, r/mixingmastering — value posts, not ads), X/Bluesky (build-in-public), producer Discord servers.
- **Cadence:** 3–4 value posts/week; 1 demo video/week.
- **Themes:** morph workflow demos, before/after masters, "Gemini labeled this bank" teardowns.

### Metrics tracked

Email signups, giveaway entries, creator response rate, reviews live, referral traffic (UTM), conversion rate, paying customers, revenue, CAC (should be ~$0), Gemini call volume/cost.

### ⚠️ Honest constraints (read before executing)

1. **48 days is tight for creator marketing.** Expect missed deadlines; don't gate paid launch on reviews.
2. **Subscriber counts in the full plan are estimates** — verify each channel's current count and responsiveness before outreach (the full plan flags which are confirmed vs. estimated).
3. **No paid placements.** XPRIZE integrity + audience trust both require honest reviews; paid content must be disclosed.
4. **The binding constraint remains paying customers**, not awareness. Marketing exists to convert; the cost model (§9) means even modest conversion is profitable.

---

## Next step

Invoke the **writing-plans** skill to produce the detailed implementation plan (build sequence, file-level changes, test plan, milestone schedule against the Aug 17 deadline).
