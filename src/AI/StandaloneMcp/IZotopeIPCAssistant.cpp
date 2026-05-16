#include "IZotopeIPCAssistant.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#if JUCE_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <tlhelp32.h>
#endif

namespace more_phi::standalone_mcp {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxMappedSize = 64u * 1024u * 1024u;
constexpr uint32_t kDefaultMagicIzot = 0x495A4F54u;
constexpr uint32_t kBroadcastTarget = 0xffffffffu;

ToolCallOutcome makeToolError(std::string error, std::string message = {})
{
    json body{{"success", false}, {"error", std::move(error)}};
    if (!message.empty())
        body["message"] = std::move(message);
    return {body, true};
}

std::string envString(const char* key)
{
    return juce::SystemStats::getEnvironmentVariable(key, {}).toStdString();
}

bool envEnabled(const char* key)
{
    const auto value = envString(key);
    return value == "1" || value == "true" || value == "TRUE" || value == "yes";
}

std::string platformName()
{
#if JUCE_WINDOWS
    return "windows";
#else
    return "unsupported";
#endif
}

uint16_t readU16LE(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0])
        | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t readU32LE(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8u)
        | (static_cast<uint32_t>(p[2]) << 16u)
        | (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t readU64LE(const uint8_t* p)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8u) | static_cast<uint64_t>(p[i]);
    return value;
}

float readF32LE(const uint8_t* p)
{
    static_assert(sizeof(float) == 4);
    uint32_t raw = readU32LE(p);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

void writeU16LE(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value & 0xffu);
    p[1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeU32LE(uint8_t* p, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>((value >> (8u * i)) & 0xffu);
}

void writeU64LE(uint8_t* p, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>((value >> (8u * i)) & 0xffu);
}

uint64_t monotonicTimestampMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool getRequiredObject(const json& root, const char* key, json& out, std::string& error)
{
    if (!root.contains(key) || !root[key].is_object())
    {
        error = std::string("manifest missing object: ") + key;
        return false;
    }

    out = root[key];
    return true;
}

bool readSizeField(const json& obj, const char* key, size_t& out, std::string& error)
{
    if (!obj.contains(key) || !obj[key].is_number_integer())
    {
        error = std::string("manifest field must be a non-negative integer: ") + key;
        return false;
    }

    const auto value = obj[key].get<long long>();
    if (value < 0)
    {
        error = std::string("manifest field must be non-negative: ") + key;
        return false;
    }

    out = static_cast<size_t>(value);
    return true;
}

bool readOptionalSizeField(const json& obj, const char* key, size_t& out, std::string& error)
{
    if (!obj.contains(key))
        return true;
    return readSizeField(obj, key, out, error);
}

bool readU32Field(const json& obj, const char* key, uint32_t& out, std::string& error)
{
    size_t value = 0;
    if (!readSizeField(obj, key, value, error))
        return false;
    if (value > 0xffffffffull)
    {
        error = std::string("manifest field exceeds uint32: ") + key;
        return false;
    }

    out = static_cast<uint32_t>(value);
    return true;
}

uint32_t magicFromString(const std::string& text)
{
    if (text.size() != 4)
        return 0;

    return (static_cast<uint32_t>(static_cast<uint8_t>(text[0])) << 24u)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 16u)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 8u)
        | static_cast<uint32_t>(static_cast<uint8_t>(text[3]));
}

struct FrameLayout
{
    size_t headerSize = 28;
    size_t magicOffset = 0;
    size_t versionOffset = 4;
    size_t messageTypeOffset = 6;
    size_t senderIdOffset = 8;
    size_t targetIdOffset = 12;
    size_t payloadSizeOffset = 16;
    size_t timestampOffset = 20;
    uint32_t magic = kDefaultMagicIzot;
    uint16_t version = 3;
};

struct RegistryLayout
{
    size_t offset = 0;
    size_t entrySize = 0;
    size_t maxEntries = 0;
    size_t idOffset = 0;
    size_t nameOffset = 0;
    size_t nameSize = 0;
    std::optional<size_t> activeOffset;
    uint32_t activeValue = 1;
};

