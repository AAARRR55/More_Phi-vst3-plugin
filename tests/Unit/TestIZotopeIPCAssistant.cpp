// TestIZotopeIPCAssistant.cpp
// Deterministic unit tests — no live DAW or Ozone required.
// All tests use fake mutable segments via setFakeSegmentForTests.

#ifdef _WIN32
#  include <windows.h>
#endif
#include "AI/StandaloneMcp/IZotopeIPCAssistant.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace more_phi::standalone_mcp;

// ═══════════════════════════════════════════════════════════════════════════
// Test helpers
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t kMagic      = 0x495A4F54u;
static constexpr size_t   kSegSz      = 4u * 1024u * 1024u;
static constexpr size_t   kRingOff    = 512u;
static constexpr size_t   kRingSize   = kSegSz - kRingOff;
static constexpr size_t   kReadPtrOff = 256u;
static constexpr size_t   kWrtPtrOff  = 260u;
static constexpr size_t   kRegOff     = 264u;
static constexpr size_t   kEntrySize  = 16u;
static constexpr size_t   kFrmHdrSz   = 28u; // sizeof(FrameHeader)

static uint32_t readU32At(const std::vector<uint8_t>& seg, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, seg.data() + offset, 4);
    return value;
}

static void writeU32At(std::vector<uint8_t>& seg, size_t offset, uint32_t value)
{
    std::memcpy(seg.data() + offset, &value, 4);
}

// Build a default 4 MiB zero-filled segment
static std::vector<uint8_t> blankSeg() {
    return std::vector<uint8_t>(kSegSz, 0u);
}

// Write a plugin registry entry (slot 0 = Ozone by default)
static void writeRegistryEntry(std::vector<uint8_t>& seg,
                                uint32_t instanceId,
                                const char* nameTag,
                                size_t slot = 0)
{
    size_t off = kRegOff + slot * kEntrySize;
    std::memcpy(seg.data() + off, &instanceId, 4);
    uint32_t flags = 0x01u;
    std::memcpy(seg.data() + off + 4, &flags, 4);
    std::memset(seg.data() + off + 8, 0, 8);
    std::memcpy(seg.data() + off + 8, nameTag, std::min(strlen(nameTag), (size_t)8u));
}

// FrameHeader (must match the wire struct in the implementation)
#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t messageType;
    uint32_t senderId;
    uint32_t targetId;
    uint32_t payloadSize;
    uint64_t timestamp;
};
#pragma pack(pop)
static constexpr size_t kHdrSz = sizeof(FrameHeader);

// Write a frame at ring offset `ringPos` and advance the write pointer.
// Sets seg[kWrtPtrOff] to ringPos + hdrSz + payloadSize.
static void writeFrameAt(std::vector<uint8_t>& seg,
                          uint32_t ringPos,
                          uint16_t msgType,
                          uint32_t senderId,
                          uint32_t targetId,
                          const std::vector<uint8_t>& payload)
{
    FrameHeader hdr{};
    hdr.magic       = kMagic;
    hdr.version     = 3;
    hdr.messageType = msgType;
    hdr.senderId    = senderId;
    hdr.targetId    = targetId;
    hdr.payloadSize = static_cast<uint32_t>(payload.size());

    size_t dest = kRingOff + ringPos;
    std::memcpy(seg.data() + dest, &hdr, kHdrSz);
    if (!payload.empty())
        std::memcpy(seg.data() + dest + kHdrSz, payload.data(), payload.size());

    uint32_t newWp = ringPos + static_cast<uint32_t>(kHdrSz + payload.size());
    std::memcpy(seg.data() + kWrtPtrOff, &newWp, 4);
}

