# More-Phi MCP Server & Communication Protocol Audit Report

**Version:** 3.3.0  
**Auditor:** Sub-Agent 4 — MCP Server & Communication Protocol Auditor  
**Date:** 2025-06-27  
**Scope:** `src/AI/` (MCPServer, MCPToolHandler, MCPToolsExtended, StandaloneMcp, InstanceRegistry, TokenOptimizer, LockFreeQueue)

---

## Executive Summary

The More-Phi MCP server implementation provides a functional JSON-RPC 2.0 interface for AI-driven parameter control with several well-designed security features (localhost-only binding, bearer-token authentication, rate limiting, and non-local connection rejection). However, the audit reveals **3 critical security/correctness issues**, **6 high-priority robustness concerns**, and a number of medium/low-priority refinements. The most severe issues are a **timing side-channel in token validation**, **incorrect JSON-RPC notification handling**, and a **potential deadlock in TokenOptimizer**.

---

## Critical Issues

### C-1: `validateAuth()` Timing Side-Channel Leaks Expected Token Length
**File:** `MCPServer.cpp` (lines 449–483)  
**Severity:** Critical — Security

The "constant-time comparison" (M-11 FIX) compares `std::max(candidate.size(), expected.size())` iterations. Because the loop length depends on the longer of the two strings, an attacker can probe the expected token length by sending tokens of varying lengths and measuring comparison time. When the attacker’s token length exceeds the expected length, the loop duration jumps, revealing the secret length. This is a classic timing side-channel vulnerability.

Additionally, the `volatile uint8_t diff` trick is not guaranteed to produce constant-time behavior on modern optimizing compilers or CPUs with out-of-order execution and cache timing effects.

**Recommended Fix:**
```cpp
// Use a fixed comparison length (e.g., 32 bytes for SHA-256 HMAC)
static constexpr size_t kTokenLen = 32; // bytes

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
        diff |= (a[i] ^ b[i]);
    return diff == 0;
}
```
Alternatively, replace the bearer token with a time-limited HMAC-SHA256 signature using a per-instance secret key.

---

### C-2: JSON-RPC Notifications Are Not Suppressed — Protocol Non-Compliance
**File:** `MCPServer.cpp` (lines 292–447)  
**Severity:** Critical — Protocol Correctness

JSON-RPC 2.0 requires that requests **without** an `id` field (notifications) **must not** produce a response (`{"result": ...}` or `{"error": ...}`). The `processRequest()` method always returns a response string, even when `id` is missing (line 304: `idVar` is `void`, so `reqId` stays `nullptr`, and the response is still built with `"id": nullptr`).

This breaks clients that rely on notification semantics, can cause the client to misinterpret the response as an error, and violates the MCP protocol which uses `notifications/initialized` (line 345) as a server-to-client notification, not a request-response pair.

**Recommended Fix:**
```cpp
// After parsing idVar:
if (idVar.isVoid()) {
    // Notification — process but do not return any response
    return {}; // Empty string = no response
}
```

> ⚠️ **Cascading Impact:** If notifications are suppressed, the `initialize` handshake (which sends a notification *after* the response) must also be re-examined. The current implementation concatenates two JSON objects with `\n` (line 347). This is a custom framing extension that may confuse strict JSON-RPC clients. Consider using a proper message boundary (e.g., Content-Length headers per the MCP spec) or sending the notification as a separate asynchronous write.

---

### C-3: TokenOptimizer Deadlock via Lock-Order Inversion
**File:** `TokenOptimizer.cpp` (lines 127–140, 455–475)  
**Severity:** Critical — Thread Safety

Two functions acquire `budgetMutex_` and `statsMutex_` in **opposite order**:

- `isBudgetExceeded()` (line 127): `budgetMutex_` → `statsMutex_`
- `generateUsageReport()` (line 455): `statsMutex_` → `budgetMutex_`
- `getDisplayData()` (line 536): `statsMutex_` → `budgetMutex_`

If two threads call these functions concurrently, a classic AB-BA deadlock can occur. The MCP thread and UI thread both touch `TokenOptimizer`.

