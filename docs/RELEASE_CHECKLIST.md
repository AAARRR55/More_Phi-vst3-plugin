# Pre-Release Checklist — More-Phi v1.0.0

**Status:** Draft — executable, not advisory.
**Scope:** Turns the "B3 acceptance gate" named in
[`docs/validation/PRODUCTION_READINESS_AUDIT_2026-06-18.md`](validation/PRODUCTION_READINESS_AUDIT_2026-06-18.md)
into concrete commands, and adds the **repo-hygiene** and **WIP-reconciliation**
steps that the audit doc does not cover but which block any clean tag/release.

**Relationship to existing docs (do not duplicate):**
- DSP / concurrency / serialization evidence → see audit doc above.
- Per-module sign-off template → [`docs/audits/QA-SignOff-Matrix.md`](audits/QA-SignOff-Matrix.md).
- Per-DAW manual smoke steps → `docs/audits/QA-Manual-Checklist-{FLStudio,Ableton,Logic}.md`.
- Version history (Keep-a-Changelog) → [`CHANGELOG.md`](../CHANGELOG.md).

**Verified ground state at time of writing (2026-06-19):**
- Branch `chore/ponytail-dead-code-cleanup` is **16 commits ahead of `main`, 0 behind** → fast-forward merge is possible.
- `main` is an ancestor of the branch → no merge commit required *if* working tree is clean.
- **Working tree is NOT clean:** 14 tracked files modified (incl. `src/AI/MCPServer.cpp`, `MCPToolHandler.cpp`, `Core/LUFSMeter.cpp`, `TruePeakEstimator.h`, `GranularMorphEngine.cpp`, 3 test files). This WIP must be reconciled before any merge.
- **567 Catch2 cases registered** (audit doc cites **520 / 87,445 assertions**; the 520 figure is post-filter runnable count, not registration count — both are valid, do not "correct" one to the other).
- `pluginval` is **not installed** on this host.
- `MORE_PHI_ENABLE_SANITIZERS` is **Clang/GCC-only** — this Windows host is MSVC/MinGW, so the ASan pass must run on Linux or a Clang runner.

---

## ⛔ Steps that require explicit human sign-off

The following are **destructive or outward-facing**. Do not execute on autopilot.
Each is marked `[CONFIRM]`. The release engineer initials each before it runs.

1. `git merge` / `git push` to `main`
2. Deleting any untracked file (see §2)
3. Splitting or removing `store-backend/` (306 MB)
4. `git tag` / `git push --tags`
5. Publishing a release artifact (GitHub Releases, installer upload)
6. Publishing the product/pricing page

---

## Phase 0 — Reconcile uncommitted WIP (BLOCKER)

The branch cannot merge cleanly while tracked production files are modified
in the working tree. The WIP is the release engineer's to commit, stash, or
discard — **this checklist does not do it for you.**

```bash
# 1. Review what's modified and decide per-file:
git status
git diff --stat src/ tests/
git diff src/AI/MCPServer.cpp            # inspect, don't blind-accept

# 2a. Commit the WIP (if it's intended work):
git add src/ tests/ docs/
git commit -m "chore(ponytail): finalize <topic>"

# 2b. OR stash it (if it's exploratory):
git stash push -m "pre-release-wip" src/ tests/ docs/

# 3. Verify clean:
git status --porcelain src/ tests/        # must be empty
```

**Exit criterion:** `git status --porcelain src/ tests/` returns nothing.

---

## Phase 1 — Repo hygiene (BLOCKER)

### 1a. Remove confirmed junk files

These were inspected on 2026-06-19 and are not referenced by any source/test
script. All are untracked, so `rm` is safe — no history impact.

```bash
# [CONFIRM] — junk debug/test output
rm -f nul                                # 51-byte Windows null-device artifact
rm -f signup_console_errors.txt          # landing-page debug output
rm -f signup_127_console_errors.txt      # landing-page debug output
rm -f mcp_integration_test_output.txt    # 0-byte MCP test capture
```

### 1b. DO NOT delete these (verified referenced)

| File | Why it stays |
|------|-------------|
| `pink_noise.wav` (960 KB) | Referenced by `src/AI/Dataset/fl_studio_dataset_generator.py` and `scripts/generate_test_audio.py` |
| `store-backend/` (306 MB) | **Decide, don't delete.** See §1c. |