struct RingLayout
{
    size_t readIndexOffset = 0;
    size_t writeIndexOffset = 0;
    size_t dataOffset = 0;
    size_t capacityBytes = 0;
};

struct ResultLayout
{
    size_t countOffset = 0;
    size_t entryOffset = 2;
    size_t entrySize = 6;
    size_t paramIndexOffset = 0;
    size_t valueOffset = 2;
};

struct MessageTypes
{
    uint16_t assistantRequest = 0x0020;
    uint16_t assistantResult = 0x0021;
    std::optional<uint16_t> observerDeregister;
};

struct IpcSchemaManifest
{
    size_t mappedSizeBytes = 4u * 1024u * 1024u;
    std::optional<std::string> segmentNameTemplate;
    FrameLayout frame;
    RegistryLayout registry;
    RingLayout ring;
    ResultLayout assistantResult;
    MessageTypes messages;
};

bool parseFrameLayout(const json& root, FrameLayout& frame, std::string& error)
{
    json obj;
    if (!getRequiredObject(root, "frame", obj, error))
        return false;

    if (!readOptionalSizeField(obj, "header_size", frame.headerSize, error)
        || !readOptionalSizeField(obj, "magic_offset", frame.magicOffset, error)
        || !readOptionalSizeField(obj, "version_offset", frame.versionOffset, error)
        || !readOptionalSizeField(obj, "message_type_offset", frame.messageTypeOffset, error)
        || !readOptionalSizeField(obj, "sender_id_offset", frame.senderIdOffset, error)
        || !readOptionalSizeField(obj, "target_id_offset", frame.targetIdOffset, error)
        || !readOptionalSizeField(obj, "payload_size_offset", frame.payloadSizeOffset, error)
        || !readOptionalSizeField(obj, "timestamp_offset", frame.timestampOffset, error))
        return false;

    if (obj.contains("magic"))
    {
        if (obj["magic"].is_string())
            frame.magic = magicFromString(obj["magic"].get<std::string>());
        else if (!readU32Field(obj, "magic", frame.magic, error))
            return false;
    }

    uint32_t version = frame.version;
    if (obj.contains("version") && !readU32Field(obj, "version", version, error))
        return false;
    if (version > 0xffffu || frame.magic == 0)
    {
        error = "manifest frame magic/version is invalid";
        return false;
    }

    frame.version = static_cast<uint16_t>(version);
    const auto minimumEnd = std::max({
        frame.magicOffset + 4,
        frame.versionOffset + 2,
        frame.messageTypeOffset + 2,
        frame.senderIdOffset + 4,
        frame.targetIdOffset + 4,
        frame.payloadSizeOffset + 4,
        frame.timestampOffset + 8
    });

    if (frame.headerSize < minimumEnd)
    {
        error = "manifest frame header_size is smaller than its field layout";
        return false;
    }

    return true;
}

bool parseRegistryLayout(const json& root, RegistryLayout& registry, std::string& error)
{
    json obj;
    if (!getRequiredObject(root, "registry", obj, error))
        return false;

    if (!readSizeField(obj, "offset", registry.offset, error)
        || !readSizeField(obj, "entry_size", registry.entrySize, error)
        || !readSizeField(obj, "max_entries", registry.maxEntries, error)
        || !readSizeField(obj, "id_offset", registry.idOffset, error)
        || !readSizeField(obj, "name_offset", registry.nameOffset, error)
        || !readSizeField(obj, "name_size", registry.nameSize, error))
        return false;

    if (obj.contains("active_offset"))
    {
        size_t activeOffset = 0;
        if (!readSizeField(obj, "active_offset", activeOffset, error))
            return false;
        registry.activeOffset = activeOffset;
    }

    if (obj.contains("active_value") && !readU32Field(obj, "active_value", registry.activeValue, error))
        return false;

    const auto minimumEnd = std::max({
        registry.idOffset + 4,
        registry.nameOffset + registry.nameSize,
        registry.activeOffset ? *registry.activeOffset + 4 : 0
    });

    if (registry.entrySize < minimumEnd || registry.maxEntries == 0 || registry.nameSize == 0)
    {
        error = "manifest registry layout is internally inconsistent";
        return false;
    }

    return true;
}