**Recommended Fix:**
```cpp
// Define a strict global lock order:
// 1. statsMutex_  2. budgetMutex_  3. usageMutex_  4. rateMutex_  5. batchMutex_  6. contextMutex_
// Enforce in all functions, or use a single coarse-grained mutex for budget+stats
// since they are always accessed together.
```

---

## High-Priority Improvements

### H-1: No JSON-RPC Batch Request Support
**File:** `MCPServer.cpp` (line 295), `StandaloneMcpServer.cpp` (line 332)  
**Severity:** High — Protocol Compliance

`MCPServer::processRequest()` assumes every request is a single JSON object. JSON-RPC 2.0 mandates that servers **must** support batch requests (arrays of request objects). If a client sends `[{"jsonrpc":"2.0","method":"tools/list","id":1}, ...]`, `juce::JSON::parse` returns an array, `getProperty("method", "")` returns empty, and the server responds with `Invalid Request` (-32600). This breaks spec-compliant clients and LLM tool-calling frameworks that batch requests for efficiency.

**Recommended Fix:**
```cpp
auto parsed = juce::JSON::parse(jsonRequest);
if (parsed.isArray()) {
    json batchResponses = json::array();
    for (auto& item : *parsed.getArray()) {
        bool auth = authenticated; // per-request auth state
        auto resp = processSingleRequest(item, auth);
        if (resp.isNotEmpty()) batchResponses.push_back(json::parse(resp.toStdString()));
    }
    return juce::String(batchResponses.dump());
}
```

---

### H-2: `ConnectionThread` Constructor Ignores `startThread()` Failure
**File:** `MCPServer.cpp` (lines 54–58)  
**Severity:** High — Resource Leak / Crash Vector

```cpp
ConnectionThread::ConnectionThread(MCPServer& owner, juce::StreamingSocket* socket)
    : Thread("MCP-Connection"), owner_(owner), socket_(socket)
{
    startThread();  // Return value ignored!
}
```

If `startThread()` fails (e.g., OS thread limit reached), the `ConnectionThread` object is still added to `activeConnections_`, but `run()` never executes. The socket is never drained, and `connectedClients_` is never incremented (line 81 is inside `run()`). However, the socket pointer is leaked because the destructor’s `signalExit()` / `stopThread()` path may not close it properly if the thread was never started.

**Recommended Fix:**
```cpp
bool ConnectionThread::startThreadSafe() {
    return startThread(); // Return value propagated
}
// In MCPServer::run():
auto* conn = new ConnectionThread(*this, client);
if (!conn->startThreadSafe()) {
    delete conn;
    delete client;
    logError("connection", "Failed to start connection thread");
    continue;
}
activeConnections_.add(conn);
```

---

### H-3: `connectedClients_` Can Leak on Forced Thread Termination
**File:** `MCPServer.cpp` (lines 81, 186, 65–69)  
**Severity:** High — State Corruption

`connectedClients_` is incremented at the top of `ConnectionThread::run()` (line 81) and decremented at the bottom (line 186). If the thread is force-stopped via `stopThread()` in the destructor (line 65) before `run()` reaches line 186, the decrement never happens. Repeated rapid connect/disconnect cycles during plugin unload can inflate the count permanently, misleading monitoring and potentially blocking new connections if a threshold is ever enforced.

**Recommended Fix:** Use RAII guard or decrement in `signalExit()` / destructor:
```cpp
class ConnectionThread : public juce::Thread {
    struct ClientCounterGuard {
        std::atomic<int>& counter;
        bool released = false;
        ClientCounterGuard(std::atomic<int>& c) : counter(c) { ++counter; }
        void release() { if (!released) { --counter; released = true; } }
        ~ClientCounterGuard() { release(); }
    };
    ClientCounterGuard counterGuard{owner_.connectedClients_};
    ...
    // In destructor or signalExit, call counterGuard.release()
};
```

---

### H-4: Port Availability TOCTOU Race Condition
**File:** `InstanceRegistry.cpp` (lines 110–118)  
**Severity:** High — Race Condition

```cpp
bool InstanceRegistry::isPortAvailable(int port) const
{
    juce::StreamingSocket loopbackProbe;
    if (!loopbackProbe.createListener(port, "127.0.0.1"))
        return false;
    loopbackProbe.close();
    return true;
}
```