### 1c. `store-backend/` and `landing-page/` — verified NOT a problem (2026-06-19)

Earlier drafts of this checklist flagged `store-backend/` as a "306 MB ship blocker."
**That was wrong.** Verified ground state:

- `store-backend/` is **57 tracked files, ~422 KB** of source — it is the project's
  actual purchasing/licensing backend (`.env.example`, refresh-token logic, audit fixes).
  The 306 MB is untracked `node_modules/` and build artifacts sitting in the directory;
  **none of it entered git history.**
- It is **already on `main`** (3 commits, incl. `feat(store): import existing
  purchasing/licensing backend` and `fix(store): revoke consumed refresh tokens`).
- Conclusion: it **belongs in the repo** as the product's commerce layer. No action needed.

`landing-page/` is **0 tracked files** — genuinely local-only marketing site. It is
already effectively ignored by virtue of never having been `git add`ed; no `.gitignore`
entry is required, but adding one is harmless:

```bash
# Optional — landing-page/ is untracked and never staged; this just makes it explicit.
echo "landing-page/" >> .gitignore
```

**Exit criterion:** none required. Both directories are in their correct state.

---

## Phase 2 — Merge to main (after Phase 0 + 1)

Fast-forward is possible because `main` is an ancestor of the branch.

```bash
# 1. Confirm branch is still ahead and clean:
git checkout chore/ponytail-dead-code-cleanup
git status --porcelain                     # must be empty
git rev-list --left-right --count main...HEAD   # expect "0   16"

# 2. [CONFIRM] — fast-forward main to branch tip
git checkout main
git merge --ff-only chore/ponytail-dead-code-cleanup

# 3. [CONFIRM] — push
git push origin main

# 4. Optional cleanup (only after CI green on main):
# git branch -d chore/ponytail-dead-code-cleanup
```

**Exit criterion:** `main` HEAD == branch tip; CI green on `main`.

---

## Phase 3 — B3 acceptance gates

These are the gates named in the production-readiness audit. Each maps to a
row in [`docs/audits/QA-SignOff-Matrix.md`](audits/QA-SignOff-Matrix.md);
fill that matrix as you complete each gate.

### 3a. Clean build matrix

```bash
# Windows MSVC (VST3) — run from a "x64 Native Tools Command Prompt for VS"
cmake --preset windows-msvc-safe            # or: -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build --preset windows-safe --config Release --parallel 4

# macOS (VST3 + AU, Universal)
cmake -B build-macos -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build-macos --config Release

# Linux (ASAN/UBSAN) — sanitizer build is Clang/GCC only, NOT MSVC
cmake -B build-asan -S . -G "Ninja" \
  -DMORE_PHI_BUILD_TESTS=ON \
  -DMORE_PHI_ENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang
cmake --build build-asan --parallel 4
```

**Focus for the sanitizer pass:** the concurrent MCP load/unload path. The
commit `837659c` fixed a use-after-free there (CPP-H2); ASan must re-prove
it's clean under concurrent stress, not just the unit suite.

**Exit criterion:** all three builds produce a plugin binary with zero warnings-as-errors violations.

### 3b. Full test suite

```bash
ctest --test-dir build --build-config Release --output-on-failure --parallel 4
# Linux sanitizer:
ctest --test-dir build-asan --build-config Debug --output-on-failure --parallel 4
```

**Expected:** **520 test cases / 87,445 assertions, all green** (per 2026-06-19
audit). Registration count is 567 — the delta is filtered/skipped cases, not failures.

### 3c. pluginval (NOT installed — install first)

```bash
# Install (one-time):
#   Windows:  choco install pluginval
#   macOS:    brew install pluginval
#   Manual:   https://github.com/Tracktion/pluginval/releases
pluginval --version

# Validate the built VST3 at strictness 5:
pluginval --strictness-level 5 \
  --validate "build/MorePhi_artefacts/Release/VST3/MorePhi.vst3" \
  --output-dir validation/pluginval/

# Repeat for AU on macOS:
pluginval --strictness-level 5 \
  --validate "build-macos/MorePhi_artefacts/Release/AU/MorePhi.component"
```

**Exit criterion:** `pluginval` reports 0 failures at strictness 5 for every format.

### 3d. Steinberg vst3_validator (if available)

Optional but recommended. Bundled with the VST3 SDK; not in this repo.

### 3e. DAW smoke tests (manual)