bool parseRingLayout(const json& root, RingLayout& ring, std::string& error)
{
    json obj;
    if (!getRequiredObject(root, "ring", obj, error))
        return false;

    if (!readSizeField(obj, "read_index_offset", ring.readIndexOffset, error)
        || !readSizeField(obj, "write_index_offset", ring.writeIndexOffset, error)
        || !readSizeField(obj, "data_offset", ring.dataOffset, error)
        || !readSizeField(obj, "capacity_bytes", ring.capacityBytes, error))
        return false;

    if (ring.capacityBytes == 0)
    {
        error = "manifest ring capacity_bytes must be non-zero";
        return false;
    }

    return true;
}

bool parseResultLayout(const json& root, ResultLayout& result, std::string& error)
{
    json obj;
    if (!getRequiredObject(root, "assistant_result", obj, error))
        return false;

    if (!readOptionalSizeField(obj, "count_offset", result.countOffset, error)
        || !readOptionalSizeField(obj, "entry_offset", result.entryOffset, error)
        || !readOptionalSizeField(obj, "entry_size", result.entrySize, error)
        || !readOptionalSizeField(obj, "param_index_offset", result.paramIndexOffset, error)
        || !readOptionalSizeField(obj, "value_offset", result.valueOffset, error))
        return false;

    if (result.entrySize < std::max(result.paramIndexOffset + 2, result.valueOffset + 4))
    {
        error = "manifest assistant_result entry_size is too small";
        return false;
    }

    return true;
}

bool parseMessages(const json& root, MessageTypes& messages, std::string& error)
{
    json obj;
    if (!getRequiredObject(root, "messages", obj, error))
        return false;

    uint32_t request = 0;
    uint32_t result = 0;
    if (!readU32Field(obj, "assistant_request", request, error)
        || !readU32Field(obj, "assistant_result", result, error))
        return false;

    if (request > 0xffffu || result > 0xffffu)
    {
        error = "manifest message type IDs must fit in uint16";
        return false;
    }

    if (obj.contains("observer_deregister"))
    {
        uint32_t deregister = 0;
        if (!readU32Field(obj, "observer_deregister", deregister, error))
            return false;
        if (deregister > 0xffffu)
        {
            error = "manifest message type IDs must fit in uint16";
            return false;
        }

        messages.observerDeregister = static_cast<uint16_t>(deregister);
    }

    messages.assistantRequest = static_cast<uint16_t>(request);
    messages.assistantResult = static_cast<uint16_t>(result);
    return true;
}

bool validateMappedRange(size_t mappedSize, size_t offset, size_t bytes)
{
    return offset <= mappedSize && bytes <= mappedSize - offset;
}

bool parseManifestFile(const std::string& path, IpcSchemaManifest& manifest, std::string& error)
{
    if (path.empty())
    {
        error = "schema_path or IZOTOPE_IPC_SCHEMA_PATH is required.";
        return false;
    }

    const juce::File file{juce::String(path)};
    if (!file.existsAsFile())
    {
        error = "schema_path does not exist: " + path;
        return false;
    }

    json root;
    try
    {
        root = json::parse(file.loadFileAsString().toStdString());
    }
    catch (const std::exception& e)
    {
        error = std::string("schema JSON parse failed: ") + e.what();
        return false;
    }

    if (root.contains("mapped_size_bytes")
        && !readSizeField(root, "mapped_size_bytes", manifest.mappedSizeBytes, error))
        return false;

    if (manifest.mappedSizeBytes == 0 || manifest.mappedSizeBytes > kMaxMappedSize)
    {
        error = "manifest mapped_size_bytes must be between 1 and 67108864";
        return false;
    }

    if (root.contains("segment_name_template") && root["segment_name_template"].is_string())
        manifest.segmentNameTemplate = root["segment_name_template"].get<std::string>();

    if (!parseFrameLayout(root, manifest.frame, error)
        || !parseRegistryLayout(root, manifest.registry, error)
        || !parseRingLayout(root, manifest.ring, error)
        || !parseResultLayout(root, manifest.assistantResult, error)
        || !parseMessages(root, manifest.messages, error))
        return false;

    if (!validateMappedRange(manifest.mappedSizeBytes, manifest.ring.readIndexOffset, 4)
        || !validateMappedRange(manifest.mappedSizeBytes, manifest.ring.writeIndexOffset, 4)
        || !validateMappedRange(manifest.mappedSizeBytes, manifest.ring.dataOffset, manifest.ring.capacityBytes)
        || !validateMappedRange(manifest.mappedSizeBytes, manifest.registry.offset,
            manifest.registry.entrySize * manifest.registry.maxEntries))
    {
        error = "manifest layout exceeds mapped_size_bytes";
        return false;
    }

    return true;
}

