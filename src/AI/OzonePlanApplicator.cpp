/*
 * More-Phi — AI/OzonePlanApplicator.cpp
 */
#include "OzonePlanApplicator.h"
#include "Plugin/PluginProcessor.h"
#include "Core/AutoMasteringEngine.h"   // AUDIT-FIX (Fix 2): ApplyVerification definition
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <algorithm>

namespace more_phi {

using json = nlohmann::json;

// AUDIT-FIX (Fix 2): base-class default — returns an empty verification so test
// stubs that don't override getLastVerification() still compile and report "no
// verification performed" rather than failing to link.
ApplyVerification OzonePlanApplicatorBase::getLastVerification() const noexcept
{
    return {};
}

ApplyVerification OzonePlanApplicator::getLastVerification() const noexcept
{
    // AUDIT-FIX (Fix 2): readback verification. For each param enqueued in the last
    // apply(), read the hosted plugin's current normalized value back and compare to
    // the requested (post-snap) value. Discrete-aware tolerance mirrors
    // MCPToolHandler::classifyVerification: discrete/boolean params get
    // max(0.5/numSteps, 0.001) so a legitimate step-snap isn't flagged as drift;
    // continuous params use 0.001. Drift vs mismatch is distinguished so callers can
    // tell "snapped to a valid step" (driftedDiscrete, usually fine) from "the write
    // didn't land" (mismatched, a real problem).
    ApplyVerification v;
    v.requested = lastBreakdown_.enqueued + lastBreakdown_.unmapped
                + lastBreakdown_.ambiguous + lastBreakdown_.skipped;
    v.enqueued  = lastBreakdown_.enqueued;
    v.unmapped  = lastBreakdown_.unmapped;
    v.ambiguous = lastBreakdown_.ambiguous;

    auto& bridge = processor_.getParameterBridge();
    for (const auto& p : lastBreakdown_.applied)
    {
        const float actual = bridge.getParameterNormalized(p.index);
        const int numSteps = bridge.getParameterNumSteps(p.index);
        const bool discrete = bridge.isDiscrete(p.index) || bridge.isBoolean(p.index);
        // Discrete-aware tolerance: half a step (so adjacent-step snaps pass), with
        // a 0.001 floor for booleans / numSteps<=1.
        const float tol = discrete
            ? std::max(0.5f / static_cast<float>(std::max(1, numSteps)), 0.001f)
            : 0.001f;
        const float delta = std::abs(actual - p.requestedNormalized);
        if (delta <= tol)
        {
            ++v.verified;
        }
        else if (discrete)
        {
            ++v.driftedDiscrete;
        }
        else
        {
            ++v.mismatched;
        }
    }
    return v;
}

OzonePlanApplicator::OzonePlanApplicator(MorePhiProcessor& processor,
                                         const OzoneParameterMap& map) noexcept
    : processor_(processor), map_(map)
{
}

int OzonePlanApplicator::emitDeferredGestures() noexcept
{
    // F1 FIX (2026-06-30): emit ONE DAW gesture per applied parameter, after the
    // drain. See the header doc for the rationale (audio-thread setValue is
    // gesture-free by design; this is the message-thread companion that groups
    // the batch into undoable automation events).
    //
    // Idempotency: gesturedPlanId_ tracks the plan whose gestures have already
    // been emitted. processPendingReverify may poll multiple times before the
    // next apply; without this guard each poll would re-emit the same N gestures.
    // lastPlanId_ is a plain uint64_t (message-thread-only: apply() writes it,
    // emitDeferredGestures() reads it, both on the message thread), so a direct
    // read is correct — no atomic load needed for the source value.
    const std::uint64_t submitted = lastPlanId_;
    if (submitted == 0)
        return 0;  // no plan was ever applied (apply() only bumps lastPlanId_ on >0 writes)
    if (gesturedPlanId_.load(std::memory_order_acquire) == submitted)
        return 0;  // already gestured this plan

    int gestured = 0;
    auto& bridge = processor_.getParameterBridge();
    for (const auto& p : lastBreakdown_.applied)
    {
        // Each param gets its own begin/perform/end triplet. We do NOT wrap the
        // whole batch in a single gesture (VST3 gestures are per-ParamID, not
        // cross-parameter transactions), but emitting them back-to-back on the
        // message thread right after drain lets the DAW coalesce them into one
        // undo step in most hosts (Reaper, Cubase, Logic all do this for
        // contiguous same-timestamp gestures).
        if (! bridge.beginParameterGesture(p.index))
            continue;  // non-VST3, no host-editing extension, or out of range — skip
        bridge.performParameterGesture(p.index, p.requestedNormalized);
        bridge.endParameterGesture(p.index);
        ++gestured;
    }

    gesturedPlanId_.store(submitted, std::memory_order_release);
    return gestured;
}

int OzonePlanApplicator::apply(const MultiEffectPlan& plan)
{
    if (!plan.valid)
        return 0;

    // AUDIT-FIX-5: fail loud. If no parameter has been audited/mapped yet, log a
    // warning so the silent no-op cannot masquerade as a successful apply. Until
    // audit_ozone_parameters(apply=true) runs against the hosted Ozone instance,
    // every plan field writes nothing — callers deserve to know.
    if (!map_.hasAnyMapping())
    {
        DBG(
            "OzonePlanApplicator: WARNING — parameter map is all-stubs; "
            "no Ozone parameters will be set. Run audit_ozone_parameters(apply=true) "
            "against the hosted plugin to populate the map.");
        lastAppliedCount_ = 0;
        lastBreakdown_ = {};   // AUDIT-FIX (Fix 6)
        return 0;
    }

    // AUDIT-FIX (Fix 6): reset the breakdown so it always reflects THIS call.
    lastBreakdown_ = OzoneApplyBreakdown{};

    // M2 FIX (2026-06-30): plugin-swap / positional-reorder guard. The map's
    // indices are positional and were captured against a specific parameter
    // layout (fingerprintHost at build time). nameMatches() per-write catches a
    // name change at a given index, but cannot detect a reorder where the
    // swapped plugin reuses the same name at a different index — the per-write
    // check would pass and the stale index would corrupt an unrelated control.
    // Re-fingerprint the live host here; if it differs from the stored one, the
    // whole apply is refused (every slot counted as skipped) because NO
    // positional index is trustworthy until the map is rebuilt. Skipped for the
    // stub map (empty fingerprint — buildForOzone11 never probed a live host).
    if (map_.hostFingerprint.isNotEmpty())
    {
        const juce::String liveFp = OzoneParameterMap::fingerprintHost(
            processor_.getParameterBridge());
        if (liveFp != map_.hostFingerprint)
        {
            DBG("OzonePlanApplicator: host fingerprint changed (map built for "
                + map_.hostFingerprint.substring(0, 12) + ", live is "
                + liveFp.substring(0, 12) + ") — plugin swapped or parameters "
                  "reordered. Refusing apply to avoid writing stale positional "
                  "indices. Re-host and rebuild the map.");
            lastBreakdown_.skipped = 1;   // signal the apply produced nothing usable
            lastAppliedCount_ = 0;
            return 0;
        }
    }

    int total = 0;
    total += applyEQ(plan);
    total += applyDynamics(plan);
    total += applyStereoImager(plan);
    total += applyMaximizer(plan);
    total += applyExciter(plan);   // F4 FIX: harmonic/saturation side-channel

    // P3.10 (AUDIT): close the plan with a transaction boundary so the audio
    // thread can confirm the full plan drained (getLastDrainedPlanId). Only emit
    // when at least one parameter was enqueued — a zero-param apply has nothing to
    // commit. The boundary is a no-op command (paramIndex=-1) and does not count
    // toward the returned total.
    //
    // F11 FIX (2026-06-30): only emit the boundary for a FULLY-enqueued plan
    // (no skips, no unmapped slots). The boundary is the atomicity signal callers
    // correlate against getLastDrainedPlanId(); a partial apply (queue full,
    // index drift, name mismatch) must NOT claim drain-completion atomicity, or
    // a downstream reverify would treat a half-applied plan as fully committed.
    // Partial applies still return their count for telemetry; they simply do not
    // advance lastPlanId_, so processPendingReverify never fires for them.
    const bool fullyEnqueued = (lastBreakdown_.skipped == 0 && lastBreakdown_.unmapped == 0
                                && lastBreakdown_.ambiguous == 0);
    if (total > 0 && fullyEnqueued)
    {
        const std::uint64_t planId = ++lastPlanId_;
        processor_.enqueuePlanBoundary(planId, MorePhiProcessor::ParameterEditSource::MCP);
    }

    lastAppliedCount_ = total;
    return total;
}

// ── EQ ───────────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyEQ(const MultiEffectPlan& plan)
{
    // RT-AUDIT (2026-06-30): THREADING INVARIANT — applyEQ performs json::parse
    // (heavy heap: lexer + DOM node per band/freq/gain/Q), juce::String concats,
    // and vector push_back via enqueueIfMapped. It MUST NEVER run on the audio
    // thread. Reachable only from OzonePlanApplicator::apply, which is called
    // only by ChainPlanExecutor::applyPlan on the message thread (timer-driven
    // processPendingApplication) or the MCP server connection thread — both
    // off-audio. The pending-plan atomic-flag pattern in SonicMasterAnalysisEngine
    // (analysis thread sets flag → message-thread timer applies) is what prevents
    // any audio-thread reachability; do not bypass it.
    if (plan.eqPrescriptionJSON.isEmpty())
        return 0;

    int count = 0;
    try
    {
        const auto j = json::parse(plan.eqPrescriptionJSON.toStdString());
        if (!j.contains("bands") || !j["bands"].is_array())
            return 0;

        const auto& bands = j["bands"];
        const int numBands = std::min(static_cast<int>(bands.size()),
                                      OzoneParameterMap::kEQBands);

        for (int i = 0; i < numBands; ++i)
        {
            const auto& b   = bands[i];
            const auto& bm  = map_.eq[i];

            const float freq    = b.value("freq", 1000.0f);
            const float gain    = b.value("gain",    0.0f);
            const float q       = b.value("Q",       1.0f);
            const auto  typeStr = juce::String(b.value("type", "peak"));

            count += enqueueIfMapped(bm.freqIdx,
                OzoneParameterMap::normalizeFreq(freq),
                "eq band " + juce::String(i + 1) + " frequency");
            count += enqueueIfMapped(bm.gainIdx,
                // P2.7 (AUDIT): pass the SAME ±AdaptiveEQ::kMaxGainDB range the
                // decode path uses (AutoMasteringEngine.cpp:469-472 and
                // buildBridgePlanFromNeural :640-643 decode eq[i] *
                // AdaptiveEQ::kMaxGainDB). normalizeGain() defaults to ±18 dB;
                // with kMaxGainDB == 12, a decoded +12 dB gain re-encoded over
                // ±18 dB arrived at the plugin at ~0.833 instead of 1.0 — a 1.5x
                // systematic under-gain across all 8 EQ bands. Centralizing on the
                // decode constant makes the round-trip identity.
                OzoneParameterMap::normalizeGain(gain,
                    -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB),
                "eq band " + juce::String(i + 1) + " gain");
            count += enqueueIfMapped(bm.qIdx,
                // Q is linear in VST3 normalized space over [0.1, 8.0] — see
                // OzoneParameterMap::normalizeQ. Do NOT route through normalizeFreq
                // (log2); that distorted Q and used a range ([0.1, 20]) that
                // disagreed with PluginSemanticMapper's documented Q domain.
                OzoneParameterMap::normalizeQ(q),
                "eq band " + juce::String(i + 1) + " q");
            count += enqueueIfMapped(bm.typeIdx,
                OzoneParameterMap::encodeFilterType(typeStr),
                "eq band " + juce::String(i + 1) + " type");
            count += enqueueIfMapped(bm.enabledIdx, 1.0f,  // enable band
                "eq band " + juce::String(i + 1) + " enabled");
        }
    }
    catch (const std::exception& e)
    {
        // F12 FIX (2026-06-30): a malformed eqPrescriptionJSON previously caused a
        // silent partial apply — dynamics/imager/maximizer still ran from the same
        // plan, but EQ was skipped with no record. Now: log loudly and account for
        // every EQ slot that would have been written (up to kEQBands bands x 5
        // params = freq/gain/Q/type/enabled) as unmapped in the breakdown, so
        // getLastOzoneApplyBreakdown reflects that EQ was dropped and a downstream
        // reverify / partial classifier sees it. Per the C-3 logging convention the
        // DBG is message-thread-only (apply() runs on the message thread).
        DBG("OzonePlanApplicator::applyEQ: malformed eqPrescriptionJSON ("
            + juce::String(e.what()) + ") — skipping EQ, "
            + juce::String(OzoneParameterMap::kEQBands * 5)
            + " slots counted as unmapped.");
        lastBreakdown_.unmapped += OzoneParameterMap::kEQBands * 5;
    }
    return count;
}

// ── Dynamics ─────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyDynamics(const MultiEffectPlan& plan)
{
    // AUDIT-FIX (L4-1, 2026-06-29): per-band compression path. When the hosted
    // plugin exposes per-band compressor params AND the neural model produced
    // per-band data, route each of the 3 bands × 6 params directly instead of
    // collapsing to a single scalar. Falls back to the global-envelope scalar
    // path when per-band mapping is absent or the model didn't produce compParams.
    if (plan.hasCompBandParams && map_.hasPerBandCompMapping())
        return applyDynamicsPerBand(plan);

    const auto& dm = map_.dynamics;
    int count = 0;

    // compressionNeed [0..1] → threshold dBFS (lerp from gentle to aggressive)
    const float threshDB = kThresholdAtMinNeed
        + plan.compressionNeed * (kThresholdAtMaxNeed - kThresholdAtMinNeed);
    count += enqueueIfMapped(dm.thresholdIdx,
        OzoneParameterMap::normalizeThreshold(threshDB),
        "dynamics threshold");

    // compressionNeed [0..1] → ratio
    const float ratio = kRatioAtMinNeed
        + plan.compressionNeed * (kRatioAtMaxNeed - kRatioAtMinNeed);
    const float ratioNorm = std::clamp(
        (ratio - kOzoneRatioMin) / (kOzoneRatioMax - kOzoneRatioMin), 0.0f, 1.0f);
    count += enqueueIfMapped(dm.ratioIdx, ratioNorm, "dynamics ratio");

    // Fixed attack/release defaults (good mastering starting points)
    const float attackNorm = std::clamp(
        (kDefaultAttackMs - kOzoneAttackMin) / (kOzoneAttackMax - kOzoneAttackMin),
        0.0f, 1.0f);
    const float releaseNorm = std::clamp(
        (kDefaultReleaseMs - kOzoneReleaseMin) / (kOzoneReleaseMax - kOzoneReleaseMin),
        0.0f, 1.0f);
    count += enqueueIfMapped(dm.attackIdx,  attackNorm,  "dynamics attack");
    count += enqueueIfMapped(dm.releaseIdx, releaseNorm, "dynamics release");

    return count;
}

int OzonePlanApplicator::applyDynamicsPerBand(const MultiEffectPlan& plan)
{
    int count = 0;
    static constexpr const char* kBandNames[3] = {
        "comp band 1", "comp band 2", "comp band 3"
    };

    // Enable the per-band compressor module if an enable toggle exists.
    count += enqueueIfMapped(map_.compEnableIdx, 1.0f, "comp enable");

    for (int b = 0; b < static_cast<int>(MultiEffectPlan::kCompBandCount); ++b)
    {
        const auto& cb = map_.compBands[static_cast<std::size_t>(b)];
        const auto& bp = plan.compBandParams[static_cast<std::size_t>(b)];
        const auto prefix = kBandNames[b];

        // Threshold dBFS → normalized [0,1] over [-60, 0]
        count += enqueueIfMapped(cb.thresholdIdx,
            OzoneParameterMap::normalizeThreshold(bp.thresholdDb),
            juce::String(prefix) + " threshold");

        // Ratio → normalized over [1, 20]
        const float ratioNorm = std::clamp(
            (bp.ratio - kOzoneRatioMin) / (kOzoneRatioMax - kOzoneRatioMin), 0.0f, 1.0f);
        count += enqueueIfMapped(cb.ratioIdx, ratioNorm,
            juce::String(prefix) + " ratio");

        // Attack ms → normalized over [0.1, 100]
        const float attackNorm = std::clamp(
            (bp.attackMs - kOzoneAttackMin) / (kOzoneAttackMax - kOzoneAttackMin),
            0.0f, 1.0f);
        count += enqueueIfMapped(cb.attackIdx, attackNorm,
            juce::String(prefix) + " attack");

        // Release ms → normalized over [10, 1000]
        const float releaseNorm = std::clamp(
            (bp.releaseMs - kOzoneReleaseMin) / (kOzoneReleaseMax - kOzoneReleaseMin),
            0.0f, 1.0f);
        count += enqueueIfMapped(cb.releaseIdx, releaseNorm,
            juce::String(prefix) + " release");

        // Makeup gain dB → normalized over [0, 24] (Ozone range for makeup)
        if (cb.makeupIdx >= 0)
        {
            const float makeupNorm = std::clamp(bp.makeupDb / 24.0f, 0.0f, 1.0f);
            count += enqueueIfMapped(cb.makeupIdx, makeupNorm,
                juce::String(prefix) + " makeup");
        }

        // Knee dB → normalized over [0, 24] (Ozone range for knee)
        if (cb.kneeIdx >= 0)
        {
            const float kneeNorm = std::clamp(bp.kneeDb / 24.0f, 0.0f, 1.0f);
            count += enqueueIfMapped(cb.kneeIdx, kneeNorm,
                juce::String(prefix) + " knee");
        }
    }

    return count;
}

// ── Stereo Imager ─────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyStereoImager(const MultiEffectPlan& plan)
{
    int count = 0;
    // P2.6: expected names mirror the buildFromHostedPlugin band-order tokens.
    static constexpr const char* kImagerBandNames[4] = {
        "imager sub width", "imager low width",
        "imager mid width", "imager high width"
    };
    for (int i = 0; i < 4; ++i)
    {
        count += enqueueIfMapped(map_.imager.widthIdx[i],
            OzoneParameterMap::normalizeWidth(plan.widthCurve[i]),
            kImagerBandNames[i]);
    }
    return count;
}

// ── Maximizer ────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyMaximizer(const MultiEffectPlan& plan)
{
    int count = 0;
    count += enqueueIfMapped(map_.maximizer.outputLevelIdx,
        OzoneParameterMap::normalizeLUFS(plan.targetLUFS),
        "maximizer output level");
    count += enqueueIfMapped(map_.maximizer.ceilingIdx,
        OzoneParameterMap::normalizeCeiling(plan.ceilingDBTP),
        "maximizer ceiling");
    return count;
}

// ── Exciter / Saturation (F4 FIX) ────────────────────────────────────────────

int OzonePlanApplicator::applyExciter(const MultiEffectPlan& plan)
{
    // F4 FIX (2026-06-30): forward the decoded saturation side-channel to the
    // hosted plugin's Exciter module. exciterEnabled is set by
    // buildBridgePlanFromNeural when the SonicMaster decoder raised applyHarmonic
    // and the gate amount is meaningful. drive/mix are pre-clamped [0,1] in the
    // engine; the bridge's enqueue path clamps again on [0,1], so a stray value
    // cannot reach the hosted plugin out-of-range. When the map has no exciter
    // slots mapped, every enqueue counts as unmapped (graceful) and the plan
    // proceeds — the internal exciter (when intelligence is active) still got the
    // values via AutoMasteringEngine's direct exciter_.setDrive/setDryWet calls.
    if (!plan.exciterEnabled)
        return 0;

    int count = 0;
    // Enable the module first (a toggle is idempotent and cheap).
    count += enqueueIfMapped(map_.exciter.enableIdx, 1.0f, "exciter enable");
    // Drive: the model emits [0,1]; Ozone's Exciter Drive is normalized [0,1]
    // (0 = none, 1 = max). Pass through directly — clamp happens at enqueue.
    count += enqueueIfMapped(map_.exciter.driveIdx,
        std::clamp(plan.exciterDrive, 0.0f, 1.0f),
        "exciter drive");
    // Mix: [0,1] maps to [0,1] normalized. The engine already scales mix by the
    // gate amount for the internal exciter; for the hosted plugin we forward the
    // raw decoded mix so the user's wet/dry intent is preserved.
    count += enqueueIfMapped(map_.exciter.mixIdx,
        std::clamp(plan.exciterMix, 0.0f, 1.0f),
        "exciter mix");
    return count;
}

// ── Private helpers ───────────────────────────────────────────────────────────

int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue)
{
    return enqueueIfMapped(idx, normalizedValue, /*expectedName=*/{});
}

