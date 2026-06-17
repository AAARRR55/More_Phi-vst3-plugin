# Sub-Agent 4 Report: MCP Server & Communication Protocol

## Audit Scope
- **Files reviewed**: 16 source files (~14,000 lines of code)
- **Focus areas**: JSON-RPC 2.0 compliance, authentication, port management, connection security, error handling, serialization performance, thread safety, InstanceRegistry lifecycle
- **Date**: 2026-01-15

---

## Critical Issues (SECURITY / CRASH / DATA LOSS)

### Issue 1: Multi-Instance Data Isolation Broken by Global Singletons
- **Location**: `MCPToolHandler.cpp:61-65`, `MCPToolHandler.cpp:1425-1434`, `MCPToolHandler.cpp:1429-1431`
- **Severity**: Critical
- **Description**: The `AutomationRuntime` is a function-local static singleton shared by all plugin instances in the same process. Additionally, `gSafeActionSnapshots`, `gDryRunCandidates`, and `gRenderJobs` are global static variables. This means multiple More-Phi instances in the same DAW share automation history, permission state, memory records, workflow runs, render jobs, and safe-action snapshots. Instance A can read/approve Instance B's pending approvals, restore Instance B's parameter snapshots, and poll Instance B's render jobs.
- **Root Cause**: All instances call `automationRuntime()` which returns a single `static AutomationRuntime` object. The global caches have no `instanceId` prefix or namespace isolation.
- **Recommended Fix**: 
  1. Make `AutomationRuntime` instance-scoped (owned by `MorePhiProcessor`) rather than process-global.
  2. Prefix all global cache keys with `identity.instanceId + ":"` (e.g., `"instance_abc123:render_42"`).
  3. Add `instanceId` filter to all `list`, `search`, `get`, and `restore` operations so they only return records belonging to the caller.

### Issue 2: Flawed "Constant-Time" Token Comparison Leaks Timing Information
- **Location**: `MCPServer.cpp:461-476`
- **Severity**: Critical
- **Description**: The `validateAuth()` function implements a hand-rolled constant-time comparison using `volatile uint8_t`. This is NOT constant-time in practice. The loop bound `std::max(candidate.size(), expected.size())` leaks the expected token length: an attacker can binary-search the length by sending candidates of varying lengths and measuring response time. Furthermore, modern C++ compilers optimize `volatile` arithmetic; the OR accumulation does not prevent branch prediction or instruction reordering that leaks byte-by-byte differences.
- **Root Cause**: `volatile` is not a cryptographic constant-time barrier. The comparison does not use a hardened constant-time primitive.
- **Recommended Fix**: Replace with a proper constant-time comparison (e.g., OpenSSL's `CRYPTO_memcmp`, libsodium's `sodium_memcmp`, or a hand-rolled loop with `__asm__`/`__attribute__((noinline))` and no early-exit):
  ```cpp
  // Pseudocode: true constant-time compare
  inline bool secureTimingSafeCompare(const std::string& a, const std::string& b) {
      if (a.size() != b.size()) return false; // length leak is unavoidable here; pad both to fixed length first
      volatile uint8_t diff = 0;
      for (size_t i = 0; i < a.size(); ++i) {
          diff |= static_cast<uint8_t>(a[i] ^ b[i]);
      }
      return diff == 0;
  }
  ```
  Better yet, hash the token with HMAC-SHA256 and compare the fixed-length digests.

### Issue 3: No Connection Idle Timeout Enables Connection Exhaustion DoS
- **Location**: `MCPServer.cpp:79-187` (ConnectionThread::run)
- **Severity**: Critical
- **Description**: A malicious client can open 4 TCP connections (the `MAX_CONNECTIONS` limit) and send no data. Each `ConnectionThread` loops forever with `socket_->waitUntilReady(true, 500)`, waking every 500ms to check `threadShouldExit()`. Since there is no idle timeout, these connections are held indefinitely. All subsequent legitimate connections are rejected with "max connections reached". This is a trivial DoS.
- **Root Cause**: No `lastActivity` timestamp or idle timer. The `MAX_CONNECTIONS` limit is the only defense, and it is per-process, not per-source.
- **Recommended Fix**: Add an idle timeout (e.g., 30 seconds) and a max-lifetime (e.g., 5 minutes). Track `lastActivityMs` in `ConnectionThread` and break the loop if exceeded:
  ```cpp
  // In ConnectionThread::run
  int idleMs = 0;
  while (!threadShouldExit() && !writeError) {
      const int ready = socket_->waitUntilReady(true, 500);
      if (ready == 0) { idleMs += 500; if (idleMs >= 30000) break; continue; }
      idleMs = 0;
      // ... rest of loop
  }
  ```