std::string applyPidTemplate(std::string templ, uint32_t pid)
{
    constexpr const char* token = "{pid}";
    const auto pos = templ.find(token);
    if (pos != std::string::npos)
        templ.replace(pos, std::strlen(token), std::to_string(pid));
    return templ;
}

std::optional<uint32_t> discoverDawProcessId()
{
#if JUCE_WINDOWS
    static constexpr const char* kKnownDaws[] = {
        "reaper.exe",
        "fl64.exe",
        "fl.exe",
        "ableton live 12 suite.exe",
        "ableton live 12 standard.exe",
        "ableton live 11 suite.exe",
        "studio one.exe",
        "cubase.exe",
        "nuendo.exe",
        "bitwig studio.exe"
    };

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;

    struct SnapshotCloser
    {
        HANDLE handle = nullptr;
        ~SnapshotCloser() { if (handle != nullptr) CloseHandle(handle); }
    } closer{snapshot};

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry))
        return std::nullopt;

    do
    {
        const auto exe = juce::String(entry.szExeFile).toLowerCase().toStdString();
        for (const auto* daw : kKnownDaws)
        {
            if (exe == daw)
                return static_cast<uint32_t>(entry.th32ProcessID);
        }
    }
    while (Process32NextW(snapshot, &entry));
#endif

    return std::nullopt;
}

std::string resolveSegmentName(const IpcAssistantRunArgs& args, const IpcSchemaManifest& manifest)
{
    if (args.segmentName && !args.segmentName->empty())
        return *args.segmentName;

    if (manifest.segmentNameTemplate && args.dawProcessId)
        return applyPidTemplate(*manifest.segmentNameTemplate, *args.dawProcessId);

    return envString("IZOTOPE_IPC_SEGMENT_NAME");
}

bool stringContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    auto lower = [](std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    };

    return lower(haystack).find(lower(needle)) != std::string::npos;
}

struct AttachedMemory
{
    uint8_t* bytes = nullptr;
    size_t size = 0;
    bool fake = false;
    std::string segmentName;

#if JUCE_WINDOWS
    HANDLE mappingHandle = nullptr;
#endif

    ~AttachedMemory()
    {
#if JUCE_WINDOWS
        if (!fake && bytes != nullptr)
            UnmapViewOfFile(bytes);
        if (mappingHandle != nullptr)
            CloseHandle(mappingHandle);
#endif
    }

    AttachedMemory() = default;
    AttachedMemory(const AttachedMemory&) = delete;
    AttachedMemory& operator=(const AttachedMemory&) = delete;
};

ToolCallOutcome attachRealWritable(const std::string& segmentName,
                                   size_t requestedBytes,
                                   AttachedMemory& attached)
{
#if JUCE_WINDOWS
    const auto wideName = juce::String(segmentName).toWideCharPointer();
    attached.mappingHandle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wideName);
    if (attached.mappingHandle == nullptr)
        return makeToolError("ipc_attach_failed", "OpenFileMappingW failed for read/write segment.");

    attached.bytes = static_cast<uint8_t*>(
        MapViewOfFile(attached.mappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, requestedBytes));
    if (attached.bytes == nullptr)
        return makeToolError("ipc_attach_failed", "MapViewOfFile failed for read/write segment.");

    attached.size = requestedBytes;
    attached.segmentName = segmentName;
    return {json{
        {"success", true},
        {"attached", true},
        {"segment_name", segmentName},
        {"mapped_size_bytes", requestedBytes},
        {"platform", platformName()}
    }, false};
#else
    juce::ignoreUnused(segmentName, requestedBytes, attached);
    return makeToolError("unsupported_platform", "Active IPC writes are implemented for Windows first.");
