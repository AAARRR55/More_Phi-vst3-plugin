# VST3 License Key Management System

**Target:** More-Phi v3.4.1 / JUCE 8 / C++20 VST3 and AU plugin releases  
**Purpose:** Practical implementation guide for trial, perpetual, subscription, node-locked, and floating licensing in a desktop audio plugin.  
**Security posture:** Defense-in-depth. The goal is to deter casual misuse and large-scale redistribution while keeping the plugin stable, real-time safe, privacy-aware, and user-friendly.

---

## 1. Executive Summary

Audio plugin licensing has three special constraints that differ from ordinary desktop apps:

1. **The DAW controls lifecycle.** The plugin may be instantiated many times, scanned headlessly, sandboxed, or loaded without network access.
2. **The audio thread must never block.** License validation cannot perform disk I/O, network I/O, allocation-heavy parsing, or cryptography inside `processBlock()`.
3. **Users expect offline reliability.** Musicians often work on studio machines without internet, while vendors still need periodic validation and revocation support.

Recommended architecture:

- Use **signed license tokens** rather than secret-bearing keys.
- Use **Ed25519** or **ECDSA P-256** public-key signatures for license authenticity.
- Keep the **private signing key only on the licensing server**.
- Embed only the **public verification key** in the plugin.
- Store a signed local **activation certificate** for offline operation.
- Revalidate online periodically on a background/message thread.
- Expose audio-thread-safe license status through atomics or immutable snapshots.

---

## 2. System Goals and Non-Goals

### 2.1 Goals

- License generation compatible with VST3/JUCE plugin distribution.
- Support these license types:
  - Trial
  - Perpetual
  - Subscription
  - Node-locked activation
  - Floating/team lease
- Work offline with controlled grace periods.
- Avoid blocking or unsafe work in real-time audio paths.
- Keep license storage secure but recoverable.
- Provide clear activation, error, deactivation, and transfer flows.
- Collect minimal personal data and support privacy obligations.

### 2.2 Non-Goals

- No licensing design can make a native plugin impossible to crack.
- This document does not include offensive reverse-engineering instructions.
- License checks must not intentionally damage user sessions, projects, presets, or audio output.
- The plugin must not phone home from the audio thread.

---

## 3. High-Level Architecture

```text
┌────────────────────────────────────────────────────────────────────┐
│                         Licensing Server                           │
│                                                                    │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────────┐   │
│  │ License DB   │   │ Activation   │   │ Signing Service       │   │
│  │ Customers    │──▶│ Policy API   │──▶│ Private Ed25519 Key   │   │
│  │ Seats/Leases │   │              │   │ Never shipped         │   │
│  └──────────────┘   └──────────────┘   └──────────────────────┘   │
└───────────────────────────────────┬────────────────────────────────┘
                                    │ HTTPS/TLS
                                    ▼
┌────────────────────────────────────────────────────────────────────┐
│                           VST3/AU Plugin                            │
│                                                                    │
│  Message/UI Thread                                                  │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────────┐    │
│  │ Activation UI│──▶│ LicenseManager│──▶│ SecureLicenseStore   │    │
│  └──────────────┘   │ Background I/O│   │ AppData/Keychain     │    │
│                     └──────┬───────┘   └──────────────────────┘    │
│                            │                                        │
│                            ▼ atomic snapshot                        │
│  Audio Thread        ┌────────────────┐                             │
│  processBlock() ───▶ │ LicenseStatus  │  no I/O, no locks, no net    │
│                      └────────────────┘                             │
└────────────────────────────────────────────────────────────────────┘
```

### 3.1 Components

| Component | Location | Responsibility | Real-time Safe? |
|---|---|---|---|
| `LicenseManager` | Plugin/message thread | Parses, validates signatures, schedules online checks | No |
| `LicenseRuntimeState` | Shared immutable/atomic state | Exposes licensed/trial/expired flags to UI/audio | Yes |
| `SecureLicenseStore` | Non-audio thread | Reads/writes activation certificate | No |
| `ActivationClient` | Background thread | HTTPS activation, refresh, deactivation | No |
| `ActivationPanel` | UI thread | User key entry, status, messages | No |
| Licensing server | Vendor backend | Issues licenses, validates activations, signs certificates | N/A |

---

## 4. License Types and Policy Matrix

| License Type | Use Case | Online Requirement | Local Token Behavior | Expiry Handling |
|---|---|---|---|---|
| Trial | Evaluation | First activation or first run | Signed trial certificate | Expire after `trialEndsAt` |
| Perpetual | One-time purchase | Activation + periodic validation | No product expiry, validation window expires | Grace, then feature restriction |
| Subscription | Monthly/yearly | Activation + regular refresh | `subscriptionEndsAt` and `nextCheckAt` | Grace after failed refresh |
| Node-locked | Single/few computers | Activation binds to device hash | `machineHash` in signed certificate | Reject if hardware mismatch |
| Floating | Teams/studios | Lease checkout and heartbeat/renewal | Short-lived signed lease | Return to read-only/demo when lease expires |

