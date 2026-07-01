# More-Phi × XPRIZE — Cost Model & Marginal Economics

**Purpose:** Source-of-truth for the submission's **"costs" evidence artifact** and for go/no-go decisions during the window. Validated 2026-06-30 against live pricing.

**Window:** May 19 – Aug 17, 2026 (90 days). **Intro price:** $29. **Regular price (post-window):** $99.

---

## What we actually pay

| Cost driver | Normal price | Our cost | Why |
|---|---|---|---|
| Domain (`more-phi.tech`) | ~$15/yr | **$0** | Owner-owned |
| Stripe transaction fees | 2.9% + $0.30 / sale | **$0 on first $1,000 revenue**, then standard | GitHub Student Developer Pack — "Waived transaction fees on first $1,000 in revenue processed" |
| Gemini (`gemini-2.5-flash-lite`, Vertex AI) | $0.10 / $0.40 per 1M tokens | **~$0–$1 / window** | Small calls (structured metadata → short label), low volume |
| Firestore + Cloud Storage + Cloud Run | pay-as-you-go | **$0** | Free tiers cover our volume; GCP new-account $300 / 90-day credit covers the window |
| Landing-page hosting | ~$20/mo | **$0** | Vercel free tier |

---

## Marginal cost per customer (after the $1k Stripe waiver expires)

| Component | Cost |
|---|---|
| Stripe fee on $29 sale (2.9% + $0.30) | **$1.14** |
| GCP per customer, entire window (~5 bank syncs) | **~$0** |
| **Marginal cost per customer** | **≈ $1.14 (3.9% of $29)** |

---

## Scenarios

| Customers | Revenue | Stripe fees | GCP | **Total expenses** | **Net profit** | **Margin** |
|---|---|---|---|---|---|---|
| 25 | $725 | $0 (waiver) | $0 | **~$0** | $725 | ~100% |
| 50 | $1,450 | $18 | $0 | **~$18** | $1,432 | ~99% |
| 100 | $2,900 | $75 | ~$1 | **~$76** | $2,824 | ~97% |

**Target for the entry:** 50 customers ($1,450 revenue, ~99% margin). Stretch: 100.

---

## Pre-launch checklist (cost-related)

- [ ] Activate GitHub Student Pack → Stripe benefit; **confirm the fee waiver appears** before opening checkout (known claiming bug, GitHub discussion #171064).
- [ ] Enable GCP billing (Blaze plan required for Firebase Storage since Oct 1, 2025). Set storage location to **`us-west1`** to stay on the Always-Free tier.
- [ ] Confirm GCP account is new → $300 / 90-day free trial active. If existing account, recompute scenarios without the trial.
- [ ] Rate-limit the "Analyze my mix" Gemini call (e.g., 10 calls/day/user) — defensive against cost spikes.
- [ ] Decide on Windows code-signing certificate (OV ~$100–200/yr / EV ~$300/yr) — optional, not in the table.

---

## Submission evidence we will produce

- **Stripe dashboard export:** per-transaction ledger (amount, timestamp, customer, fee) — directly satisfies "total revenue, monthly revenue, costs."
- **GCP billing export:** Firestore + Cloud Storage + Cloud Run + Vertex AI line items, demonstrating near-zero cost.
- **AI execution logs:** Vertex AI call log (request/response/latency/model) via Cloud Logging.
- **User ledger:** Firebase Auth signups + opt-in usage telemetry (banks synced, sessions).

---

## Sources

- [Stripe pricing](https://stripe.com/pricing)
- [GitHub Student Developer Pack](https://education.github.com/pack) (Stripe waiver: "first $1,000 in revenue processed")
- [Firebase pricing](https://firebase.google.com/pricing)
- [Gemini API pricing](https://ai.google.dev/gemini-api/docs/pricing)
- [Firebase AI Logic — model deprecation (2.0 Flash shut down June 1, 2026)](https://firebase.google.com/docs/ai-logic/models)
- [Firebase Storage Blaze-plan requirement](https://www.reddit.com/r/Firebase/comments/1gj9lja/firebase_storage_no_longer_accessible_under_spark/)