#endif
}

std::optional<uint32_t> findPluginInstance(const uint8_t* bytes,
                                           size_t mappedSize,
                                           const RegistryLayout& registry,
                                           const std::string& query)
{
    if (!validateMappedRange(mappedSize, registry.offset, registry.entrySize * registry.maxEntries))
        return std::nullopt;

    for (size_t i = 0; i < registry.maxEntries; ++i)
    {
        const auto* entry = bytes + registry.offset + i * registry.entrySize;
        if (registry.activeOffset)
        {
            const auto active = readU32LE(entry + *registry.activeOffset);
            if (active != registry.activeValue)
                continue;
        }

        const auto id = readU32LE(entry + registry.idOffset);
        if (id == 0)
            continue;

        std::string name(reinterpret_cast<const char*>(entry + registry.nameOffset), registry.nameSize);
        if (const auto nul = name.find('\0'); nul != std::string::npos)
            name.resize(nul);

        if (stringContainsCaseInsensitive(name, query))
            return id;
    }

    return std::nullopt;
}

bool validateFrameRange(const FrameLayout& frame, size_t frameSize)
{
    return frame.magicOffset + 4 <= frameSize
        && frame.versionOffset + 2 <= frameSize
        && frame.messageTypeOffset + 2 <= frameSize
        && frame.senderIdOffset + 4 <= frameSize
        && frame.targetIdOffset + 4 <= frameSize
        && frame.payloadSizeOffset + 4 <= frameSize
        && frame.timestampOffset + 8 <= frameSize;
}

std::vector<uint8_t> makeFrame(const FrameLayout& layout,
                               uint16_t messageType,
                               uint32_t senderId,
                               uint32_t targetId,
                               const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame(layout.headerSize + payload.size(), 0);
    writeU32LE(frame.data() + layout.magicOffset, layout.magic);
    writeU16LE(frame.data() + layout.versionOffset, layout.version);
    writeU16LE(frame.data() + layout.messageTypeOffset, messageType);
    writeU32LE(frame.data() + layout.senderIdOffset, senderId);
    writeU32LE(frame.data() + layout.targetIdOffset, targetId);
    writeU32LE(frame.data() + layout.payloadSizeOffset, static_cast<uint32_t>(payload.size()));
    writeU64LE(frame.data() + layout.timestampOffset, monotonicTimestampMs());

    if (!payload.empty())
        std::memcpy(frame.data() + layout.headerSize, payload.data(), payload.size());

    return frame;
}

bool writeFrameToRing(uint8_t* bytes,
                      size_t mappedSize,
                      const RingLayout& ring,
                      const std::vector<uint8_t>& frame)
{
    if (frame.empty() || frame.size() > ring.capacityBytes)
        return false;
    if (!validateMappedRange(mappedSize, ring.writeIndexOffset, 4)
        || !validateMappedRange(mappedSize, ring.dataOffset, ring.capacityBytes))
        return false;

    const auto writeIndex = readU32LE(bytes + ring.writeIndexOffset) % static_cast<uint32_t>(ring.capacityBytes);
    for (size_t i = 0; i < frame.size(); ++i)
        bytes[ring.dataOffset + ((writeIndex + i) % ring.capacityBytes)] = frame[i];

    const auto nextWriteIndex = static_cast<uint32_t>((writeIndex + frame.size()) % ring.capacityBytes);
    std::atomic_thread_fence(std::memory_order_release);
    writeU32LE(bytes + ring.writeIndexOffset, nextWriteIndex);
    return true;
}

std::vector<uint8_t> copyFromRing(const uint8_t* bytes,
                                  const RingLayout& ring,
                                  size_t ringOffset,
                                  size_t size)
{
    std::vector<uint8_t> copied(size);
    for (size_t i = 0; i < size; ++i)
        copied[i] = bytes[ring.dataOffset + ((ringOffset + i) % ring.capacityBytes)];
    return copied;
}

struct ParsedFrame
{
    uint16_t messageType = 0;
    uint32_t senderId = 0;
    uint32_t targetId = 0;
    uint32_t payloadSize = 0;
    std::vector<uint8_t> payload;
};