Between `isPortAvailable()` returning `true` and `MCPServer::createServerListener()` actually binding the port, another process (or another plugin instance) could bind the port. This is a Time-of-Check to Time-of-Use (TOCTOU) race.

**Recommended Fix:** Remove the probe entirely. `findAvailablePort()` should attempt the bind directly in `MCPServer::createServerListener()`, and if it fails, move to the next port. The `InstanceRegistry` should track *intended* ports in the `instances_` map, and `MCPServer` should retry with the next candidate.

---

### H-5: `SO_REUSEADDR` Not Set — Rapid Restart Failures
**File:** `MCPServer.cpp` (line 287–289)  
**Severity:** High — Robustness

```cpp
bool MCPServer::createServerListener()
{
    return serverSocket_.createListener(port_, "127.0.0.1");
}
```

JUCE’s `StreamingSocket::createListener()` does not set `SO_REUSEADDR` by default. After the plugin is unloaded, the TCP port may remain in `TIME_WAIT` state for 2–4 minutes. Reloading the plugin (or creating a new instance) can fail to bind with `WSAEADDRINUSE` / `EADDRINUSE`.

**Recommended Fix:** Wrap the socket creation to set `SO_REUSEADDR` before `bind()`:
```cpp
bool MCPServer::createServerListener()
{
    // After createListener, if it fails with EADDRINUSE, try SO_REUSEADDR
    // Or use a raw socket with setsockopt(SO_REUSEADDR, 1) before bind.
}
```
> Note: JUCE may not expose this directly. A platform-specific patch or upstream JUCE modification may be needed.

---

### H-6: `processRequest()` Re-Parses Large Tool Results — Performance & OOM Risk
**File:** `MCPServer.cpp` (lines 403–406)  
**Severity:** High — Performance / DoS

```cpp
json resultEmbedded;
try {
    resultEmbedded = json::parse(toolResult.toStdString());
}
```

For tools like `list_parameters` with 2000+ hosted plugin parameters, `toolResult` can be a ~200 KB JSON string. The server re-parses this entire string into `nlohmann::json` just to wrap it in an envelope, then re-serializes. This is O(n) memory and CPU overhead on every call. A malicious or buggy client making rapid `list_parameters` calls can cause excessive heap churn on the MCP thread.

**Recommended Fix:** Use `nlohmann::json::parse()` with a custom `json::json_sax_t` callback that validates structure without full DOM construction, or accept that the tool handler already produces valid JSON and splice it directly:
```cpp
// Unsafe but fast: string concatenation (only if toolResult is guaranteed valid JSON)
std::string envelope = "{\"jsonrpc\":\"2.0\",\"result\":" + toolResult.toStdString()
                     + ",\"id\":" + reqId.dump() + "}";
return juce::String(envelope);
```
Alternatively, pre-allocate the `nlohmann::json` result buffer in the tool handler to avoid `juce::String` → `std::string` → `nlohmann::json` conversion.

---

## Medium-Priority Refinements

### M-1: `juce::JSON::parse` Used for Incoming, `nlohmann::json` for Outgoing — Dialect Risk
**File:** `MCPServer.cpp` (line 295)  
**Severity:** Medium — Protocol Compliance

`juce::JSON::parse` and `nlohmann::json::parse` have different edge-case behaviors (e.g., handling of `NaN`, `Infinity`, duplicate keys, large integers, Unicode escapes). A client sending `{"id": 1, "method": "tools/list", "params": null, "id": 2}` might be parsed differently by the two libraries. This could cause request/response `id` mismatches or security issues if the JUCE parser accepts malformed JSON that `nlohmann::json` rejects (or vice versa).

**Recommended Fix:** Standardize on `nlohmann::json` for **all** JSON parsing and serialization in the MCP server layer. Convert `nlohmann::json` to `juce::var` only when crossing into the JUCE-heavy tool handler layer.

---

### M-2: `MCPServer::processRequest` Does Not Validate `jsonrpc` Version Field
**File:** `MCPServer.cpp` (line 295–304)  
**Severity:** Medium — Protocol Compliance

The request object is never checked for `"jsonrpc": "2.0"`. A client sending a JSON-RPC 1.0 request (or a plain JSON object) will be processed as if it were 2.0. This can lead to subtle interoperability bugs.