static void appendBytesAtCurrentWrite(std::vector<uint8_t>& seg,
                                       const std::vector<uint8_t>& bytes,
                                       size_t ringOff = kRingOff,
                                       size_t ringSize = kRingSize,
                                       size_t writePtrOff = kWrtPtrOff)
{
    const auto writePos = readU32At(seg, writePtrOff) % static_cast<uint32_t>(ringSize);
    for (size_t i = 0; i < bytes.size(); ++i)
        seg[ringOff + ((writePos + i) % ringSize)] = bytes[i];

    writeU32At(seg, writePtrOff, static_cast<uint32_t>((writePos + bytes.size()) % ringSize));
}

static void appendFrameAtCurrentWrite(std::vector<uint8_t>& seg,
                                       uint16_t msgType,
                                       uint32_t senderId,
                                       uint32_t targetId,
                                       const std::vector<uint8_t>& payload,
                                       size_t ringOff = kRingOff,
                                       size_t ringSize = kRingSize,
                                       size_t writePtrOff = kWrtPtrOff,
                                       uint32_t magic = kMagic)
{
    FrameHeader hdr{};
    hdr.magic       = magic;
    hdr.version     = 3;
    hdr.messageType = msgType;
    hdr.senderId    = senderId;
    hdr.targetId    = targetId;
    hdr.payloadSize = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> frame(kHdrSz + payload.size());
    std::memcpy(frame.data(), &hdr, kHdrSz);
    if (!payload.empty())
        std::memcpy(frame.data() + kHdrSz, payload.data(), payload.size());

    appendBytesAtCurrentWrite(seg, frame, ringOff, ringSize, writePtrOff);
}