Follow the per-DAW checklists; record results in the QA-SignOff-Matrix:

| DAW | Checklist | Min. version |
|-----|-----------|-------------|
| FL Studio | [`QA-Manual-Checklist-FLStudio.md`](audits/QA-Manual-Checklist-FLStudio.md) | 21 |
| Ableton Live | [`QA-Manual-Checklist-Ableton.md`](audits/QA-Manual-Checklist-Ableton.md) | 12 |
| Logic Pro | [`QA-Manual-Checklist-Logic.md`](audits/QA-Manual-Checklist-Logic.md) | 11 |
| Reaper | *(none yet — author one if shipping to Reaper users)* | 7 |

**Exit criterion:** every shipped-DAW checklist signed PASS in the matrix.

---

## Phase 4 — Licensing decision (REQUIRED before any paid binary)

Per [JUCE 8 EULA](https://juce.com/legal/juce-8-licence/), the binary is
governed by JUCE's terms, not the repo's MIT source license. Decide the tier
at which this release will operate:

| Tier | Revenue/funding ceiling | Cost |
|------|------------------------|------|
| Personal | ≤ $20,000/yr | Free |
| Starter | ≤ $300,000/yr | $800 perpetual |
| Indie | No limit | $3,500 perpetual |
| Pro | No limit | Higher |

```bash
# Record the decision in the release notes / a licensing doc:
# - "This binary is released under JUCE <Tier> tier."
# - State the revenue ceiling the release will respect.
# - If Personal: ensure annual revenue is tracked so you re-tier before $20K.
```

**Exit criterion:** tier chosen and documented; revenue-tracking in place if Personal.

---

## Phase 5 — Marketing copy against shipped capability (REQUIRED)

The README already enforces "capability vs. roadmap" discipline on the code
(see its "Analysis, Metering, and AI Claim Boundaries" section). The same
discipline must govern the product page. At launch:

- ✅ **MCP server, arbitrary-plugin hosting, 12-slot morph, physics modes,
  genetic breeding** → describe as shipped features.
- ⚠️ **Neural mastering, genre classifier, VAE morph** → must be labeled as
  heuristic/stub/fallback *exactly as the code reports them*
  (`heuristic_rule_engine`, `default_fallback`, "safe stub backend").
- ❌ Do not market "$199 — AI-powered mastering" while `VAEMorphEngine`
  returns stub output.

**Exit criterion:** product page copy audited against
[`README.md`](../README.md) "AI Claim Boundaries" section; no claim outruns
what the binary does.

---

## Phase 6 — Tag and release (FINAL, all above green)

```bash
# 1. [CONFIRM] — tag
git tag -a v1.0.0 -m "More-Phi v1.0.0 — see CHANGELOG.md and docs/audits/"
git push origin v1.0.0

# 2. [CONFIRM] — build the installer (Windows, requires Inno Setup 6)
powershell -ExecutionPolicy Bypass -File ./scripts/build-installer.ps1

# 3. [CONFIRM] — publish GitHub Release with artifacts:
#    - morphy-Setup-<version>.exe
#    - MorePhi.vst3 (Windows)
#    - MorePhi.vst3 + MorePhi.component (macOS, notarized)
```

**Exit criterion:** release is public, artifacts download and install on a clean machine, plugin loads in a DAW.

---

## Summary — what "ready for production" requires, concretely

| Phase | What | Who | Auto? |
|-------|------|-----|-------|
| 0 | Reconcile WIP | Release engineer | No — it's your code |
| 1 | Repo hygiene | Release engineer | Partial (junk files yes, store-backend decision no) |
| 2 | Merge to main | Release engineer | Yes (fast-forward) |
| 3 | B3 gates | Build/CI + manual DAW | Mixed |
| 4 | JUCE tier decision | Product owner | No |
| 5 | Honest marketing copy | Product owner | No |
| 6 | Tag + release | Release engineer | Yes |

**Ship today?** No — Phase 0 (uncommitted WIP) is the remaining blocker;
Phase 1 is now mostly done (junk files removed, `.playwright-mcp/` gitignored,
`store-backend/` verified as intentional tracked source).
**Ship after Phases 0–5?** Yes — as v1.0.0, at the Launch tier
($149 list / $99 intro) recorded in
[`docs/PRODUCT_POSITIONING.md`](PRODUCT_POSITIONING.md) (if present).