**Recommended Fix:**
```cpp
auto version = parsed.getProperty("jsonrpc", "").toString();
if (version != "2.0")
    return errResponse(-32600, "Invalid Request: jsonrpc must be 2.0");
```

---

### M-3: `catch (...)` in `ConnectionThread::run` May Throw on Write
**File:** `MCPServer.cpp` (lines 173–184)  
**Severity:** Medium — Crash Vector

```cpp
catch (...)
{
    owner_.logError("connection", "Unhandled exception in connection thread");
    const auto response = juce::String(json{...}.dump()) + "\n";
    if (socket_ != nullptr && socket_->isConnected())
        socket_->write(response.toRawUTF8(), ...);
    break;
}
```

If the socket is in a bad state (e.g., already closed by the peer), `socket_->isConnected()` may return `true` but `write()` may throw an OS-level exception, causing a nested throw inside `catch (...)`, which calls `std::terminate()` and crashes the host DAW.

**Recommended Fix:** Wrap the socket write in a nested `try/catch`:
```cpp
catch (...) {
    try {
        if (socket_ && socket_->isConnected())
            socket_->write(...);
    } catch (...) {
        // Swallow — we are already in recovery mode
    }
    break;
}
```

---

### M-4: `LockFreeQueue` `juce::SpinLock` Priority Inversion Risk
**File:** `LockFreeQueue.h` (line 111)  
**Severity:** Medium — Real-Time Safety

The `pushMutex_` is a `juce::SpinLock` (busy-wait). If the MCP thread (low priority) is preempted while holding the spinlock, the UI thread (higher priority) will spin-wait, wasting CPU. While the critical section is small, in a DAW with heavy load, this can cause stuttering or dropouts.

**Recommended Fix:** Replace with `std::mutex` (kernel transition is acceptable here since the MCP/UI threads are not real-time) or a `juce::CriticalSection` with `tryEnter()` fallback. The comment says "SpinLock avoids kernel transition (priority inversion)" but this is incorrect—spinlocks **amplify** priority inversion.

---

### M-5: `TokenOptimizer` Rate Limiting Is Global, Not Per-Client
**File:** `MCPServer.cpp` (line 358), `TokenOptimizer.cpp` (lines 420–428)  
**Severity:** Medium — DoS Resilience

The rate limiter (`MAX 60 requests/minute`) is a single global counter per `TokenOptimizer` instance. A misbehaving client can exhaust the budget, blocking legitimate clients. The MCP server supports up to 4 concurrent connections (`MAX_CONNECTIONS`), but the rate limiter does not track per-client IP or connection ID.

**Recommended Fix:** Add per-connection rate tracking, or at least exempt read-only methods (`tools/list`, `get_plugin_info`) from the rate limit.

---

### M-6: `InstanceRegistry` Static Destruction Order Hazard
**File:** `InstanceRegistry.h` (lines 19–23)  
**Severity:** Medium — Crash on Exit

The Meyers singleton `InstanceRegistry` holds `juce::String` objects in a `std::map`. At program exit, the static destructor order is undefined. If JUCE’s `String` heap or internal globals are destroyed before `InstanceRegistry`, destructing `juce::String` members may cause use-after-free crashes. This is a known JUCE plugin pitfall.

**Recommended Fix:** Add a `shutdown()` method that clears `instances_` during `MorePhiProcessor` destruction, before JUCE is torn down. Alternatively, store raw `char[]` / `int` port data instead of `juce::String` in the registry.

---

### M-7: `StandaloneMcpServer` Lacks Authentication
**File:** `StandaloneMcpServer.h` / `StandaloneMcpServer.cpp`  
**Severity:** Medium — Security

The standalone MCP server (`main()`) reads from `std::cin` and writes to `std::cout`. There is no bearer token, no `initialize` handshake validation, and no client identity check. Any process with access to the stdio pipes can invoke tools. While this is intended for local IPC, the security model is inconsistent with the embedded server.

**Recommended Fix:** Add a lightweight `X-Auth-Token` header or require the `initialize` method to carry a secret key. Document the threat model clearly.

---

### M-8: `MCPToolHandler::renderMasteringBatch` Spawns Unnamed Detached Thread
**File:** `MCPToolHandler.cpp` (lines 4381–4493)  
**Severity:** Medium — Observability / Debugging