template <typename Writer>
static ToolCallOutcome runWithDelayedIpcWriter(IZotopeIPCAssistant& assistant,
                                               const std::string& segName,
                                               const IpcAssistantRunArgs& args,
                                               Writer writer,
                                               size_t writePtrOff = kWrtPtrOff)
{
    const auto* constSegment = assistant.getFakeSegmentForTests(segName);
    REQUIRE(constSegment != nullptr);
    auto* segment = const_cast<std::vector<uint8_t>*>(constSegment);
    const auto initialWrite = readU32At(*segment, writePtrOff);
    std::atomic<bool> done{false};

    std::thread responder([&, initialWrite]() {
        for (size_t attempt = 0; attempt < 1000 && !done.load(std::memory_order_acquire); ++attempt)
        {
            if (readU32At(*segment, writePtrOff) != initialWrite)
            {
                writer(*segment);
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    auto outcome = assistant.runAssistant(args);
    done.store(true, std::memory_order_release);
    responder.join();
    return outcome;
}

// Build a valid AssistantResult payload: N params as {uint16,float32}
static std::vector<uint8_t> makeAssistantResultPayload(
    const std::vector<std::pair<int, float>>& params)
{
    uint16_t n = static_cast<uint16_t>(params.size());
    std::vector<uint8_t> buf(2 + params.size() * 6);
    std::memcpy(buf.data(), &n, 2);
    for (size_t i = 0; i < params.size(); ++i) {
        const auto index = static_cast<uint16_t>(params[i].first);
        std::memcpy(buf.data() + 2 + i * 6,     &index,  2);
        std::memcpy(buf.data() + 2 + i * 6 + 2, &params[i].second, 4);
    }
    return buf;
}

// Base args pointing at a fake segment with write gates open
static IpcAssistantRunArgs baseArgs(const std::string& segName)
{
    IpcAssistantRunArgs a;
    a.segmentName     = segName;
    a.ozoneInstanceId = 0xABCD1234u;
    a.observerId      = 0xDEADBEEFu;
    a.timeoutMs       = 100;   // fast for tests
    a.pollIntervalMs  = 1;
    a.allowUnsafeWrite = true;
    return a;
}

// Set the env gate for the duration of a test scope
struct WriteGateGuard {
#ifdef _WIN32
    WriteGateGuard()  { SetEnvironmentVariableA("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1"); }
    ~WriteGateGuard() { SetEnvironmentVariableA("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", nullptr); }
#else
    WriteGateGuard()  { setenv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", "1", 1); }
    ~WriteGateGuard() { unsetenv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE"); }
#endif
};

struct DebugMagicGuard {
#ifdef _WIN32
    DebugMagicGuard()  { SetEnvironmentVariableA("MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC", "1"); }
    ~DebugMagicGuard() { SetEnvironmentVariableA("MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC", nullptr); }
#else
    DebugMagicGuard()  { setenv("MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC", "1", 1); }
    ~DebugMagicGuard() { unsetenv("MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC"); }
#endif
};

// ═══════════════════════════════════════════════════════════════════════════
// Test cases
// ═══════════════════════════════════════════════════════════════════════════

// ── 1. Staged write publishes a complete AssistantRequest ─────────────────
TEST_CASE("Staged write publishes complete AssistantRequest frame and advances write pointer last")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();

    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");
    // Plant a stale result before the request. The delayed writer appends the
    // fresh result only after runAssistant publishes AssistantRequest.
    auto stalePayload = makeAssistantResultPayload({{88, 0.91f}});
    writeFrameAt(seg, 0, 0x0021, 0xABCD1234u, 0xDEADBEEFu, stalePayload);

    assistant->setFakeSegmentForTests("test_seg_1", seg);
    auto args = baseArgs("test_seg_1");
    args.timeoutMs = 500;
    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_1",
        args,
        [](std::vector<uint8_t>& segment) {
            auto freshPayload = makeAssistantResultPayload({{99, 0.42f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, freshPayload);
        });

    REQUIRE(result.isSuccess());
    REQUIRE(result.text().find("\"index\":99") != std::string::npos);
    REQUIRE(result.text().find("\"index\":88") == std::string::npos);

    // Verify the written request frame is present after the result frame in
    // the ring (write pointer advanced past both frames).
    const auto* final = assistant->getFakeSegmentForTests("test_seg_1");
    REQUIRE(final != nullptr);

    uint32_t wp;
    std::memcpy(&wp, final->data() + kWrtPtrOff, 4);
    // Write pointer must be strictly greater than where the result frame ends
    REQUIRE(wp > 0u);
}

// ── 2. Both write gates must be open ─────────────────────────────────────
TEST_CASE("Write blocked when env gate is missing")
{
    // Do NOT open the env gate
#ifdef _WIN32
    SetEnvironmentVariableA("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE", nullptr);
#else
    unsetenv("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE");
#endif

    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    const std::vector<uint8_t> before(seg.begin(), seg.end());

    assistant->setFakeSegmentForTests("test_seg_gate_env", seg);

    IpcAssistantRunArgs args = baseArgs("test_seg_gate_env");
    args.allowUnsafeWrite = true;  // runtime flag set, but env is 0

    auto result = assistant->runAssistant(args);
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.errorMessage().find("blocked") != std::string::npos);

    // Segment must be unchanged
    const auto* after = assistant->getFakeSegmentForTests("test_seg_gate_env");
    REQUIRE(*after == before);
}

TEST_CASE("Write blocked when allow_unsafe_write is false")
{
    WriteGateGuard gate;  // env gate open
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    const std::vector<uint8_t> before(seg.begin(), seg.end());

    assistant->setFakeSegmentForTests("test_seg_gate_flag", seg);

    IpcAssistantRunArgs args = baseArgs("test_seg_gate_flag");
    args.allowUnsafeWrite = false;  // runtime flag closed

    auto result = assistant->runAssistant(args);
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.errorMessage().find("blocked") != std::string::npos);

    const auto* after = assistant->getFakeSegmentForTests("test_seg_gate_flag");
    REQUIRE(*after == before);
}

// ── 3. Corrupt bytes before valid AssistantResult are skipped ─────────────
TEST_CASE("Corrupt bytes before AssistantResult are resynced and result is returned")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_corrupt", seg);
    auto args = baseArgs("test_seg_corrupt");
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_corrupt",
        args,
        [](std::vector<uint8_t>& segment) {
            appendBytesAtCurrentWrite(segment, std::vector<uint8_t>(12, 0xFFu));
            auto payload = makeAssistantResultPayload({{14, 0.55f}, {88, 0.91f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload);
        });
    REQUIRE(result.isSuccess());
    // Result must contain both param entries
    REQUIRE(result.text().find("\"index\":14") != std::string::npos);
    REQUIRE(result.text().find("\"index\":88") != std::string::npos);
}

// ── 4. Unknown valid frames before AssistantResult are consumed ───────────
TEST_CASE("Unknown valid frames are consumed and poll continues to AssistantResult")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_skip", seg);
    auto args = baseArgs("test_seg_skip");
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_skip",
        args,
        [](std::vector<uint8_t>& segment) {
            appendFrameAtCurrentWrite(segment, 0x0010, 0xABCD1234u, 0xFFFFFFFFu, std::vector<uint8_t>(64, 0xAA));
            appendFrameAtCurrentWrite(segment, 0x0011, 0xABCD1234u, 0xFFFFFFFFu, std::vector<uint8_t>(16, 0xBB));
            auto resultPayload = makeAssistantResultPayload({{102, 0.73f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, resultPayload);
        });
    REQUIRE(result.isSuccess());
    REQUIRE(result.text().find("\"index\":102") != std::string::npos);
}

// ── 5. Incomplete frame waits until timeout ───────────────────────────────
TEST_CASE("Incomplete frame in ring causes clean timeout error")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_incomplete", seg);
    auto args = baseArgs("test_seg_incomplete");
    args.timeoutMs    = 80;
    args.pollIntervalMs = 10;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_incomplete",
        args,
        [](std::vector<uint8_t>& segment) {
            FrameHeader hdr{};
            hdr.magic       = kMagic;
            hdr.version     = 3;
            hdr.messageType = 0x0021u;
            hdr.senderId    = 0xABCD1234u;
            hdr.targetId    = 0xDEADBEEFu;
            hdr.payloadSize = 100u;

            std::vector<uint8_t> partial(kHdrSz + 10, 0x01);
            std::memcpy(partial.data(), &hdr, kHdrSz);
            appendBytesAtCurrentWrite(segment, partial);
        });
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.errorMessage().find("timeout") != std::string::npos);
    REQUIRE(result.errorMessage().find("80") != std::string::npos);
}

// ── 6. Oversized payload returns an error ─────────────────────────────────
TEST_CASE("Oversized payload claim returns error without consuming segment")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_oversized", seg);
    auto args = baseArgs("test_seg_oversized");
    args.timeoutMs = 100;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_oversized",
        args,
        [](std::vector<uint8_t>& segment) {
            FrameHeader hdr{};
            hdr.magic       = kMagic;
            hdr.version     = 3;
            hdr.messageType = 0x0021u;
            hdr.senderId    = 0xABCD1234u;
            hdr.targetId    = 0xDEADBEEFu;
            hdr.payloadSize = 5u * 1024u * 1024u;

            std::vector<uint8_t> header(kHdrSz);
            std::memcpy(header.data(), &hdr, kHdrSz);
            appendBytesAtCurrentWrite(segment, header);
        });
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.errorMessage().find("oversized") != std::string::npos);
}

