# PRD — VST3 License Key Management Implementation

## Original Problem Statement
Create and implement a complete license key management system for a VST3 plugin, covering key generation/validation, multiple license types, secure storage, offline activation, periodic validation, anti-tamper guidance, VST3/JUCE integration, user activation flows, compliance, C++ patterns, and third-party options.

## Architecture Decisions
- Target stack: More-Phi JUCE 8 / C++20 VST3/AU plugin.
- Implemented a local C++ licensing module under `src/Licensing`.
- Runtime state is audio-thread safe via atomics (`LicenseRuntimeState`).
- License certificates are signed-token oriented, parsed from base64url JSON envelopes, and validated off the audio path.
- Online activation is represented by a mockable `IActivationClient`; current implementation uses `StubActivationClient` until a real backend/provider is chosen.
- Cached license loading is scheduled through the existing message-thread maintenance timer to avoid audio-thread I/O.

## Implemented
- Added license model, key parser/checksum, runtime state, verifier, secure store, machine fingerprint, activation client interface/stub, and license manager.
- Added configurable real HTTP activation client using `LICENSE_API_BASE_URL`, `MOREPHI_PUBLIC_CLIENT_TOKEN`, and optional `MOREPHI_CLIENT_HEADER`.
- Wired plugin endpoints: `/plugin/licenses/activate`, `/plugin/licenses/refresh`, and `/plugin/licenses/deactivate`.
- Added activate/refresh certificate response parsing for both compact and backend-friendly response shapes; deactivation accepts 2xx empty-body success.
- Wired licensing sources into CMake for plugin and CLI targets.
- Exposed `getLicenseRuntimeState()` and `getLicenseManager()` from `MorePhiProcessor`.
- Added one-shot cached certificate load via `loadCachedLicenseIfNeeded()` on the message-thread timer.
- Added Catch2 tests for key normalization/checksum and certificate validation/machine mismatch.
- Testing agent completed static review; full CMake/Catch2 execution could not run because `cmake` is missing in this container.

## Prioritized Backlog
### P0
- Run full CMake configure/build/tests in an environment with CMake + JUCE FetchContent available.
- Replace development signature verifier with production Ed25519/libsodium or selected licensing-provider verifier before release.
- Wire activation UI to `LicenseManager::activateWithKey`, `importOfflineCertificate`, and `clearActivation`.

### P1
- Add tests for `LicenseManager` cached-store load and runtime-state publication.
- Encrypt/wrap local certificate using platform keychain/DPAPI where available.

### P2
- Add floating lease renewal/release workflow.
- Add server audit event contracts and privacy retention configuration.
- Add support diagnostics panel for license state/error details.

## Next Tasks
- Choose real licensing backend/provider.
- Add production signature verification dependency.
- Build activation UI and connect to the new manager APIs.