### 4.1 Recommended Defaults

```yaml
trial_days: 14
max_node_activations_per_license: 2
perpetual_online_check_interval_days: 30
subscription_online_check_interval_days: 7
offline_grace_days_after_failed_check: 14
floating_lease_duration_hours: 12
floating_renewal_interval_minutes: 30
clock_skew_tolerance_minutes: 10
```

---

## 5. License Key Format

### 5.1 Human-Readable License Key

Use a short purchase key for user entry, not as the full source of truth.

```text
MPH1-XXXX-XXXX-XXXX-XXXX-XXXX-C
```

Where:

- `MPH1` = product/version prefix.
- `XXXX...` = Base32 Crockford encoded random 128-bit identifier.
- `C` = short checksum for typo detection.

Example:

```text
MPH1-9K4M-Q7XN-2R8D-HW6T-3PZA-F
```

This key identifies a license record on the server. It does **not** contain secret material and should not be trusted by itself.

### 5.2 Checksum Algorithm

Use checksum only for typo detection before sending to the server.

```cpp
uint8_t licenseChecksum(std::string_view normalizedKeyWithoutChecksum)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : normalizedKeyWithoutChecksum)
    {
        crc ^= c;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
    return static_cast<uint8_t>((crc ^ 0xFFFFFFFFu) % 32u);
}
```

### 5.3 Activation Certificate

After activation, the server returns a signed certificate stored locally.

```json
{
  "schema": "morephi-license-cert-v1",
  "licenseId": "lic_01HZX...",
  "activationId": "act_01HZY...",
  "productId": "more-phi-vst3",
  "licenseType": "perpetual",
  "features": ["morph_pad", "plugin_hosting", "mcp", "premium_presets"],
  "issuedAt": "2026-03-08T12:00:00Z",
  "validFrom": "2026-03-08T12:00:00Z",
  "validUntil": null,
  "trialEndsAt": null,
  "subscriptionEndsAt": null,
  "nextOnlineCheckAt": "2026-04-07T12:00:00Z",
  "offlineGraceEndsAt": "2026-04-21T12:00:00Z",
  "machineHash": "b64url_sha256_hash",
  "customerRegion": "EU",
  "nonce": "b64url_128bit_random"
}
```

The server signs the canonical JSON payload:

```json
{
  "payload": "base64url(canonical-json)",
  "signature": "base64url(ed25519_signature)",
  "keyId": "prod-ed25519-2026-01"
}
```

---

## 6. Cryptographic Design

### 6.1 Recommended Algorithms

| Purpose | Recommended | Notes |
|---|---|---|
| License authenticity | Ed25519 signature | Fast, compact, simple verification |
| Transport | TLS 1.2+ / TLS 1.3 | Certificate validation required |
| Stored token integrity | Ed25519 signature + local HMAC wrapper | Signature remains authoritative |
| Local encryption | OS keychain where possible; otherwise AES-256-GCM | Do not rely on obscurity alone |
| Machine fingerprint | SHA-256/HMAC-SHA-256 of stable identifiers | Store only hashed/salted value |
| Password/API secret storage | Server side only | Never ship signing keys in plugin |

### 6.2 Why Public-Key Signatures

Symmetric schemes require a secret inside the plugin. Native plugins can be inspected, patched, and debugged. With public-key signatures:

- The plugin verifies licenses using a public key.
- Attackers cannot generate valid new certificates without the private key.
- Key rotation can be handled by `keyId` and multiple embedded public keys.

### 6.3 Canonical Payload

Before signing, serialize fields in deterministic order and use strict UTF-8. Avoid signing pretty-printed JSON directly unless your canonicalization is stable.

```text
signature = Ed25519.Sign(privateKey, canonicalJson(payload))
valid = Ed25519.Verify(publicKeyFor(keyId), canonicalJson(payload), signature)
```

---

## 7. Machine Binding / Node Locking

### 7.1 Privacy-Preserving Device Fingerprint

Build a fingerprint from stable identifiers, then hash locally before sending.

Potential inputs:

- OS installation ID where allowed.
- Primary volume UUID.
- CPU architecture and OS family.
- MAC address should be avoided unless explicitly justified because it is more privacy-sensitive and less stable in virtualized/networked environments.

Recommended approach:

```text
rawFingerprint = productId + osFamily + normalizedHardwareIds
machineHash = HMAC-SHA-256(vendorPepper, normalize(rawFingerprint))
```

The plugin sends only `machineHash`, never raw hardware IDs.

### 7.2 Hardware Change Tolerance

Do not fail hard on small changes. Use a server-side confidence score:

```text
sameMachine if score >= 0.75
possibleMachine if 0.50 <= score < 0.75, allow with refresh
newMachine if score < 0.50, consumes activation seat
```

---