int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue,
                                         const juce::String& expectedName)
{
    // AUDIT-FIX (Fix 6): classify the skip reason so applyValidatedPlan can tell
    // partial-apply from all-unmapped. idx == -2 (Fix 3, ambiguity) is counted as
    // ambiguous; idx == -1 is unmapped. Both still return 0 to preserve the int API.
    if (idx == -2)
    {
        ++lastBreakdown_.ambiguous;
        return 0;
    }
    if (idx < 0)
    {
        ++lastBreakdown_.unmapped;
        return 0;
    }
    // P2.6 (AUDIT): index-drift re-validation. The OzoneParameterMap is built once
    // against a hosted plugin's positional indices. If the user swaps plugins, the
    // stale index (e.g. EQ Band 1 Freq = 42) writes to whatever parameter sits at
    // position 42 in the NEW plugin — silently corrupting an unrelated control.
    // When the caller supplies the name the slot was mapped from, re-read the
    // hosted plugin's current parameter name at that index and bail (counted as
    // skipped) on mismatch. The MCP setParameter path is immune (it re-resolves
    // names live each call); this brings the neural path to the same safety level
    // without rebuilding the whole map on every write.
    if (expectedName.isNotEmpty())
    {
        const juce::String actualName = processor_.getParameterBridge().getParameterName(idx);
        if (! nameMatches(expectedName, actualName))
        {
            ++lastBreakdown_.skipped;
            DBG("OzonePlanApplicator: index drift — expected '"
                + expectedName + "' at index " + juce::String(idx)
                + " but hosted plugin reports '" + actualName
                + "'; skipping to avoid writing the wrong parameter. "
                  "Re-hosting the plugin or rebuilding the map is required.");
            return 0;
        }
    }
    // AUDIT-FIX (Fix 5): snap discrete/binary params to the nearest valid step
    // before enqueue. The raw enqueue path writes a continuous float regardless of
    // isDiscrete/isBoolean and relies on the hosted plugin to self-snap — which is
    // unreliable (e.g. enabledIdx ← 1.0f, typeIdx ← 0.25). Continuous params pass
    // through unchanged (snapNormalizedToStep is identity for non-discrete).
    const float snapped = processor_.getParameterBridge().snapNormalizedToStep(idx, normalizedValue);
    const bool ok = processor_.enqueueParameterSet(
        idx, snapped,
        // AUDIT (E2, 2026-06-25): tag neural-plan writes with a distinct source
        // so ParameterBridge's per-parameter stamp can distinguish an automated
        // neural plan write from a manual MCP edit (and observe write-precedence
        // conflicts on the same hosted control). Previously both AI entry paths
        // passed ::MCP, making them indistinguishable.
        MorePhiProcessor::ParameterEditSource::Neural,
        // P2.4 (AUDIT): hold the AI edit against morph. The neural/Ozone path
        // previously passed false, so a running morph block could overwrite the
        // recommended value before/while it drained — the edit vanished audibly.
        // The MCP setParameter path already holds (MCPToolHandler.cpp:3344) for
        // exactly this reason; the two AI entry paths now share the invariant.
        // Drain-delay risk under commandConsumerLock_ is bounded: the value is
        // retained in the ring buffer and the hold prevents morph clobbering it
        // while it waits.
        /*holdAgainstMorph=*/true);
    if (ok)
    {
        ++lastBreakdown_.enqueued;
        // AUDIT-FIX (Fix 2): record the (index, requested) pair for readback.
        // 'requested' is the post-snap value actually pushed to the queue.
        lastBreakdown_.applied.push_back({ idx, snapped });
        return 1;
    }
    ++lastBreakdown_.skipped;
    return 0;
}