// ── 7. Observer slot cleared on success ──────────────────────────────────
TEST_CASE("Observer slot is cleared from plugin registry after successful run")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone", 0);
    // Leave slot 1 empty for observer registration

    assistant->setFakeSegmentForTests("test_seg_observer_success", seg);
    auto args = baseArgs("test_seg_observer_success");
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_observer_success",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{55, 0.5f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload);
        });
    REQUIRE(result.isSuccess());

    const auto* final = assistant->getFakeSegmentForTests("test_seg_observer_success");
    // Scan registry: observer entry (id=0xDEADBEEF) must be zeroed
    bool found = false;
    for (size_t slot = 0; slot < 32; ++slot) {
        uint32_t id;
        std::memcpy(&id, final->data() + kRegOff + slot * kEntrySize, 4);
        if (id == 0xDEADBEEFu) { found = true; break; }
    }
    REQUIRE_FALSE(found); // should have been cleared
}

// ── 8. Observer slot cleared on error path (no Ozone instance) ────────────
TEST_CASE("Observer slot is cleared after error when Ozone instance not found")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    // Do NOT write any plugin registry entry — findPluginByName will fail

    assistant->setFakeSegmentForTests("test_seg_observer_err", seg);

    IpcAssistantRunArgs args = baseArgs("test_seg_observer_err");
    args.ozoneInstanceId = std::nullopt;  // force name lookup
    args.pluginNameQuery = "Ozone";

    auto result = assistant->runAssistant(args);
    REQUIRE_FALSE(result.isSuccess());

    const auto* final = assistant->getFakeSegmentForTests("test_seg_observer_err");
    // Observer slot must be gone
    bool found = false;
    for (size_t slot = 0; slot < 32; ++slot) {
        uint32_t id;
        std::memcpy(&id, final->data() + kRegOff + slot * kEntrySize, 4);
        if (id == 0xDEADBEEFu) { found = true; break; }
    }
    REQUIRE_FALSE(found);
}