## 8. Secure Storage

### 8.1 Storage Locations

| Platform | Recommended Location | Notes |
|---|---|---|
| macOS | Keychain for secret wrapper key; certificate in `~/Library/Application Support/MorePhi/` | Works across VST3/AU instances |
| Windows | Credential Manager or DPAPI; certificate in `%APPDATA%\MorePhi\` | Prefer per-user storage |
| Linux | Secret Service/libsecret when available; fallback `~/.config/morephi/` | Linux plugin support varies by DAW |

### 8.2 JUCE-Friendly Storage Pattern

For a JUCE plugin, store certificate data outside DAW project state. Do not save user license keys inside `getStateInformation()`.

```cpp
juce::File getLicenseDirectory()
{
    auto dir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory)
        .getChildFile("MorePhi")
        .getChildFile("Licensing");
    dir.createDirectory();
    return dir;
}
```

### 8.3 What to Store

Store:

- Signed activation certificate.
- Last successful online validation timestamp.
- Last validation error code for UI diagnostics.
- Local encrypted cache metadata.

Do not store:

- Plain-text full customer PII.
- Private signing keys.
- Raw machine identifiers.
- User password or payment details.

---

## 9. Activation Workflow

### 9.1 Online Activation

```text
User enters license key
        │
        ▼
Plugin normalizes + checksum-validates key
        │
        ▼
Plugin sends activation request over HTTPS from background thread
        │
        ▼
Server validates license, seat count, subscription status
        │
        ▼
Server signs activation certificate
        │
        ▼
Plugin verifies server signature locally before storing
        │
        ▼
Plugin updates LicenseRuntimeState atomically
```

### 9.2 Activation API Contract

Request:

```http
POST /v1/activations
Content-Type: application/json
```

```json
{
  "licenseKey": "MPH1-9K4M-Q7XN-2R8D-HW6T-3PZA-F",
  "productId": "more-phi-vst3",
  "pluginVersion": "3.4.1",
  "machineHash": "b64url_sha256_hash",
  "os": "macos-14-arm64",
  "dawHint": "Ableton Live",
  "requestNonce": "b64url_128bit_random"
}
```

Response:

```json
{
  "status": "activated",
  "certificate": {
    "payload": "base64url(canonical-json)",
    "signature": "base64url(signature)",
    "keyId": "prod-ed25519-2026-01"
  }
}
```

### 9.3 Offline Activation

Offline activation is useful for studio machines.

```text
Plugin displays offline request code
        │
        ▼
User copies request code to web portal on another device
        │
        ▼
Server returns offline response file/text
        │
        ▼
User imports response in plugin
        │
        ▼
Plugin verifies signed certificate and activates
```

Offline request payload:

```json
{
  "schema": "morephi-offline-request-v1",
  "productId": "more-phi-vst3",
  "pluginVersion": "3.4.1",
  "machineHash": "b64url_sha256_hash",
  "requestNonce": "b64url_128bit_random",
  "createdAt": "2026-03-08T12:00:00Z"
}
```

---

## 10. Validation Logic

### 10.1 Validation States

```cpp
enum class LicenseState : uint8_t
{
    Unknown,
    TrialActive,
    Licensed,
    GracePeriod,
    Expired,
    Invalid,
    ActivationRequired,
    FloatingLeaseUnavailable
};

struct LicenseRuntimeState
{
    std::atomic<LicenseState> state { LicenseState::Unknown };
    std::atomic<bool> premiumFeaturesEnabled { false };
    std::atomic<int64_t> graceEndsUnixSeconds { 0 };
    std::atomic<int64_t> nextCheckUnixSeconds { 0 };
};
```

### 10.2 Plugin Initialization Flow

```text
PluginProcessor constructor
        │
        ├─ create LicenseRuntimeState with safe defaults
        │
        ▼
prepareToPlay()
        │
        ├─ audio remains safe even if license unresolved
        │
        ▼
PluginEditor / message thread
        │
        ├─ LicenseManager.loadFromDiskAsync()
        ├─ verify certificate signature
        ├─ verify product, expiry, machine hash, feature set
        ├─ publish LicenseRuntimeState atomically
        └─ schedule online validation if due