```cpp
std::thread([...] { ... }).detach();
```

The detached render thread has no name, making it invisible to debuggers and profilers. It also cannot be gracefully joined or cancelled.

**Recommended Fix:** Use `juce::Thread` with a named identifier (`"MorePhi-OfflineRender"`) and store a `std::unique_ptr<juce::Thread>` so it can be signaled and waited upon.

---

## Low-Priority Enhancements

### L-1: `TokenOptimizer::estimateTokensInString` is Overly Simplistic
**File:** `TokenOptimizer.cpp` (line 570–574)  
**Severity:** Low — Accuracy

```cpp
uint32_t TokenOptimizer::estimateTokensInString(const std::string& text) const
{
    return static_cast<uint32_t>(text.length() / 4 + 1);
}
```

A 4-char-per-token heuristic is inaccurate for many LLM tokenizers (e.g., GPT-4 uses ~3.5 chars/English token, but ~1 char/Chinese token). Budget warnings may be misleading for non-ASCII parameter names.

**Recommended Fix:** Use a lightweight tokenizer approximation (e.g., `cl100k_base` byte-pair encoding regex) or at least document the heuristic’s inaccuracy.

---

### L-2: `MCPServer::processRequest` Error Code `-32600` Used for Auth Failures
**File:** `MCPServer.cpp` (lines 350, 355)  
**Severity:** Low — Protocol Compliance

`-32600` is "Invalid Request" in JSON-RPC 2.0. Using it for authentication failures is semantically incorrect. While there is no standard JSON-RPC auth error code, the project should use a custom code (e.g., `-32001` for "Unauthorized") to avoid confusing generic JSON-RPC clients.

**Recommended Fix:**
```cpp
static constexpr int kErrorUnauthorized = -32001;
return errResponse(kErrorUnauthorized, "Unauthorized: invalid bearer_token");
```

---

### L-3: `generateSecureRandomHexString(16)` Produces 128-Bit Token
**File:** `InstanceIdentity.h` (line 123)  
**Severity:** Low — Security Margin

```cpp
id.bearerToken = generateSecureRandomHexString(16); // 16 bytes = 128 bits
```

128 bits of entropy is sufficient for a local-only service with 60-second token lifetime. However, if the token is ever logged or exposed through `listInstances` (which intentionally redacts it), the entropy margin is thin. Increasing to 32 bytes (256 bits) is trivial and adds long-term safety margin.

**Recommended Fix:**
```cpp
id.bearerToken = generateSecureRandomHexString(32); // 256 bits
```

---

### L-4: `MCPToolHandler.cpp` is 5507 Lines — Excessive for a Single File
**File:** `MCPToolHandler.cpp`  
**Severity:** Low — Maintainability

At 5507 lines, `MCPToolHandler.cpp` is a monolith. It contains tool dispatch, diagnostic test suites, Ozone mastering logic, iZotope IPC wrappers, dataset generation helpers, and multi-instance tools. Compilation times, merge conflicts, and code navigation are all negatively impacted.

**Recommended Fix:** Split into `MCPToolHandlerCore.cpp`, `MCPToolHandlerMastering.cpp`, `MCPToolHandlerDiagnostics.cpp`, `MCPToolHandlerIPC.cpp`.

---

### L-5: `processRequest` Logs Full Error Details in Release Builds
**File:** `MCPServer.cpp` (lines 386–391)  
**Severity:** Low — Information Disclosure

```cpp
#ifdef NDEBUG
return errResponse(-32603, "Internal error");
#else
return errResponse(-32603, std::string("Internal error: ") + e.what());
#endif
```

In debug builds, exception details (`e.what()`) are leaked to the client. This is acceptable for debug, but ensure that no production build is accidentally compiled with `NDEBUG` undefined.

---

### L-6: Missing Documentation for Custom `\n` Message Framing
**File:** `MCPServer.cpp` (line 347)  
**Severity:** Low — Documentation

The server returns two newline-separated JSON objects for `initialize`:
```cpp
return juce::String(initResponse + "\n" + initNotification);
```