// ── 9. schema_path arg takes precedence over IZOTOPE_IPC_SCHEMA_PATH env ──
TEST_CASE("schema_path argument takes precedence over IZOTOPE_IPC_SCHEMA_PATH env")
{
    WriteGateGuard gate;
    // Set env to a nonexistent path that would cause wrong offsets
#ifdef _WIN32
    SetEnvironmentVariableA("IZOTOPE_IPC_SCHEMA_PATH", "C:\\nonexistent\\wrong_schema.json");
#else
    setenv("IZOTOPE_IPC_SCHEMA_PATH", "/nonexistent/wrong_schema.json", 1);
#endif

    // Write a schema JSON using default offsets to a temp file
    const std::string schemaFile = (std::filesystem::temp_directory_path() / "test_schema_correct.json").string();
    {
        std::ofstream f(schemaFile);
        f << R"({"readPtrOff":256,"writePtrOff":260,"pluginRegOff":264,)"
          << R"("ringOff":512,"ringSize":4193792,"entrySize":16,"maxEntries":32})";
    }

    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_schema", seg);
    auto args = baseArgs("test_seg_schema");
    args.schemaPath = schemaFile;
    args.timeoutMs  = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_schema",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{7, 0.3f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload);
        });
    // If arg schema was used (correct offsets) the call should succeed
    REQUIRE(result.isSuccess());

    // Cleanup
#ifdef _WIN32
    SetEnvironmentVariableA("IZOTOPE_IPC_SCHEMA_PATH", nullptr);
#else
    unsetenv("IZOTOPE_IPC_SCHEMA_PATH");
#endif
    std::remove(schemaFile.c_str());
}

// ── 10. Ring wrap-around write and read ───────────────────────────────────
TEST_CASE("Ring wrap-around: frame written at end of ring is read correctly")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    // Start both pointers 8 bytes before the end of the ring so the request
    // and delayed AssistantResult exercise ring wrap-around.
    uint32_t nearEnd = static_cast<uint32_t>(kRingSize - 8);
    std::memcpy(seg.data() + kReadPtrOff, &nearEnd, 4);
    std::memcpy(seg.data() + kWrtPtrOff, &nearEnd, 4);

    assistant->setFakeSegmentForTests("test_seg_wrap", seg);
    auto args = baseArgs("test_seg_wrap");
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_wrap",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{33, 0.77f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload);
        });
    REQUIRE(result.isSuccess());
    REQUIRE(result.text().find("\"index\":33") != std::string::npos);
    REQUIRE(result.text().find("0.77") != std::string::npos);
}