```

### 10.3 Core Validation Pseudocode

```cpp
ValidationResult LicenseManager::validateCertificate(const SignedCertificate& cert,
                                                     const MachineHash& machine,
                                                     TimePoint now)
{
    auto payloadBytes = base64UrlDecode(cert.payload);

    if (!publicKeyRing_.contains(cert.keyId))
        return { LicenseState::Invalid, "Unknown license signing key." };

    if (!ed25519Verify(publicKeyRing_[cert.keyId], payloadBytes, cert.signature))
        return { LicenseState::Invalid, "License file has been modified." };

    auto payload = parseCanonicalJson(payloadBytes);

    if (payload.productId != PRODUCT_ID)
        return { LicenseState::Invalid, "License is for a different product." };

    if (payload.validFrom > now + CLOCK_SKEW_TOLERANCE)
        return { LicenseState::Invalid, "System clock appears incorrect." };

    if (payload.machineHash != machine.value)
        return { LicenseState::ActivationRequired, "License is activated on another computer." };

    if (payload.licenseType == "trial")
    {
        if (now <= payload.trialEndsAt)
            return { LicenseState::TrialActive, "Trial active." };
        return { LicenseState::Expired, "Trial has expired." };
    }

    if (payload.licenseType == "subscription")
    {
        if (now <= payload.subscriptionEndsAt)
            return { LicenseState::Licensed, "Subscription active." };
        if (now <= payload.offlineGraceEndsAt)
            return { LicenseState::GracePeriod, "Subscription validation grace period." };
        return { LicenseState::Expired, "Subscription expired." };
    }

    if (payload.licenseType == "floating")
    {
        if (now <= payload.leaseEndsAt)
            return { LicenseState::Licensed, "Floating lease active." };
        return { LicenseState::FloatingLeaseUnavailable, "Floating license lease expired." };
    }

    if (payload.licenseType == "perpetual")
    {
        if (payload.validUntil && now > payload.validUntil.value())
            return { LicenseState::Expired, "License expired." };

        if (now <= payload.nextOnlineCheckAt)
            return { LicenseState::Licensed, "License active." };

        if (now <= payload.offlineGraceEndsAt)
            return { LicenseState::GracePeriod, "Offline validation grace period." };

        return { LicenseState::ActivationRequired, "Online validation required." };
    }

    return { LicenseState::Invalid, "Unknown license type." };
}
```

### 10.4 Audio Thread Rule

Audio thread checks should be limited to atomic reads.

```cpp
void MorePhiProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                    juce::MidiBuffer& midi)
{
    const auto licenseState = licenseRuntime_.state.load(std::memory_order_relaxed);
    const bool enabled = licenseRuntime_.premiumFeaturesEnabled.load(std::memory_order_relaxed);

    if (!enabled)
    {
        // Safe fallback: disable premium-only processing or use demo mode.
        // Do not allocate, show UI, perform I/O, or contact network here.
    }

    // Continue normal real-time-safe processing.
}
```

---

## 11. VST3/JUCE Integration Points

### 11.1 Recommended File Structure

```text
src/
├── Licensing/
│   ├── LicenseTypes.h
│   ├── LicenseManager.h/.cpp
│   ├── LicenseRuntimeState.h
│   ├── LicenseVerifier.h/.cpp
│   ├── SecureLicenseStore.h/.cpp
│   ├── ActivationClient.h/.cpp
│   └── MachineFingerprint.h/.cpp
└── UI/
    └── ActivationPanel.h/.cpp
```

### 11.2 Processor Ownership Pattern

```cpp
class MorePhiProcessor final : public juce::AudioProcessor
{
public:
    MorePhiProcessor();
    ~MorePhiProcessor() override;

    LicenseRuntimeState& getLicenseRuntimeState() noexcept { return licenseRuntime_; }
    LicenseManager& getLicenseManager() noexcept { return *licenseManager_; }

private:
    LicenseRuntimeState licenseRuntime_;
    std::unique_ptr<LicenseManager> licenseManager_;
};

MorePhiProcessor::MorePhiProcessor()
{
    licenseManager_ = std::make_unique<LicenseManager>(licenseRuntime_);
    licenseManager_->loadCachedCertificateAsync();
}
```

### 11.3 Do Not Store License in DAW Project State

`getStateInformation()` should serialize plugin parameters, snapshots, hosted plugin state, and UI state. It should not serialize the license key or activation certificate into project files because:

- Project files are often shared with collaborators.
- DAWs may expose project state in crash reports.
- License data should remain per-user/per-machine, not per-session.

Good state pattern:

```cpp
void MorePhiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto xml = std::make_unique<juce::XmlElement>("MorePhiState");
    xml->addChildElement(apvts_.copyState().createXml().release());
    // snapshots, morph state, hosted plugin state...
    copyXmlToBinary(*xml, destData);
}
```

### 11.4 Parameter and Feature Gating

Avoid removing parameters based on license state because DAW automation depends on stable parameter layouts. Instead:

- Always expose the same parameters.
- Gate behavior internally.
- Display locked UI affordances for premium controls.
- Preserve project recall even when license is inactive.

```cpp
bool MorePhiProcessor::isFeatureEnabled(Feature feature) const noexcept
{
    const auto state = licenseRuntime_.state.load(std::memory_order_relaxed);
    if (state == LicenseState::Licensed || state == LicenseState::TrialActive)
        return licenseRuntime_.featureMask.load(std::memory_order_relaxed) & toMask(feature);
    return false;
}
```

---

## 12. C++ Implementation Patterns

### 12.1 License Types

```cpp
enum class LicenseType
{
    Trial,
    Perpetual,
    Subscription,
    NodeLocked,
    Floating
};