This is not standard JSON-RPC (which uses a single response per request, or HTTP-style Content-Length headers). The `ConnectionThread` then appends another `\n` (line 157), resulting in three newlines. Document this custom framing protocol explicitly for third-party client authors.

---

## Recommended Fixes Summary Table

| ID | File | Line | Issue | Fix Priority |
|----|------|------|-------|-------------|
| C-1 | `MCPServer.cpp` | 459–476 | Timing side-channel in auth | Critical |
| C-2 | `MCPServer.cpp` | 304–347 | Notifications return responses | Critical |
| C-3 | `TokenOptimizer.cpp` | 127–140 | Deadlock (lock-order inversion) | Critical |
| H-1 | `MCPServer.cpp` | 295 | No batch request support | High |
| H-2 | `MCPServer.cpp` | 57 | `startThread()` failure ignored | High |
| H-3 | `MCPServer.cpp` | 81–186 | `connectedClients_` leak on forced stop | High |
| H-4 | `InstanceRegistry.cpp` | 110–118 | Port availability TOCTOU race | High |
| H-5 | `MCPServer.cpp` | 287–289 | `SO_REUSEADDR` missing | High |
| H-6 | `MCPServer.cpp` | 403–406 | Re-parse of large tool results | High |
| M-1 | `MCPServer.cpp` | 295 | Mixed JSON parser dialects | Medium |
| M-2 | `MCPServer.cpp` | 295–304 | `jsonrpc` version not validated | Medium |
| M-3 | `MCPServer.cpp` | 173–184 | Nested throw in `catch(...)` | Medium |
| M-4 | `LockFreeQueue.h` | 111 | SpinLock priority inversion | Medium |
| M-5 | `TokenOptimizer.cpp` | 420–428 | Global rate limiter | Medium |
| M-6 | `InstanceRegistry.h` | 19–23 | Static destruction order | Medium |
| M-7 | `StandaloneMcpServer.cpp` | — | No auth on standalone | Medium |
| M-8 | `MCPToolHandler.cpp` | 4381 | Unnamed detached thread | Medium |
| L-1 | `TokenOptimizer.cpp` | 570–574 | Token estimation heuristic | Low |
| L-2 | `MCPServer.cpp` | 350, 355 | Wrong error code for auth | Low |
| L-3 | `InstanceIdentity.h` | 123 | 128-bit token entropy | Low |
| L-4 | `MCPToolHandler.cpp` | — | 5507-line monolith | Low |
| L-5 | `MCPServer.cpp` | 386–391 | Debug error leak | Low |
| L-6 | `MCPServer.cpp` | 347 | Custom framing undocumented | Low |

---

## Positive Findings

1. **Localhost-only binding** (`127.0.0.1`) with `isLocal()` check (line 256) is a strong defense-in-depth measure against remote exploitation.
2. **Request size limit** (`MAX_REQUEST_BYTES = 256 KB`) prevents trivial memory exhaustion (line 118).
3. **Connection limit** (`MAX_CONNECTIONS = 4`) prevents resource exhaustion (line 265).
4. **LockFreeQueue** correctly uses cache-line-aligned atomics and a spinlock for multi-producer safety; the consumer (audio thread) is truly lock-free.
5. **CSPRNG token generation** (`BCryptGenRandom`, `SecRandomCopyBytes`, `getrandom()`) is cryptographically sound (line 40–80 of `InstanceIdentity.h`).
6. **Token zeroization** (`SecureZeroMemory` / `explicit_bzero`) is present (line 144–161 of `InstanceIdentity.h`).
7. **NaN/Inf validation** (`MCP-PARAMS-01`) on parameter values prevents undefined behavior in hosted plugins (line 2789–2791, 513 of `MCPToolHandler.cpp`).
8. **Parameter command queue** for all write operations ensures thread safety between MCP/UI threads and the audio thread.

---

## Conclusion

The More-Phi MCP server is a capable and generally secure local control interface, but it has **critical gaps in JSON-RPC compliance, authentication timing safety, and thread synchronization** that must be addressed before production hardening. The **three critical issues (C-1, C-2, C-3)** should be fixed immediately. The **six high-priority issues (H-1 through H-6)** should be resolved in the next sprint. Medium and low items can be triaged based on customer impact and roadmap priorities.

**No source files were modified during this audit.**

---
*End of Report*