std::optional<ParsedFrame> parseFrameAtRingOffset(const uint8_t* bytes,
                                                  const RingLayout& ring,
                                                  const FrameLayout& layout,
                                                  size_t ringOffset)
{
    if (layout.headerSize > ring.capacityBytes)
        return std::nullopt;

    const auto header = copyFromRing(bytes, ring, ringOffset, layout.headerSize);
    if (!validateFrameRange(layout, header.size()))
        return std::nullopt;
    if (readU32LE(header.data() + layout.magicOffset) != layout.magic)
        return std::nullopt;

    const auto payloadSize = readU32LE(header.data() + layout.payloadSizeOffset);
    const auto totalSize = layout.headerSize + static_cast<size_t>(payloadSize);
    if (totalSize > ring.capacityBytes)
        return std::nullopt;

    ParsedFrame parsed;
    parsed.messageType = readU16LE(header.data() + layout.messageTypeOffset);
    parsed.senderId = readU32LE(header.data() + layout.senderIdOffset);
    parsed.targetId = readU32LE(header.data() + layout.targetIdOffset);
    parsed.payloadSize = payloadSize;
    parsed.payload = copyFromRing(bytes, ring, ringOffset + layout.headerSize, payloadSize);
    return parsed;
}

json parseAssistantResultPayload(const std::vector<uint8_t>& payload,
                                 const ResultLayout& layout,
                                 std::string& error)
{
    if (layout.countOffset + 2 > payload.size())
    {
        error = "AssistantResult payload is too small for count.";
        return json::array();
    }

    const auto count = readU16LE(payload.data() + layout.countOffset);
    json params = json::array();
    for (uint16_t i = 0; i < count; ++i)
    {
        const auto entryBase = layout.entryOffset + static_cast<size_t>(i) * layout.entrySize;
        if (entryBase + layout.entrySize > payload.size())
        {
            error = "AssistantResult payload ended before all parameter entries.";
            return json::array();
        }

        const auto paramIndex = readU16LE(payload.data() + entryBase + layout.paramIndexOffset);
        const auto value = readF32LE(payload.data() + entryBase + layout.valueOffset);
        params.push_back({{"index", paramIndex}, {"value", value}});
    }

    return params;
}

bool writeObserverDeregister(uint8_t* bytes,
                             size_t mappedSize,
                             const IpcSchemaManifest& manifest,
                             uint32_t observerId,
                             uint32_t ozoneId)
{
    if (!manifest.messages.observerDeregister)
        return true;

    const auto frame = makeFrame(
        manifest.frame,
        *manifest.messages.observerDeregister,
        observerId,
        ozoneId,
        {});

    return writeFrameToRing(bytes, mappedSize, manifest.ring, frame);
}
std::optional<json> findAssistantResult(const uint8_t* bytes,
                                        const IpcSchemaManifest& manifest,
                                        uint32_t ozoneId,
                                        uint32_t observerId,
                                        std::string& error)
{
    for (size_t offset = 0; offset + manifest.frame.headerSize <= manifest.ring.capacityBytes; ++offset)
    {
        const auto frame = parseFrameAtRingOffset(bytes, manifest.ring, manifest.frame, offset);
        if (!frame)
            continue;
        if (frame->messageType != manifest.messages.assistantResult)
            continue;
        if (frame->senderId != ozoneId)
            continue;
        if (frame->targetId != observerId && frame->targetId != kBroadcastTarget && frame->targetId != 0)
            continue;

        auto params = parseAssistantResultPayload(frame->payload, manifest.assistantResult, error);
        if (!error.empty())
            return std::nullopt;

        return json{
            {"source_instance_id", frame->senderId},
            {"target_instance_id", frame->targetId},
            {"parameter_count", params.size()},
            {"parameters", params}
        };
    }

    return std::nullopt;
}

} // namespace

IZotopeIPCAssistant::~IZotopeIPCAssistant() = default;