// P2.6 (AUDIT): case-insensitive, trimming comparator for the index-drift check.
// The map stores names as built by buildFromHostedPlugin (lowercased substrings of
// the hosted plugin's own names), so a direct == would be too strict; a substring
// check in both directions tolerates the vendor's suffix/prefix variations while
// still catching a genuine mismatch (e.g. "EQ Band 1 Frequency" vs "Compressor Mix").
//
// AUDIT-FIX (F2.1, 2026-06-27): the prior bidirectional substring comparator
// (a.contains(e) || e.contains(a)) was too permissive — a parameter literally
// named "gain" would match an expected "eq band 1 gain" (because the expected
// string contains "gain"), and an expected "compressor threshold" could match a
// param named "threshold" alone even when the index pointed at a different
// module's threshold. The tokenized comparator below splits both names on
// non-alphanumeric boundaries and matches iff every token of the SHORTER string
// is present in the LONGER string's token set. This preserves vendor-suffix
// tolerance ("eq band 1 frequency" vs "EQ Band 1 Frequency (Hz)") and the
// map-key-as-single-token case ("gain" vs "eq band 1 gain"), while rejecting
// genuine module mismatches and band-swap confusion ("band 1" vs "band 2").
bool OzonePlanApplicator::nameMatches(const juce::String& expectedName,
                                      const juce::String& actualName) noexcept
{
    if (expectedName.isEmpty() || actualName.isEmpty())
        return false;
    // Split on any non-alphanumeric run (space, punctuation, "(Hz)", etc.).
    auto tokenize = [](const juce::String& s, std::array<juce::String, 16>& out)
    {
        std::size_t n = 0;
        juce::String tok;
        for (auto c : s)
        {
            if (juce::CharacterFunctions::isLetterOrDigit(c))
                tok += juce::CharacterFunctions::toLowerCase(c);
            else if (tok.isNotEmpty())
            {
                if (n < out.size()) out[n++] = tok;
                tok.clear();
            }
        }
        if (tok.isNotEmpty() && n < out.size()) out[n++] = tok;
        return n;
    };
    std::array<juce::String, 16> eTok {}, aTok {};
    const std::size_t eN = tokenize(expectedName.trim(), eTok);
    const std::size_t aN = tokenize(actualName.trim(), aTok);
    if (eN == 0 || aN == 0)
        return false;
    // The shorter token set must be a subset of the longer one. This preserves
    // both directions: expected "gain" ⊆ actual "eq band 1 gain", and expected
    // "eq band 1 frequency" ⊆ actual "eq band 1 frequency hz".
    const bool eShorter = eN <= aN;
    const auto& needle  = eShorter ? eTok : aTok;
    const std::size_t needleN = eShorter ? eN : aN;
    const auto& haystack = eShorter ? aTok : eTok;
    const std::size_t hayN = eShorter ? aN : eN;
    for (std::size_t i = 0; i < needleN; ++i)
    {
        bool found = false;
        for (std::size_t j = 0; j < hayN; ++j)
            if (needle[i] == haystack[j]) { found = true; break; }
        if (!found)
            return false;
    }
    return true;
}

} // namespace more_phi