// ── 11. Structured string magic is interpreted as exact wire bytes ─────────
TEST_CASE("Structured string magic uses wire-byte order")
{
    WriteGateGuard gate;
    const std::string schemaFile = (std::filesystem::temp_directory_path() / "wire_magic_schema.json").string();
    {
        std::ofstream f(schemaFile);
        f << R"({
            "mapped_size_bytes":4194304,
            "frame":{"header_size":28,"magic":"IZOT","version":3},
            "registry":{"offset":264,"entry_size":16,"max_entries":32,"id_offset":0,"name_offset":8,"name_size":8},
            "ring":{"read_index_offset":256,"write_index_offset":260,"data_offset":512,"capacity_bytes":4193792},
            "assistant_result":{"count_offset":0,"entry_offset":2,"entry_size":6,"param_index_offset":0,"value_offset":2},
            "messages":{"assistant_request":32,"assistant_result":33}
        })";
    }

    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_wire_magic", seg);
    auto args = baseArgs("test_seg_wire_magic");
    args.schemaPath = schemaFile;
    args.timeoutMs = 500;

    static constexpr uint32_t kAsciiWireMagic = 0x544F5A49u;
    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_wire_magic",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{11, 0.64f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu,
                                      payload, kRingOff, kRingSize, kWrtPtrOff, kAsciiWireMagic);
        });

    REQUIRE(result.isSuccess());
    REQUIRE(result.text().find("\"index\":11") != std::string::npos);

    const auto* final = assistant->getFakeSegmentForTests("test_seg_wire_magic");
    REQUIRE(final != nullptr);
    REQUIRE((*final)[kRingOff + 0] == static_cast<uint8_t>('I'));
    REQUIRE((*final)[kRingOff + 1] == static_cast<uint8_t>('Z'));
    REQUIRE((*final)[kRingOff + 2] == static_cast<uint8_t>('O'));
    REQUIRE((*final)[kRingOff + 3] == static_cast<uint8_t>('T'));

    std::remove(schemaFile.c_str());
}

// ── 12. Debug magic probe reports request frame bytes ─────────────────────
TEST_CASE("Debug IPC magic probe reports request frame bytes")
{
    WriteGateGuard gate;
    DebugMagicGuard debugMagic;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_magic_probe", seg);
    auto args = baseArgs("test_seg_magic_probe");
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_magic_probe",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{12, 0.21f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload);
        });

    REQUIRE(result.isSuccess());
    REQUIRE(result.body.contains("ipc_magic_probe"));

    const auto& probe = result.body["ipc_magic_probe"];
    REQUIRE(probe["expected_magic_hex"].get<std::string>() == "0x495A4F54");
    REQUIRE(probe["request_start_index"].get<int>() == 0);
    REQUIRE(probe["request_watermark"].get<int>() == 28);
    REQUIRE(probe["request_frame_magic"]["value_hex"].get<std::string>() == "0x495A4F54");
    REQUIRE(probe["request_frame_magic"]["bytes"][0].get<int>() == 0x54);
    REQUIRE(probe["request_frame_magic"]["bytes"][1].get<int>() == 0x4F);
    REQUIRE(probe["request_frame_magic"]["bytes"][2].get<int>() == 0x5A);
    REQUIRE(probe["request_frame_magic"]["bytes"][3].get<int>() == 0x49);
}

// ── 13. Full ring is reported before overwriting unread data ───────────────
TEST_CASE("Full ring prevents AssistantRequest write")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    writeU32At(seg, kReadPtrOff, 1);
    writeU32At(seg, kWrtPtrOff, 0);
    const auto before = seg;

    assistant->setFakeSegmentForTests("test_seg_ring_full", seg);
    auto args = baseArgs("test_seg_ring_full");

    auto result = assistant->runAssistant(args);
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.body.value("code", std::string{}) == "ipc_ring_full");

    const auto* after = assistant->getFakeSegmentForTests("test_seg_ring_full");
    REQUIRE(*after == before);
}

