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

## Validation Update
- Installed CMake and required JUCE Linux build headers in the container.
- CMake configure succeeded with `MORE_PHI_BUILD_TESTS=ON`, `MORE_PHI_BUILD_COMPREHENSIVE_E2E=OFF`, and `MORE_PHI_SAFE_BUILD_MODE=ON`.
- Built `MorePhiTests` successfully.
- Ran full Catch2 suite: 415 test cases, 71,027 assertions, all passed.
- Fixed three existing JUCE/GCC compile blockers discovered during full build: ambiguous `juce::var` int64 conversion in `AutomationControlPlane.cpp`, ambiguous `juce::File` reset in `TrackAssistantStore.cpp`, and ambiguous int64 conversions in `MCPToolHandler.cpp`.

## Landing Page Backend Wiring Update
- Cloned and inspected `https://github.com/AAARRR55/more-phi-landing-page.git`.
- Added plugin-public FastAPI routes in the cloned repo: `/api/plugin/licenses/activate`, `/api/plugin/licenses/refresh`, `/api/plugin/licenses/deactivate`.
- Backend routes use `X-MorePhi-Client` with `MOREPHI_PUBLIC_CLIENT_TOKEN`, license key + machine ID validation, activation limits, and signed activation certificates.
- C++ client now calls `/api/plugin/licenses/*` and accepts current backend `MPHI-XXXXX-XXXXX-XXXXX-XXXXX` license keys as well as newer `MPH1` keys.
- Validation: Python syntax/lint passed for backend changes; C++ full suite passed with 416 test cases and 71,029 assertions.
- Note: cloned GitHub repo changes are staged in `/tmp/more-phi-landing-page`; push/apply them to that repository using the platform’s GitHub save flow.

## Final Backend Wiring Validation
- Removed insecure JWT_SECRET-derived signing fallback; backend now requires `LICENSE_SIGNING_PRIVATE_KEY_PEM` for activation certificate signing.
- Deactivation now validates activation/license/product context before releasing a machine.
- Added positive-path plugin API pytest skeleton guarded by `MOREPHI_TEST_LICENSE_KEY`.
- Exported latest landing-page repo patch to `/app/integration_patches/more-phi-landing-page-plugin-licensing.patch`.
- Runtime live endpoint tests require `NEXT_PUBLIC_API_URL` or `REACT_APP_BACKEND_URL`, `MOREPHI_PUBLIC_CLIENT_TOKEN`, backend `LICENSE_SIGNING_PRIVATE_KEY_PEM`, and `MOREPHI_TEST_LICENSE_KEY`.

## Live Plugin API Test Attempt
- Checked runtime environment for `NEXT_PUBLIC_API_URL`, `REACT_APP_BACKEND_URL`, `MOREPHI_PUBLIC_CLIENT_TOKEN`, `MOREPHI_TEST_LICENSE_KEY`, and `LICENSE_SIGNING_PRIVATE_KEY_PEM`; none were present.
- Ran `/tmp/more-phi-landing-page/backend/tests/test_plugin_license_api.py`; result: 3 tests skipped because live API URL/token/test license were not configured.
- JUnit report saved at `/app/test_reports/plugin_live_api.xml`.
- Live plugin activation flow is not validated yet because required runtime values are missing.