struct LicensePayload
{
    std::string schema;
    std::string licenseId;
    std::string activationId;
    std::string productId;
    LicenseType type;
    std::vector<std::string> features;
    std::chrono::system_clock::time_point issuedAt;
    std::optional<std::chrono::system_clock::time_point> validUntil;
    std::optional<std::chrono::system_clock::time_point> trialEndsAt;
    std::optional<std::chrono::system_clock::time_point> subscriptionEndsAt;
    std::chrono::system_clock::time_point nextOnlineCheckAt;
    std::chrono::system_clock::time_point offlineGraceEndsAt;
    std::string machineHash;
};

struct SignedCertificate
{
    std::string payloadBase64Url;
    std::string signatureBase64Url;
    std::string keyId;
};
```

### 12.2 Secure Store Interface

```cpp
class SecureLicenseStore
{
public:
    std::optional<SignedCertificate> loadCertificate();
    bool saveCertificate(const SignedCertificate& certificate);
    bool clearCertificate();

private:
    juce::File getCertificateFile() const;
    std::vector<uint8_t> encryptForLocalUser(std::span<const uint8_t> plaintext);
    std::vector<uint8_t> decryptForLocalUser(std::span<const uint8_t> ciphertext);
};
```

### 12.3 Background Validation Pattern

```cpp
class LicenseManager : private juce::Thread
{
public:
    explicit LicenseManager(LicenseRuntimeState& runtime)
        : juce::Thread("MorePhi License Validation"), runtime_(runtime) {}

    void loadCachedCertificateAsync()
    {
        pendingJob_.store(Job::LoadCachedCertificate);
        startThread();
    }

    void activateAsync(std::string licenseKey)
    {
        pendingLicenseKey_ = std::move(licenseKey);
        pendingJob_.store(Job::ActivateOnline);
        startThread();
    }

private:
    enum class Job { None, LoadCachedCertificate, ActivateOnline, RefreshOnline };

    void run() override
    {
        const auto job = pendingJob_.exchange(Job::None);
        switch (job)
        {
            case Job::LoadCachedCertificate: loadCachedCertificate_(); break;
            case Job::ActivateOnline: activateOnline_(); break;
            case Job::RefreshOnline: refreshOnline_(); break;
            default: break;
        }
    }

    void publish_(const ValidationResult& result)
    {
        runtime_.state.store(result.state, std::memory_order_release);
        runtime_.premiumFeaturesEnabled.store(result.enablesPremiumFeatures,
                                             std::memory_order_release);
    }

    LicenseRuntimeState& runtime_;
    std::atomic<Job> pendingJob_ { Job::None };
    std::string pendingLicenseKey_;
    SecureLicenseStore store_;
    LicenseVerifier verifier_;
    ActivationClient client_;
};
```

---

## 13. Error Handling and Grace Policies

### 13.1 Error Codes

| Code | Meaning | User Message | Suggested Behavior |
|---|---|---|---|
| `LICENSE_INVALID_FORMAT` | Typo/checksum failure | “That license key format doesn’t look right.” | Stay on activation screen |
| `LICENSE_SIGNATURE_INVALID` | Certificate modified/forged | “License file could not be verified.” | Require reactivation |
| `LICENSE_PRODUCT_MISMATCH` | Wrong product | “This license is for another product.” | Require correct key |
| `LICENSE_MACHINE_MISMATCH` | Different computer | “This license is activated on another computer.” | Offer transfer/deactivation |
| `LICENSE_SEAT_LIMIT` | Too many activations | “Activation limit reached.” | Link account portal/support |
| `LICENSE_EXPIRED` | Trial/subscription ended | “Your license has expired.” | Disable premium features |
| `NETWORK_UNAVAILABLE` | Offline/no server | “Couldn’t reach validation server.” | Use grace if available |
| `CLOCK_SKEW` | System time suspicious | “System date/time appears incorrect.” | Temporary block online refresh |
| `LEASE_UNAVAILABLE` | Floating seats used | “No floating seats are currently available.” | Retry/queue option |

### 13.2 Grace Period Rules

Recommended policy:

- If a previously valid license cannot be refreshed due to network failure, allow a **14-day grace period**.
- If server explicitly says license revoked, expired, or refunded, do **not** extend grace beyond existing certificate policy.
- For subscriptions, allow grace only if `subscriptionEndsAt` plus server policy allows it.
- For floating licenses, use shorter leases and clearer UI because seats are shared.

### 13.3 Fallback Behavior

The plugin should remain project-safe:

- Continue loading saved sessions.
- Preserve parameter automation.
- Allow preset export/import if not premium-gated.
- Disable premium-only operations with clear UI messaging.
- Avoid audio dropouts or sudden destructive changes.

Possible demo restrictions:

- Disable saving new premium presets.
- Disable MCP/AI premium controls.
- Periodic subtle audio watermark only in unlicensed trial mode, never in paid grace mode.
- Limit hosted plugin slots or snapshot count in unlicensed mode.

---

## 14. Activation UI / UX

### 14.1 UI States

```text
┌──────────────────────────────────────────────┐
│ More-Phi Activation                          │
├──────────────────────────────────────────────┤
│ Status: Trial active — 10 days remaining     │
│                                              │
│ License Key                                  │
│ [ MPH1-____-____-____-____-____-_ ]          │
│                                              │
│ [Activate] [Start Trial] [Offline Activation]│
│                                              │
│ Already activated? Last checked Mar 8, 2026  │
└──────────────────────────────────────────────┘
```

### 14.2 Messaging Principles

- Use human-readable explanations, not raw error codes.
- Show next action: “Reconnect to the internet within 9 days” is better than “validation failed”.
- Avoid scary messages during live sessions.
- Never block plugin scanning with modal dialogs. Plugin scanners may instantiate plugins without user interaction.

### 14.3 Deactivation / Transfer

Flow:

```text
Settings → License → Deactivate this computer
        │
        ├─ Online: server releases activation seat immediately
        └─ Offline: plugin creates signed deactivation request; user submits via portal
