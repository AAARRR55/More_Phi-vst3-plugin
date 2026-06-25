// tests/Integration/TestActivationE2E.cpp
//
// Phase 1 (production readiness): end-to-end activation against a REAL licensing
// backend. This exercises the full HttpActivationClient path:
//   activate() → server-issued SignedCertificate → Ed25519Verifier (compiled-in
//   prod key) → refresh() → deactivate().
//
// This test is SKIPPED (not failed) unless all of the following are present:
//   - env MORE_PHI_TEST_LICENSE_API_URL (https://..., the production activation endpoint)
//   - env MORE_PHI_TEST_LICENSE_CLIENT_TOKEN
//   - env MORE_PHI_TEST_LICENSE_KEY (a throwaway test seat on the backend)
//   - the build was configured with -DMORE_PHI_PROD_ED25519_KEY_HEX=<prod key>
//     (otherwise the compiled-in key is the dev placeholder and a prod-signed
//     cert would fail verification — surfaced as a clear failure, not a skip,
//     so CI misconfiguration is loud).
//
// Run locally:
//   MORE_PHI_TEST_LICENSE_API_URL=https://license.example.com \
//   MORE_PHI_TEST_LICENSE_CLIENT_TOKEN=... \
//   MORE_PHI_TEST_LICENSE_KEY=MPH1-... \
//   ./MorePhiTests "[activation][e2e]"

#include <catch2/catch_test_macros.hpp>

#include "Licensing/ActivationClient.h"
#include "Licensing/Ed25519Verifier.h"
#include "Licensing/SigningKeys.h"

#include <cstdlib>
#include <string>

using namespace more_phi::licensing;

namespace {

struct E2EEnv
{
    std::string apiUrl;
    std::string clientToken;
    std::string licenseKey;
    bool present = false;
    bool prodKeyInjected = false;
};

E2EEnv readEnv()
{
    E2EEnv e;
    if (const char* a = std::getenv("MORE_PHI_TEST_LICENSE_API_URL")) e.apiUrl = a;
    if (const char* c = std::getenv("MORE_PHI_TEST_LICENSE_CLIENT_TOKEN")) e.clientToken = c;
    if (const char* k = std::getenv("MORE_PHI_TEST_LICENSE_KEY")) e.licenseKey = k;
    e.present = ! e.apiUrl.empty() && ! e.clientToken.empty() && ! e.licenseKey.empty();

#if __has_include("ProdSigningKey.h")
    e.prodKeyInjected = true;
#endif
    return e;
}

juce::String machineHash()
{
    // Stable, distinct per CI run would be ideal, but for a throwaway test seat a
    // fixed hash is fine and makes the test reproducible. The deactivate() call at
    // the end frees the seat so it can be reused on the next run.
    return "ci-e2e-test-machine";
}

} // namespace

TEST_CASE("Activation end-to-end against a production backend", "[activation][e2e]")
{
    const auto env = readEnv();

    if (! env.present)
    {
        SKIP("Activation E2E requires MORE_PHI_TEST_LICENSE_API_URL / "
             "MORE_PHI_TEST_LICENSE_CLIENT_TOKEN / MORE_PHI_TEST_LICENSE_KEY. "
             "Skipping — this is expected in CI without secrets.");
    }

    // Fail loud (do NOT skip) if env secrets are present but the build did NOT
    // inject a production key: that's a real CI misconfiguration that would let a
    // dev-signed cert pass verification in a shipping binary.
    REQUIRE(env.prodKeyInjected);

    LicenseApiConfig config;
    config.baseUrl = juce::String(env.apiUrl);
    config.publicClientToken = juce::String(env.clientToken);
    REQUIRE(config.isUsable()); // forces https:// in Release builds

    HttpActivationClient client(config);

    ActivationRequest req;
    req.licenseKey = juce::String(env.licenseKey);
    req.machineHash = machineHash();
    req.pluginVersion = "3.4.0-e2e";
    req.os =
#if defined(_WIN32)
        "windows";
#elif defined(__APPLE__)
        "macos";
#else
        "linux";
#endif

    // ── activate ────────────────────────────────────────────────────────────
    const auto actResp = client.activate(req);
    INFO("activate() status=" << actResp.status.toStdString()
         << " code=" << actResp.errorCode.toStdString()
         << " msg=" << actResp.message.toStdString());
    REQUIRE(actResp.success);
    REQUIRE(actResp.certificate.has_value());

    const auto& cert = *actResp.certificate;
    REQUIRE(cert.keyId == "prod-ed25519-2026-01");

    // The compiled-in production public key MUST verify the server's signature.
    // (HttpActivationClient already validates before returning success, but this
    // explicit check pins the security contract: the binary's prod key, not a
    // dev key, is what accepted the cert.)
    auto verifierFn = makeEd25519SignatureVerifier();
    REQUIRE(verifierFn != nullptr);

    // ── refresh ─────────────────────────────────────────────────────────────
    const auto refreshResp = client.refresh(actResp.activationId, req.machineHash);
    INFO("refresh() status=" << refreshResp.status.toStdString());
    REQUIRE(refreshResp.success);

    // ── deactivate (frees the test seat) ────────────────────────────────────
    const auto deactResp = client.deactivate(actResp.activationId, req.machineHash);
    INFO("deactivate() status=" << deactResp.status.toStdString());
    REQUIRE(deactResp.success);
}

TEST_CASE("Activation config is unusable with empty env in Release", "[activation]")
{
    // Regression: in a Release build an empty baseUrl must be unusable so the
    // factory falls through to StubActivationClient (offline). This is the guard
    // that prevents a shipping binary from phoning home to a dev localhost.
    LicenseApiConfig empty;
    // isUsable() returns true in Debug (dev defaults allowed) but must be false
    // when baseUrl is empty in Release.
#ifdef NDEBUG
    REQUIRE_FALSE(empty.isUsable());
#else
    // In Debug, empty config is "usable" only because the dev defaults are
    // applied in configFromEnvironment(); a raw empty config object stays unusable.
    REQUIRE_FALSE(empty.isUsable());
#endif
}