// ── 14. Observer deregistration is best-effort on success ─────────────────
TEST_CASE("Observer deregistration failure is reported without failing AssistantResult")
{
    WriteGateGuard gate;
    const std::string schemaFile = (std::filesystem::temp_directory_path() / "small_ring_schema.json").string();
    {
        std::ofstream f(schemaFile);
        f << R"({
            "mapped_size_bytes":4194304,
            "frame":{"header_size":28,"magic":1230655316,"version":3},
            "registry":{"offset":264,"entry_size":16,"max_entries":32,"id_offset":0,"name_offset":8,"name_size":8},
            "ring":{"read_index_offset":256,"write_index_offset":260,"data_offset":512,"capacity_bytes":65},
            "assistant_result":{"count_offset":0,"entry_offset":2,"entry_size":6,"param_index_offset":0,"value_offset":2},
            "messages":{"assistant_request":32,"assistant_result":33,"observer_deregister":34}
        })";
    }

    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    writeRegistryEntry(seg, 0xABCD1234u, "Ozone");

    assistant->setFakeSegmentForTests("test_seg_deregister_best_effort", seg);
    auto args = baseArgs("test_seg_deregister_best_effort");
    args.schemaPath = schemaFile;
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_deregister_best_effort",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{2, 0.25f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0xABCD1234u, 0xDEADBEEFu, payload, kRingOff, 65);
        });

    REQUIRE(result.isSuccess());
    REQUIRE(result.body.value("observer_deregister_sent", true) == false);
    REQUIRE(result.text().find("\"index\":2") != std::string::npos);

    std::remove(schemaFile.c_str());
}

// ── 15. Invalid schema bounds return a typed error ─────────────────────────
TEST_CASE("Invalid schema bounds return typed error without touching ring")
{
    WriteGateGuard gate;
    const std::string schemaFile = (std::filesystem::temp_directory_path() / "bad_schema.json").string();
    {
        std::ofstream f(schemaFile);
        // ringOff + ringSize far exceeds 4 MiB segment
        f << R"({"readPtrOff":256,"writePtrOff":260,"pluginRegOff":264,)"
          << R"("ringOff":512,"ringSize":99999999})";
    }

    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    const auto before = seg;

    assistant->setFakeSegmentForTests("test_seg_bad_schema", seg);

    IpcAssistantRunArgs args = baseArgs("test_seg_bad_schema");
    args.schemaPath = schemaFile;

    auto result = assistant->runAssistant(args);
    REQUIRE_FALSE(result.isSuccess());
    REQUIRE(result.errorMessage().find("schema bounds") != std::string::npos);

    const auto* after = assistant->getFakeSegmentForTests("test_seg_bad_schema");
    REQUIRE(*after == before); // untouched

    std::remove(schemaFile.c_str());
}

// ── 16. getFakeSegmentForTests returns pointer, not copy ──────────────────
TEST_CASE("getFakeSegmentForTests returns null for unknown segment name")
{
    auto assistant = createIZotopeIPCAssistant();
    REQUIRE(assistant->getFakeSegmentForTests("nonexistent") == nullptr);
}

// ── 17. Valid explicit ozoneInstanceId bypasses registry lookup ────────────
TEST_CASE("Explicit ozoneInstanceId skips plugin registry lookup")
{
    WriteGateGuard gate;
    auto assistant = createIZotopeIPCAssistant();
    auto seg = blankSeg();
    // Registry is empty — but we pass an explicit instance ID

    assistant->setFakeSegmentForTests("test_seg_explicit_id", seg);
    auto args = baseArgs("test_seg_explicit_id");
    args.ozoneInstanceId = 0x11223344u;
    args.timeoutMs = 500;

    auto result = runWithDelayedIpcWriter(
        *assistant,
        "test_seg_explicit_id",
        args,
        [](std::vector<uint8_t>& segment) {
            auto payload = makeAssistantResultPayload({{1, 0.1f}});
            appendFrameAtCurrentWrite(segment, 0x0021, 0x11223344u, 0xDEADBEEFu, payload);
        });
    REQUIRE(result.isSuccess());
    REQUIRE(result.text().find("\"index\":1") != std::string::npos);
}