```

Deactivation request:

```json
{
  "activationId": "act_01HZY...",
  "machineHash": "b64url_sha256_hash",
  "createdAt": "2026-03-08T12:00:00Z",
  "reason": "user_transfer"
}
```

Server should tolerate lost/stale devices by allowing self-service reset limits, for example one reset per 30 days.

---

## 15. Floating License Design

Floating licenses are best for studios and teams.

### 15.1 Lease Lifecycle

```text
Plugin opens → request lease
        │
        ├─ lease available → signed lease certificate for 12 hours
        │       └─ renew every 30 minutes while plugin/DAW is active
        │
        └─ no lease available → show read-only/demo state and retry option
```

### 15.2 Lease API

```http
POST /v1/floating/leases
```

```json
{
  "licenseKey": "MPH1-...",
  "machineHash": "...",
  "sessionId": "plugin-instance-uuid",
  "dawHint": "Logic Pro",
  "requestedDurationSeconds": 43200
}
```

Server response:

```json
{
  "status": "leased",
  "leaseEndsAt": "2026-03-09T00:00:00Z",
  "renewAfterSeconds": 1800,
  "certificate": { "payload": "...", "signature": "...", "keyId": "..." }
}
```

---

## 16. Security Hardening

### 16.1 Defensive Measures

Use layered defenses:

- Public-key signature verification.
- Local certificate encryption or OS-protected storage.
- Multiple validation checkpoints outside obvious single branches.
- Server-side activation limits and anomaly detection.
- Short-lived floating leases.
- Key rotation via `keyId`.
- Build hardening flags and symbol stripping for release builds.
- Obfuscate non-critical strings and avoid obvious debug names in release binaries.
- Validate feature entitlement at feature boundaries, not only at startup.

### 16.2 Anti-Tamper Guidelines

Practical defensive patterns:

- Keep validation code small, heavily tested, and isolated.
- Avoid one global `isLicensed()` branch controlling everything.
- Derive feature masks from the signed certificate and check them where features execute.
- Store validation state in immutable snapshots and atomics, not mutable global strings.
- Use server telemetry to identify impossible activation patterns.
- Keep release builds stripped and signed/notarized where applicable.

Do not:

- Put private keys or shared signing secrets in the binary.
- Rely on checksums as security.
- Block the audio thread during validation.
- Punish users with destructive behavior when validation fails.
- Log full license keys or personal data.

### 16.3 Secure Communication

- Use HTTPS with normal platform certificate validation.
- Add request nonces to reduce replay risk.
- Include `pluginVersion`, `productId`, `activationId`, and `machineHash` in validation requests.
- Use server-side rate limiting by IP, license key, and account.
- Consider certificate pinning only if you have an operational rotation plan.
- Never disable TLS verification to “fix” user network issues.

---

## 17. Audit Trail and Server Model

### 17.1 Core Tables

```text
licenses
  id, product_id, license_key_hash, type, status, customer_id,
  max_activations, subscription_ends_at, created_at

activations
  id, license_id, machine_hash, status, activated_at,
  last_seen_at, deactivated_at, plugin_version, os_hint

validation_events
  id, license_id, activation_id, event_type, result,
  created_at, ip_hash, user_agent_hash

floating_leases
  id, license_id, activation_id, session_id, status,
  lease_starts_at, lease_ends_at, renewed_at