ToolCallOutcome IZotopeIPCAssistant::runAssistant(const IpcAssistantRunArgs& args)
{
    if (!args.allowUnsafeWrite || !envEnabled("MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE"))
    {
        return makeToolError(
            "ipc_write_disabled",
            "Active iZotope IPC writes require MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1 and allow_unsafe_write=true.");
    }

    const auto schemaPath = args.schemaPath.value_or(envString("IZOTOPE_IPC_SCHEMA_PATH"));
    IpcSchemaManifest manifest;
    std::string schemaError;
    if (!parseManifestFile(schemaPath, manifest, schemaError))
        return makeToolError("ipc_schema_invalid", schemaError);

    auto segmentName = resolveSegmentName(args, manifest);
    if (segmentName.empty() && manifest.segmentNameTemplate && !args.dawProcessId)
    {
        const auto discoveredPid = discoverDawProcessId();
        if (!discoveredPid)
            return makeToolError("daw_not_found", "No supported DAW process was found for manifest segment_name_template replacement.");

        segmentName = applyPidTemplate(*manifest.segmentNameTemplate, *discoveredPid);
    }

    if (segmentName.empty())
        return makeToolError("missing_segment_name", "segment_name, IZOTOPE_IPC_SEGMENT_NAME, or manifest template + daw_process_id is required.");

    AttachedMemory attached;
    if (auto it = fakeSegments.find(segmentName); it != fakeSegments.end())
    {
        attached.bytes = it->second.data();
        attached.size = it->second.size();
        attached.fake = true;
        attached.segmentName = segmentName;
    }
    else
    {
        auto attachOutcome = attachRealWritable(segmentName, manifest.mappedSizeBytes, attached);
        if (attachOutcome.isError)
            return attachOutcome;
    }

    if (attached.bytes == nullptr || attached.size < manifest.mappedSizeBytes)
        return makeToolError("ipc_attach_failed", "Attached segment is smaller than manifest mapped_size_bytes.");

    const auto ozoneId = args.ozoneInstanceId
        ? args.ozoneInstanceId
        : findPluginInstance(attached.bytes, attached.size, manifest.registry, args.pluginNameQuery);
    if (!ozoneId)
        return makeToolError("ozone_instance_not_found", "Ozone instance not found in manifest-defined plugin registry.");

    const auto requestFrame = makeFrame(
        manifest.frame,
        manifest.messages.assistantRequest,
        args.observerId,
        *ozoneId,
        {});

    if (!writeFrameToRing(attached.bytes, attached.size, manifest.ring, requestFrame))
        return makeToolError("assistant_request_write_failed", "Could not write AssistantRequest frame to manifest-defined ring buffer.");

    const auto start = std::chrono::steady_clock::now();
    auto elapsedMs = [&]() -> size_t
    {
        return static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    };

    while (elapsedMs() <= args.timeoutMs)
    {
        std::string parseError;
        if (auto result = findAssistantResult(attached.bytes, manifest, *ozoneId, args.observerId, parseError))
        {
            if (!writeObserverDeregister(attached.bytes, attached.size, manifest, args.observerId, *ozoneId))
                return makeToolError("observer_deregister_failed", "Could not write observer deregistration frame to manifest-defined ring buffer.");

            return {json{
                {"success", true},
                {"segment_name", segmentName},
                {"schema_path", schemaPath},
                {"platform", attached.fake ? "test" : platformName()},
                {"observer_id", args.observerId},
                {"ozone_instance_id", *ozoneId},
                {"elapsed_ms", elapsedMs()},
                {"assistant_result", *result}
            }, false};
        }

        if (!parseError.empty())
            return makeToolError("assistant_result_parse_failed", parseError);

        if (args.timeoutMs == 0)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<size_t>(1, args.pollIntervalMs)));
    }

    return makeToolError("assistant_timeout", "Timed out waiting for AssistantResult frame.");
}

void IZotopeIPCAssistant::setFakeSegmentForTests(std::string name, std::vector<uint8_t> bytes)
{
    fakeSegments[std::move(name)] = std::move(bytes);
}

const std::vector<uint8_t>* IZotopeIPCAssistant::getFakeSegmentForTests(const std::string& name) const
{
    const auto it = fakeSegments.find(name);
    return it != fakeSegments.end() ? &it->second : nullptr;
}

std::unique_ptr<IZotopeIPCAssistant> createIZotopeIPCAssistant()
{
    return std::make_unique<IZotopeIPCAssistant>();
}

} // namespace more_phi::standalone_mcp