### Issue 4: Zombie Instance Entries Accumulate and Exhaust Port Pool
- **Location**: `InstanceRegistry.cpp:31-42`, `InstanceRegistry.cpp:72-108`
- **Severity**: Critical
- **Description**: If a More-Phi plugin instance crashes or is force-killed without calling `deregisterInstance()`, its port remains reserved in the `InstanceRegistry` forever (the registry is process-global and lives until the DAW exits). The `findAvailablePort()` loop skips any port already in `instances_`, even if the owning instance is dead. With `MAX_INSTANCES = 64`, after 64 crashes the port pool is exhausted and new instances cannot register.
- **Root Cause**: No heartbeat, TTL, or automatic expiration. `isPortAvailable()` probes the OS but the result is ignored if the port is registered.
- **Recommended Fix**: Add a `createdAt` timestamp and a TTL (e.g., 5 minutes). In `findAvailablePort()`, if a registered entry is older than the TTL and `isPortAvailable(port)` returns true (the OS port is free), evict the zombie entry and reuse the port. Alternatively, call `isPortAvailable()` on all registered ports during `registerInstance()` and evict any whose OS port is no longer in use.

### Issue 5: No TLS / Plaintext Token Transmission on Localhost
- **Location**: `MCPServer.cpp:289`, `MCPServer.cpp:324-351`
- **Severity**: Critical
- **Description**: The MCP server binds to `127.0.0.1:port` with no TLS. The bearer token is transmitted in plaintext inside the JSON-RPC `initialize` request. Any other process on the same machine with sufficient privileges (or even unprivileged processes using raw sockets on some platforms) can sniff the token off the loopback interface. Once captured, the token grants full parameter control.
- **Root Cause**: `juce::StreamingSocket` does not support TLS. The design assumes localhost is a trust boundary.
- **Recommended Fix**: 
  1. Short-term: Rotate tokens periodically (e.g., every 60 seconds) and require the client to re-initialize. This limits the window of a stolen token.
  2. Long-term: Replace `juce::StreamingSocket` with a TLS-enabled transport (e.g., OpenSSL wrapper, or JUCE's `OpenSSLContext` if available). At minimum, implement a Diffie-Hellman key exchange over the plaintext socket to establish a session key for subsequent messages.

---

## High-Priority Issues (THREAD-SAFETY / PROTOCOL / ROBUSTNESS)

### Issue 6: JSON-RPC 2.0 Batch Requests Not Supported
- **Location**: `MCPServer.cpp:129-171` (ConnectionThread::run message loop)
- **Severity**: High
- **Description**: JSON-RPC 2.0 spec §6 defines batch requests as a JSON array of request objects. The `ConnectionThread` message extraction logic treats any valid JSON without a newline as a single message. If a client sends a batch request (array), `juce::JSON::parse()` parses it as an array, but `processRequest()` extracts `method` via `getProperty("method", "")` which returns empty string for arrays, causing the request to fall through to `dispatchTool()` and return `{"error":"unknown_method"}`. The server never returns a batch response array.
- **Root Cause**: Message framing assumes single objects. No array detection or batch dispatch logic.
- **Recommended Fix**: In `processRequest()`, detect if `parsed` is an array. If so, iterate over elements, process each individually, collect responses, and return them as a JSON array. Skip notification entries (no `id`) in the response array.

### Issue 7: JSON-RPC `id` Handling Incomplete (Float IDs Lost)
- **Location**: `MCPServer.cpp:306-313`
- **Severity**: High
- **Description**: JSON-RPC 2.0 allows `id` to be any Number. The code only handles `isInt()`/`isInt64()` and `isString()`. If a client sends `"id": 1.5` (a float), it falls through and `reqId` becomes `nullptr`. The response will contain `id: null`, violating the spec and confusing clients that expect their original `id` echoed back.
- **Root Cause**: Missing float/double branch in `id` conversion.
- **Recommended Fix**: Add a branch for `idVar.isDouble()`:
  ```cpp
  else if (idVar.isDouble())
      reqId = static_cast<double>(idVar);
  ```

### Issue 8: `processRequest()` Does Not Validate `jsonrpc` Field
- **Location**: `MCPServer.cpp:292`, `StandaloneMcpServer.cpp:332-350`
- **Severity**: High
- **Description**: Neither the embedded `MCPServer` nor the `StandaloneMcpServer` checks that the request contains `"jsonrpc":"2.0"`. Any JSON object with a `method` field is accepted as a valid JSON-RPC request. This could lead to non-JSON-RPC clients being treated as valid MCP clients.
- **Root Cause**: No validation of the `jsonrpc` version field.
- **Recommended Fix**: Reject requests where `parsed.getProperty("jsonrpc", "").toString()` is not `"2.0"`, returning `-32600` (Invalid Request).

### Issue 9: Connection Thread Use-After-Free Risk During Shutdown
- **Location**: `MCPServer.cpp:189-210`, `MCPServer.cpp:54-71`
- **Severity**: High
- **Description**: `stopServer()` calls `stopThread(500)` on the main thread, then immediately clears `activeConnections_`. If a `ConnectionThread` is still executing `owner_.processRequest()` (which can take arbitrary time for heavy tools like `renderMasteringBatch`), the destructor (`~ConnectionThread()`) runs while the thread is active. The destructor calls `stopThread(500)` and then `socket_->close()`. If the thread is still inside `owner_.processRequest()`, it accesses `owner_` (a reference to the being-destroyed `MCPServer`). This is a use-after-free.
- **Root Cause**: `clear()` does not wait for all threads to finish before destroying them. The 500ms timeout is too short for long-running tool dispatches.
- **Recommended Fix**: In `stopServer()`, before `activeConnections_.clear()`, wait for all connection threads to finish. Use a longer timeout (e.g., 5000ms) or, better, signal threads to exit and then join them in a loop until `activeConnections_` is empty. Also, `ConnectionThread` should hold a `std::weak_ptr` or raw pointer with a shutdown flag rather than a direct reference, but the simplest fix is ensuring all threads are stopped before clearing the array.

### Issue 10: No Token Expiration or Replay Protection
- **Location**: `MCPServer.cpp:449-483`
- **Severity**: High
- **Description**: Once a connection passes `validateAuth()`, the `authenticated_` boolean is set to `true` for the entire connection lifetime. There is no session timeout, no nonce in requests, no request sequencing, and no replay protection. An attacker who captures a valid `initialize` request can replay it on a new connection at any time. The token itself is static for the instance lifetime (no rotation).
- **Root Cause**: Single `bearer_token` check per connection, no session state machine.
- **Recommended Fix**: 
  1. Add a per-connection session nonce (returned in `initialize` response) that must be included in every subsequent request.
  2. Implement a token rotation mechanism: after N requests or M minutes, require re-initialization with a new token.
  3. Add a monotonic request sequence number and reject out-of-order or duplicate requests.

### Issue 11: `isLocal()` Check May Not Be Reliable on All Platforms
- **Location**: `MCPServer.cpp:256-260`
- **Severity**: High
- **Description**: The server rejects non-local connections using `client->isLocal()`. JUCE's implementation of `isLocal()` checks the peer address against loopback ranges. On some platforms (Windows with certain network configurations, IPv6-mapped IPv4), this may not correctly identify all local connections or may incorrectly reject valid local connections. More importantly, it does not prevent connections from other users on the same machine (multi-user systems).
- **Root Cause**: Platform-dependent `isLocal()` implementation; no secondary defense (e.g., Unix domain sockets, named pipes, or `SO_PEERCRED`).
- **Recommended Fix**: 
  1. Add a fallback check: parse `getPeerAddress()` and explicitly reject anything not in `127.0.0.0/8` or `::1/128`.
  2. On Unix, use `SO_PEERCRED` to verify the connecting process UID matches the server UID.
  3. On Windows, use `GetNamedPipeClientProcessId` or compare against the DAW process ID.

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 12: Double Embedding of Tool Results in `tools/call` Response Wastes Bandwidth
- **Location**: `MCPServer.cpp:424-443`
- **Severity**: Medium
- **Description**: For `tools/call`, the response embeds the tool result twice: once as a raw JSON string inside `content[0].text`, and again as a parsed object inside `structuredContent`. For large results (e.g., `list_parameters` with 1000+ parameters), this doubles the response size. Clients that only need `structuredContent` still receive the `text` dump, and vice versa.
- **Root Cause**: MCP protocol requires both `content` (text) and `structuredContent` (object). The implementation is compliant but inefficient for large payloads.
- **Recommended Fix**: For results above a size threshold (e.g., 4KB), omit the `text` field or replace it with a summary/truncation marker. The client can still read `structuredContent`.

### Issue 13: `juce::String` Buffer May Corrupt Non-UTF8 Input
- **Location**: `MCPServer.cpp:116-145`
- **Severity**: Medium
- **Description**: `ConnectionThread::run()` accumulates received bytes into a `juce::String` using `juce::String::fromUTF8(chunk, bytesRead)`. If a malicious client sends bytes that are not valid UTF-8, JUCE may replace them with replacement characters or truncate. This could corrupt the JSON-RPC framing, causing parse errors or, in edge cases, causing the wrong message boundaries to be detected.
- **Root Cause**: `juce::String` is a UTF-16/UTF-8 string type; it is not a byte buffer.
- **Recommended Fix**: Accumulate raw bytes in a `std::vector<char>` or `std::string`, then parse the JSON from the byte buffer directly. Only convert to `juce::String` after successful JSON parsing if needed.

### Issue 14: `pushRange()` Spinlock Hold Time Unbounded for Large Batches
- **Location**: `LockFreeQueue.h:66-88`
- **Severity**: Medium
- **Description**: `pushRange()` acquires the `pushMutex_` (a `juce::SpinLock`) and iterates over the entire input range. If a batch of 8191 `ParamCommand` items is enqueued, the spinlock is held while all items are copied. On a system with high contention between the MCP thread and the UI thread, the other producer could spin for a long time. The audio thread is unaffected (it does not acquire the spinlock), but MCP thread latency increases.
- **Root Cause**: `pushRange()` copies all items under the lock rather than pre-allocating and doing a bulk copy.
- **Recommended Fix**: Pre-compute the write range under the lock, then copy items outside the lock, or break large batches into smaller chunks (e.g., 512 items at a time).

### Issue 15: `processRequest` Test-Parse Fallback Can Process Partial JSON
- **Location**: `MCPServer.cpp:129-145`
- **Severity**: Medium
- **Description**: If a message does not contain a newline, the code does `juce::JSON::parse(buffer)` and if it succeeds, treats the entire buffer as a single message. If a client sends a large batch request that starts with a valid JSON object (e.g., the first request in the batch) but the rest of the array is not yet received, the code might process the partial prefix as a complete message. In practice, `juce::JSON::parse()` on an incomplete array would fail, but for a complete object embedded in a larger stream, it could succeed and truncate.
- **Root Cause**: No length prefix or explicit message framing beyond newline.
- **Recommended Fix**: Switch to length-prefixed framing (e.g., `Content-Length: N` header followed by N bytes), which is standard for MCP/JSON-RPC over TCP and avoids ambiguity with newlines inside JSON strings.

### Issue 16: `MCPToolHandler::handle` Returns Tool Errors Inside JSON-RPC Result
- **Location**: `MCPToolHandler.cpp:2686`
- **Severity**: Medium
- **Description**: For unknown methods, `MCPToolHandler::handle` returns `toJString(json{{"error","unknown_method"}})`. This is embedded by `MCPServer::processRequest` as the JSON-RPC `result` field, not as a JSON-RPC `error`. The client receives `{"jsonrpc":"2.0","result":{"error":"unknown_method"},"id":...}` instead of `{"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found"},"id":...}`. This is a JSON-RPC compliance violation.
- **Root Cause**: Tool dispatch returns a successful JSON-RPC envelope with an error payload inside.
- **Recommended Fix**: `MCPToolHandler::handle` should throw a `std::runtime_error` with the error code, or return a structured error that `processRequest` can convert into a proper JSON-RPC error response. Alternatively, add a special return marker (e.g., `{"__jsonrpc_error__": ...}`) that `processRequest` detects and unwraps.

### Issue 17: `extractParamId` Does Not Handle String Identifiers
- **Location**: `MCPToolHandler.cpp:2079-2089`
- **Severity**: Medium
- **Description**: `extractParamId` only looks for `"id"` or `"index"` properties as integer keys. It does not handle `"stableId"` (string) or `"name"` (string), which are valid parameter identifiers in the `setParameter`/`getParameter` tools. This function is used in `diagnoseParameterPipeline`, so diagnostic output for string-identified parameters is incorrect.
- **Root Cause**: `extractParamId` was designed for numeric indices only.
- **Recommended Fix**: Make `extractParamId` also accept `"stableId"` and `"name"` string properties, or rename it to `extractParamIndex` to clarify its limited scope.

---

## Low-Priority Issues (STYLE / DOCUMENTATION / OPTIMIZATION)

### Issue 18: `MAX_REQUEST_BYTES` Check Uses Expensive `getNumBytesAsUTF8()`
- **Location**: `MCPServer.cpp:118`
- **Severity**: Low
- **Description**: After every chunk read, `buffer.getNumBytesAsUTF8()` is called to check against the 256KB limit. For a `juce::String`, this traverses the entire string to count UTF-8 bytes, which is O(n) per chunk. For a slow client sending 1KB chunks up to the 256KB limit, this is ~256 string traversals.
- **Root Cause**: `juce::String` does not cache its UTF-8 byte count.
- **Recommended Fix**: Maintain a running total of `bytesRead` (the raw byte count) instead of computing the UTF-8 length. The limit is about bytes, not characters.

### Issue 19: `InstanceRegistry` Port Search Inefficient
- **Location**: `InstanceRegistry.cpp:72-108`
- **Severity**: Low
- **Description**: `findAvailablePort()` iterates over all registered instances for every candidate port in the range. With `MAX_INSTANCES = 64`, this is O(64*64) = 4096 operations worst case, which is trivial. However, if the fallback range (49152-65535) is searched, it becomes O(16384*64) which is over 1 million map lookups.
- **Root Cause**: `isRegisteredPortInUse` is a linear scan over the `std::map`.
- **Recommended Fix**: Maintain a secondary `std::unordered_set<int>` of used ports for O(1) lookup.

### Issue 20: `StandaloneMcpServer` Does Not Validate `jsonrpc` Field
- **Location**: `StandaloneMcpServer.cpp:332-350`
- **Severity**: Low
- **Description**: Same as Issue 8 but for the standalone server. The standalone server does not check for `"jsonrpc":"2.0"`.
- **Recommended Fix**: Add the same validation as recommended for Issue 8.

### Issue 21: `generateSecureRandomHexString` Throws on CSPRNG Failure
- **Location**: `InstanceIdentity.h:84-88`
- **Severity**: Low
- **Description**: If `BCryptGenRandom`, `SecRandomCopyBytes`, or `getrandom()` fails, `generateSecureRandomHexString` throws `std::runtime_error`. If this happens during `InstanceIdentity::generate()`, the exception propagates up and may crash the plugin if not caught.
- **Root Cause**: No fallback or graceful degradation.
- **Recommended Fix**: Catch the exception in `registerInstance()` and return an empty `InstanceIdentity` (which causes `startServer` to fail gracefully) rather than crashing.

### Issue 22: LockFreeQueue Capacity Documentation Mismatch
- **Location**: `LockFreeQueue.h:33-37`
- **Severity**: Low
- **Description**: `usableCapacity()` returns `Capacity - 1`. The comment says "Ring buffers that reserve one slot can store Capacity - 1 elements." This is correct, but external callers (e.g., `enqueueParameterBatch`) may assume the full 8192 capacity. The `pushRange()` check `if (count > usableCapacity() - used) return false;` correctly rejects oversized batches, but the `MAX_PARAMETERS = 2048` means 8191 is plenty.
- **Root Cause**: No action needed; this is a documentation/awareness issue.
- **Recommended Fix**: Add a static assertion in `MorePhiProcessor` that `LockFreeQueue usableCapacity >= MAX_PARAMETERS`.

---

## Positive Findings (What Is Done Well)

1. **Cryptographically Secure Token Generation**: `InstanceIdentity` uses platform-specific CSPRNGs (`BCryptGenRandom`, `SecRandomCopyBytes`, `getrandom()`) with a 16-byte (128-bit entropy) bearer token. This is a strong foundation.

2. **Rate Limiting via TokenOptimizer**: `MCPServer::processRequest` calls `tryConsumeRequestSlot()` before dispatching authenticated tool calls. Tests verify that requests beyond the limit return `-32000` (Rate limit exceeded). This mitigates brute-force token guessing and DoS.

3. **Permission System with Approval Workflow**: `dispatchWithAutomationTransaction()` integrates with `PermissionKernel` to require user approval for high-risk operations (e.g., `hosted_plugin.load`). The approval workflow includes predicted diffs, audit logging, and transaction history. This is a robust security model.

4. **Proper Lock-Free Queue Memory Ordering**: `LockFreeQueue` uses `memory_order_release` on writes and `memory_order_acquire` on reads, with cache-line-aligned indices. This is a textbook-correct SPSC queue implementation.

5. **Request Size Limit**: `MAX_REQUEST_BYTES = 256KB` prevents memory exhaustion from malicious clients sending unbounded data.

6. **Connection Limit**: `MAX_CONNECTIONS = 4` prevents resource exhaustion (though per-IP limits would be better).

7. **Error Recovery with Bind Retry**: `MCPServer::run()` implements `MAX_BIND_ATTEMPTS` with `RECOVERY_DELAY_MS` backoff, making the server resilient to transient port conflicts.

8. **Comprehensive Test Coverage**: The test suite includes unit tests (`TestMCPServerUnit.cpp`, `TestStandaloneMcpServer.cpp`) and integration tests (`TestMCPIntegration.cpp`) covering auth, lifecycle, tool dispatch, parameter editing, rate limiting, workflow execution, permission approval, and Ozone IPC.

9. **Transaction Ledger and Rollback**: Every write operation is wrapped in `dispatchWithAutomationTransaction()`, which captures `beforeState` and `afterState` and records a rollback plan. The `automation.rollback` tool can reverse parameter changes through the safe queue.

10. **Non-Finite Value Validation**: `setParameter`, `setParametersBatch`, and `setMorphPosition` all reject `NaN` and `Inf` values before they reach the audio thread, preventing undefined behavior in the hosted plugin.

11. **Notification Framing**: The `initialize` response correctly frames the `notifications/initialized` message as a separate newline-delimited JSON-RPC message, compliant with MCP spec §3.2.

12. **Global Cache Size Caps**: `capGlobalCache()` evicts entries when `gRenderJobs`, `gDryRunCandidates`, or `gSafeActionSnapshots` exceed their limits, preventing unbounded memory growth.

13. **Standalone Server StdIO Interface**: `StandaloneMcpServer` implements a clean `processJson`/`processLine`/`run(std::istream&, std::ostream&)` interface that is easy to test and embed.

---

## Summary Statistics

| Severity | Count |
|----------|-------|
| Critical | 5 |
| High | 6 |
| Medium | 6 |
| Low | 5 |
| Positive | 13 |

**Most urgent fixes**: Issue 1 (multi-instance isolation), Issue 2 (timing leak), Issue 3 (connection exhaustion DoS), Issue 4 (zombie instances), and Issue 5 (plaintext tokens). These represent the largest attack surface and data integrity risks.