```

### 17.2 Audit Events

- License created.
- Activation succeeded/failed.
- Seat limit exceeded.
- Validation refreshed.
- License revoked/refunded.
- Deactivation completed.
- Floating lease checked out/renewed/released/expired.

Hash IP addresses if you do not need exact values for support or fraud prevention.

---

## 18. GDPR and Privacy Considerations

### 18.1 Data Minimization

Collect only what is needed:

- License key hash.
- Activation ID.
- Machine hash, not raw hardware data.
- Product/plugin version.
- OS family and architecture.
- Optional DAW hint for support diagnostics.

Avoid collecting:

- Project names.
- Track names.
- Audio content.
- Preset names unless explicitly part of cloud sync.
- Raw serial numbers or MAC addresses.

### 18.2 User Rights

Provide a privacy policy covering:

- What data is collected.
- Why licensing data is processed.
- Retention windows.
- How users request deletion/export.
- How deletion interacts with purchase records, invoices, and fraud prevention obligations.

### 18.3 Suggested Retention

| Data | Retention |
|---|---|
| Purchase/license record | Duration required for accounting/legal support |
| Activation records | License lifetime + limited support window |
| Validation logs | 90–180 days unless needed for abuse investigation |
| Floating lease events | 30–90 days |
| Support diagnostics | Until ticket resolution + short buffer |

---

## 19. Testing Plan

### 19.1 Unit Tests

- License key normalization.
- Checksum acceptance/rejection.
- Signature verification.
- Product mismatch rejection.
- Expired trial/subscription handling.
- Machine hash mismatch handling.
- Grace period transitions.
- Clock skew tolerance.

### 19.2 Integration Tests

- Activate online with mocked server.
- Activate offline with imported certificate.
- Save/load certificate from storage.
- Deactivate and clear local certificate.
- Server unavailable uses cached certificate and grace.
- Floating lease renewal and expiry.

### 19.3 DAW Lifecycle Tests

Run in Ableton Live, Logic Pro, FL Studio, Reaper, and pluginval:

- Plugin scan does not display blocking modal UI.
- Plugin loads without network.
- Project opens with expired/unlicensed state without crashing.
- License UI can activate while audio is playing.
- Multiple plugin instances share one cached activation.
- No allocations/network/disk work in `processBlock()`.

### 19.4 Abuse/Edge Tests

- Modified certificate payload.
- Modified signature.
- Unknown `keyId`.
- System clock set backwards/forwards.
- License key pasted with spaces/lowercase/dashes omitted.
- Storage file missing/corrupt.
- Server returns revoked/seat-limit/network-timeout.

---

## 20. Build and Configuration Guidelines

### 20.1 Compile-Time Configuration

```cpp
namespace more_phi::licensing::config
{
    inline constexpr std::string_view PRODUCT_ID = "more-phi-vst3";
    inline constexpr std::string_view LICENSE_SCHEMA = "morephi-license-cert-v1";
    inline constexpr int OFFLINE_GRACE_DAYS = 14;
    inline constexpr int PERPETUAL_CHECK_INTERVAL_DAYS = 30;
    inline constexpr int SUBSCRIPTION_CHECK_INTERVAL_DAYS = 7;
}
```

### 20.2 Environment/Release Configuration

Keep server URLs and public key sets separate for staging vs production:

```json
{
  "environment": "production",
  "activationBaseUrl": "https://licensing.example.com/api",
  "publicKeys": [
    {
      "keyId": "prod-ed25519-2026-01",
      "algorithm": "Ed25519",
      "publicKeyBase64": "..."
    }
  ]
}
```

Do not ship staging public keys in production unless intentionally supported.

### 20.3 Plugin Runtime HTTP Configuration

The implemented C++ activation client reads these process environment variables when constructing `LicenseManager`:

```text
LICENSE_API_BASE_URL=https://your-domain.com/api
MOREPHI_PUBLIC_CLIENT_TOKEN=public_plugin_client_token
MOREPHI_CLIENT_HEADER=X-MorePhi-Client
```

If either `LICENSE_API_BASE_URL` or `MOREPHI_PUBLIC_CLIENT_TOKEN` is missing, the plugin fails closed to `StubActivationClient` and online activation returns `not_configured`.

Configured endpoints:

```text
POST {LICENSE_API_BASE_URL}/api/plugin/licenses/activate
POST {LICENSE_API_BASE_URL}/api/plugin/licenses/refresh
POST {LICENSE_API_BASE_URL}/api/plugin/licenses/deactivate
```

Activation request body:

```json
{
  "license_key": "MPH1-XXXX-XXXX-XXXX-XXXX-XXXX-C or MPHI-XXXXX-XXXXX-XXXXX-XXXXX",
  "machine_id": "hashed-machine-id",
  "plugin_version": "3.4.1",
  "platform": "windows-vst3 or macos-au/vst3 hint",
  "daw": "Ableton Live",
  "product_id": "more-phi-vst3",
  "request_nonce": "uuid"
}
```

Expected successful response may use either compact certificate shape:

```json
{
  "status": "active",
  "certificate": {
    "payload": "base64url(canonical-json)",
    "signature": "base64url(signature)",
    "keyId": "morephi-prod-2026-01"
  }
}
```

Or backend-friendly shape:

```json
{
  "status": "active",
  "certificate_payload": "base64url(canonical-json) or raw canonical JSON string",
  "signature": "base64url(signature)",
  "public_key_id": "morephi-prod-2026-01"
}
```

The C++ client accepts either base64url-encoded payloads or raw canonical JSON strings in `certificate_payload`; it normalizes raw JSON before verifier validation.

---

## 21. Third-Party Licensing Solutions

Custom licensing is appropriate if you need full control, but third-party platforms may reduce operational burden.

| Provider Type | Examples | Fit |
|---|---|---|
| Software licensing APIs | Keygen, Cryptolens, LicenseSpring, LimeLM | Good for activation keys, seats, machine licensing |
| Commerce + licensing | Paddle, FastSpring integrations | Good for payments plus license issuance |
| Enterprise/floating license managers | Reprise RLM, FlexNet Publisher | Good for high-value B2B/floating seats |
| Custom backend | Your own API + signing service | Best control, highest maintenance |

### 21.1 Selection Criteria

Choose based on:

- Offline activation support.
- Node-locked activations.
- Floating lease support.
- C/C++ SDK availability and static linking constraints.
- Privacy/data residency requirements.
- Webhook support for purchases/refunds/subscription changes.
- Ability to use signed offline certificates.
- Vendor availability if their service is down.

### 21.2 Recommendation

For a commercial VST3 plugin like More-Phi:

- **MVP:** Use a third-party licensing platform that supports offline activation and signed local license files.
- **Long term:** Move to a custom signing service only if licensing rules, privacy, floating seats, or product analytics require deeper control.

---

## 22. Implementation Roadmap

### Phase 1 — Local Validation Foundation

- Add `src/Licensing` module.
- Implement license key normalization and checksum.
- Implement signed certificate parser/verifier.
- Implement secure store.
- Add activation UI shell.
- Add unit tests.

### Phase 2 — Online Activation

- Implement `/v1/activations` server endpoint.
- Implement plugin `ActivationClient`.
- Add server-side signing service.
- Add activation/deactivation UI.
- Add integration tests with mocked server.

### Phase 3 — Offline and Grace

- Implement offline request/response import.
- Add scheduled validation refresh.
- Add grace-period messaging.
- Add corrupt/missing certificate recovery flows.

### Phase 4 — Subscription and Floating

- Add subscription refresh policy.
- Add floating lease checkout/renew/release.
- Add team seat dashboards server-side.

### Phase 5 — Hardening and Compliance

- Add audit logs and support diagnostics.
- Add privacy policy language.
- Strip symbols and sign/notarize builds.
- Run DAW lifecycle and pluginval validation.

---

## 23. Acceptance Checklist

- [ ] Plugin loads with no network connection.
- [ ] Plugin scan never shows blocking activation dialogs.
- [ ] Audio thread performs only atomic license reads.
- [ ] License certificate signature verification works offline.
- [ ] Trial, perpetual, subscription, node-locked, and floating states are represented.
- [ ] Expiry and grace period transitions are deterministic.
- [ ] License key is never stored in DAW project state.
- [ ] Private signing key is never shipped in the plugin.
- [ ] Modified certificate payload is rejected.
- [ ] Machine mismatch is handled with transfer/deactivation UX.
- [ ] GDPR data minimization is documented.
- [ ] Error messages are clear and actionable.

---

## 24. Practical Default Design for More-Phi

For More-Phi specifically, implement this default policy:

- License format: `MPH1-XXXXX-XXXXX-XXXXX-XXXXX-C`.
- Activation certificate: signed canonical JSON with Ed25519.
- License types: trial, perpetual, subscription, floating.
- Storage: per-user application data plus OS-protected wrapper where available.
- Validation timing:
  - Validate cached certificate on editor/message thread startup.
  - Refresh online every 30 days for perpetual.
  - Refresh online every 7 days for subscription.
  - Renew floating leases every 30 minutes.
- Grace:
  - 14 days for failed refresh after a known-good paid license.
  - No grace for forged/modified certificates.
- UI:
  - Non-modal activation panel in settings.
  - Status chip visible in the top/right utility area.
  - Offline activation import/export available from the same panel.
- Feature gating:
  - Keep DAW parameters stable.
  - Gate premium behavior at feature execution points.
  - Preserve saved projects regardless of license state.

This gives a strong, user-friendly licensing foundation without violating the real-time and lifecycle constraints of VST3/AU plugin hosting.

---

## 25. Recent Changes

### More-Phi v3.4.1
- **Experimental research artifacts quarantined to `research/`:** All experimental and research-oriented code artifacts (dataset generation V2/V3 pipelines, inference server prototypes, and related tooling) have been moved to the `research/` directory at the repository root. This has **no impact on the production build** — these components are excluded from the standard VST3/AU build path and do not affect runtime behavior, plugin load times, or DAW compatibility. Production code continues to live under `src/` and is built normally via CMake.