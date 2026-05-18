#include "IZotopeIPCAssistant.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
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

ToolCallOutcome makeToolError(std::string code, std::string message = {})
{
    if (message.empty())
        message = code;

    return {json{{"success", false}, {"error", std::move(message)}, {"code", std::move(code)}}, true};
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

    // String magic is a wire-byte literal. Numeric manifest magic remains an
    // exact uint32 value for native fourcc-style schemas.
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0]))
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8u)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16u)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24u);
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

bool validateManifestRanges(size_t mappedSize, const IpcSchemaManifest& manifest, std::string& error)
{
    if (!validateMappedRange(mappedSize, manifest.ring.readIndexOffset, 4)
        || !validateMappedRange(mappedSize, manifest.ring.writeIndexOffset, 4)
        || !validateMappedRange(mappedSize, manifest.ring.dataOffset, manifest.ring.capacityBytes)
        || !validateMappedRange(mappedSize, manifest.registry.offset,
            manifest.registry.entrySize * manifest.registry.maxEntries))
    {
        error = "schema bounds invalid: manifest layout exceeds mapped_size_bytes";
        return false;
    }

    return true;
}

void useDefaultFlatManifest(IpcSchemaManifest& manifest)
{
    manifest.mappedSizeBytes = 4u * 1024u * 1024u;
    manifest.ring.readIndexOffset = 256;
    manifest.ring.writeIndexOffset = 260;
    manifest.ring.dataOffset = 512;
    manifest.ring.capacityBytes = manifest.mappedSizeBytes - manifest.ring.dataOffset;
    manifest.registry.offset = 264;
    manifest.registry.entrySize = 16;
    manifest.registry.maxEntries = 32;
    manifest.registry.idOffset = 0;
    manifest.registry.nameOffset = 8;
    manifest.registry.nameSize = 8;
}

bool parseFlatManifest(const json& root, IpcSchemaManifest& manifest, std::string& error)
{
    useDefaultFlatManifest(manifest);

    const bool hasMappedSize = root.contains("mapped_size_bytes");
    if (hasMappedSize && !readSizeField(root, "mapped_size_bytes", manifest.mappedSizeBytes, error))
        return false;

    if (root.contains("segment_name_template") && root["segment_name_template"].is_string())
        manifest.segmentNameTemplate = root["segment_name_template"].get<std::string>();

    if (!readOptionalSizeField(root, "frameHdrSize", manifest.frame.headerSize, error))
        return false;

    if (!validateFrameRange(manifest.frame, manifest.frame.headerSize))
    {
        error = "schema bounds invalid: frame header does not contain all required fields";
        return false;
    }

    if (!readOptionalSizeField(root, "readPtrOff", manifest.ring.readIndexOffset, error)
        || !readOptionalSizeField(root, "writePtrOff", manifest.ring.writeIndexOffset, error)
        || !readOptionalSizeField(root, "pluginRegOff", manifest.registry.offset, error)
        || !readOptionalSizeField(root, "ringOff", manifest.ring.dataOffset, error)
        || !readOptionalSizeField(root, "ringSize", manifest.ring.capacityBytes, error)
        || !readOptionalSizeField(root, "entrySize", manifest.registry.entrySize, error)
        || !readOptionalSizeField(root, "maxEntries", manifest.registry.maxEntries, error))
        return false;

    manifest.registry.idOffset = 0;
    manifest.registry.nameOffset = 8;
    if (manifest.registry.entrySize <= manifest.registry.nameOffset)
    {
        error = "manifest registry entrySize is too small for flat layout";
        return false;
    }
    manifest.registry.nameSize = manifest.registry.entrySize - manifest.registry.nameOffset;

    if (root.contains("activeOffset"))
    {
        size_t activeOffset = 0;
        if (!readSizeField(root, "activeOffset", activeOffset, error))
            return false;
        manifest.registry.activeOffset = activeOffset;
    }
    if (root.contains("activeValue") && !readU32Field(root, "activeValue", manifest.registry.activeValue, error))
        return false;

    if (manifest.ring.capacityBytes == 0 || manifest.registry.maxEntries == 0)
    {
        error = "manifest flat ring and registry sizes must be non-zero";
        return false;
    }

    if (!hasMappedSize)
    {
        manifest.mappedSizeBytes = std::max({
            manifest.ring.readIndexOffset + 4,
            manifest.ring.writeIndexOffset + 4,
            manifest.ring.dataOffset + manifest.ring.capacityBytes,
            manifest.registry.offset + manifest.registry.entrySize * manifest.registry.maxEntries
        });
    }

    if (manifest.mappedSizeBytes == 0 || manifest.mappedSizeBytes > kMaxMappedSize)
    {
        error = "schema bounds invalid: manifest mapped_size_bytes must be between 1 and 67108864";
        return false;
    }

    return validateManifestRanges(manifest.mappedSizeBytes, manifest, error);
}

bool parseManifestFile(const std::string& path, IpcSchemaManifest& manifest, std::string& error)
{
    if (path.empty())
    {
        useDefaultFlatManifest(manifest);
        return true;
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

    if (root.contains("readPtrOff") || root.contains("writePtrOff") || root.contains("ringOff"))
        return parseFlatManifest(root, manifest, error);

    if (!root.contains("frame") || !root.contains("registry") || !root.contains("ring")
        || !root.contains("assistant_result") || !root.contains("messages"))
    {
        error = "schema bounds invalid: manifest missing required layout objects";
        return false;
    }

    if (root.contains("mapped_size_bytes")
        && !readSizeField(root, "mapped_size_bytes", manifest.mappedSizeBytes, error))
        return false;

    if (manifest.mappedSizeBytes == 0 || manifest.mappedSizeBytes > kMaxMappedSize)
    {
        error = "schema bounds invalid: manifest mapped_size_bytes must be between 1 and 67108864";
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

    return validateManifestRanges(manifest.mappedSizeBytes, manifest, error);
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

size_t ringDistance(size_t start, size_t end, size_t capacity)
{
    return end >= start ? end - start : capacity - start + end;
}

enum class RingWriteResult
{
    ok,
    invalid,
    full
};

RingWriteResult writeFrameToRing(uint8_t* bytes,
                                 size_t mappedSize,
                                 const RingLayout& ring,
                                 const std::vector<uint8_t>& frame,
                                 uint32_t* publishedWriteIndex = nullptr)
{
    if (frame.empty() || frame.size() > ring.capacityBytes || ring.capacityBytes < 2)
        return RingWriteResult::invalid;
    if (!validateMappedRange(mappedSize, ring.writeIndexOffset, 4)
        || !validateMappedRange(mappedSize, ring.readIndexOffset, 4)
        || !validateMappedRange(mappedSize, ring.dataOffset, ring.capacityBytes))
        return RingWriteResult::invalid;

    const auto readIndex = readU32LE(bytes + ring.readIndexOffset) % static_cast<uint32_t>(ring.capacityBytes);
    const auto writeIndex = readU32LE(bytes + ring.writeIndexOffset) % static_cast<uint32_t>(ring.capacityBytes);
    const auto usedBytes = ringDistance(readIndex, writeIndex, ring.capacityBytes);
    const auto freeBytes = ring.capacityBytes - usedBytes - 1;
    if (frame.size() > freeBytes)
        return RingWriteResult::full;

    for (size_t i = 0; i < frame.size(); ++i)
        bytes[ring.dataOffset + ((writeIndex + i) % ring.capacityBytes)] = frame[i];

    const auto nextWriteIndex = static_cast<uint32_t>((writeIndex + frame.size()) % ring.capacityBytes);
    std::atomic_thread_fence(std::memory_order_release);
    writeU32LE(bytes + ring.writeIndexOffset, nextWriteIndex);
    if (publishedWriteIndex != nullptr)
        *publishedWriteIndex = nextWriteIndex;
    return RingWriteResult::ok;
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

uint32_t readU32LE(const std::array<uint8_t, 4>& bytes)
{
    return readU32LE(bytes.data());
}

json magicSampleAtRingOffset(const uint8_t* bytes,
                             const RingLayout& ring,
                             const FrameLayout& frame,
                             uint32_t ringOffset)
{
    std::array<uint8_t, 4> magicBytes {};
    const auto magicRingOffset = (static_cast<size_t>(ringOffset) + frame.magicOffset) % ring.capacityBytes;
    for (size_t i = 0; i < magicBytes.size(); ++i)
        magicBytes[i] = bytes[ring.dataOffset + ((magicRingOffset + i) % ring.capacityBytes)];

    const auto value = readU32LE(magicBytes);
    return json{
        {"ring_offset", ringOffset},
        {"bytes", json::array({
            static_cast<int>(magicBytes[0]),
            static_cast<int>(magicBytes[1]),
            static_cast<int>(magicBytes[2]),
            static_cast<int>(magicBytes[3])
        })},
        {"value_u32", value},
        {"value_hex", juce::String::formatted("0x%08X", value).toStdString()}
    };
}

json makeMagicProbe(const uint8_t* bytes,
                    const IpcSchemaManifest& manifest,
                    uint32_t requestStartIndex,
                    uint32_t requestWatermark)
{
    const auto readIndex = readU32LE(bytes + manifest.ring.readIndexOffset)
        % static_cast<uint32_t>(manifest.ring.capacityBytes);
    const auto writeIndex = readU32LE(bytes + manifest.ring.writeIndexOffset)
        % static_cast<uint32_t>(manifest.ring.capacityBytes);

    return json{
        {"expected_magic_u32", manifest.frame.magic},
        {"expected_magic_hex", juce::String::formatted("0x%08X", manifest.frame.magic).toStdString()},
        {"read_index", readIndex},
        {"write_index", writeIndex},
        {"request_start_index", requestStartIndex},
        {"request_watermark", requestWatermark},
        {"ring_zero_magic", magicSampleAtRingOffset(bytes, manifest.ring, manifest.frame, 0)},
        {"read_index_magic", magicSampleAtRingOffset(bytes, manifest.ring, manifest.frame, readIndex)},
        {"request_frame_magic", magicSampleAtRingOffset(bytes, manifest.ring, manifest.frame, requestStartIndex)},
        {"next_candidate_magic", magicSampleAtRingOffset(bytes, manifest.ring, manifest.frame, requestWatermark)}
    };
}

struct ParsedFrame
{
    uint16_t messageType = 0;
    uint32_t senderId = 0;
    uint32_t targetId = 0;
    uint32_t payloadSize = 0;
    std::vector<uint8_t> payload;
};

struct ParseFrameOutcome
{
    std::optional<ParsedFrame> frame;
    size_t consumedBytes = 1;
    bool incomplete = false;
    bool oversized = false;
};

ParseFrameOutcome parseFrameAtRingOffset(const uint8_t* bytes,
                                         const RingLayout& ring,
                                         const FrameLayout& layout,
                                         size_t ringOffset,
                                         size_t availableBytes)
{
    ParseFrameOutcome outcome;

    if (layout.headerSize > ring.capacityBytes)
    {
        outcome.oversized = true;
        return outcome;
    }
    if (availableBytes < layout.headerSize)
    {
        outcome.incomplete = true;
        return outcome;
    }

    const auto header = copyFromRing(bytes, ring, ringOffset, layout.headerSize);
    if (!validateFrameRange(layout, header.size()))
        return outcome;
    if (readU32LE(header.data() + layout.magicOffset) != layout.magic)
        return outcome;
    if (readU16LE(header.data() + layout.versionOffset) != layout.version)
        return outcome;

    const auto payloadSize = readU32LE(header.data() + layout.payloadSizeOffset);
    const auto totalSize = layout.headerSize + static_cast<size_t>(payloadSize);
    if (totalSize > ring.capacityBytes)
    {
        outcome.oversized = true;
        return outcome;
    }
    if (availableBytes < totalSize)
    {
        outcome.incomplete = true;
        return outcome;
    }

    ParsedFrame parsed;
    parsed.messageType = readU16LE(header.data() + layout.messageTypeOffset);
    parsed.senderId = readU32LE(header.data() + layout.senderIdOffset);
    parsed.targetId = readU32LE(header.data() + layout.targetIdOffset);
    parsed.payloadSize = payloadSize;
    parsed.payload = copyFromRing(bytes, ring, ringOffset + layout.headerSize, payloadSize);

    outcome.frame = std::move(parsed);
    outcome.consumedBytes = totalSize;
    return outcome;
}

double roundedJsonValue(float value)
{
    return std::round(static_cast<double>(value) * 1000000.0) / 1000000.0;
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
        const auto value = roundedJsonValue(readF32LE(payload.data() + entryBase + layout.valueOffset));
        params.push_back({{"index", paramIndex}, {"value", value}});
    }

    return params;
}

bool clearRegistryObserver(uint8_t* bytes,
                           size_t mappedSize,
                           const RegistryLayout& registry,
                           uint32_t observerId)
{
    if (!validateMappedRange(mappedSize, registry.offset, registry.entrySize * registry.maxEntries))
        return false;

    bool cleared = false;
    for (size_t i = 0; i < registry.maxEntries; ++i)
    {
        auto* entry = bytes + registry.offset + i * registry.entrySize;
        if (readU32LE(entry + registry.idOffset) != observerId)
            continue;

        std::fill(entry, entry + registry.entrySize, static_cast<uint8_t>(0));
        cleared = true;
    }

    return cleared;
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

    return writeFrameToRing(bytes, mappedSize, manifest.ring, frame) == RingWriteResult::ok;
}

std::optional<json> findAssistantResult(const uint8_t* bytes,
                                        const IpcSchemaManifest& manifest,
                                        uint32_t ozoneId,
                                        uint32_t observerId,
                                        uint32_t requestWatermark,
                                        std::string& error)
{
    if (!validateMappedRange(manifest.mappedSizeBytes, manifest.ring.readIndexOffset, 4)
        || !validateMappedRange(manifest.mappedSizeBytes, manifest.ring.writeIndexOffset, 4))
    {
        error = "schema bounds invalid: ring pointer offsets exceed mapped segment.";
        return std::nullopt;
    }

    // Ozone owns the ring read pointer. The MCP server only observes the
    // immutable post-request window and never writes readIndexOffset.
    juce::ignoreUnused(readU32LE(bytes + manifest.ring.readIndexOffset));
    const auto scanStart = requestWatermark % static_cast<uint32_t>(manifest.ring.capacityBytes);
    const auto writeIndex = readU32LE(bytes + manifest.ring.writeIndexOffset)
        % static_cast<uint32_t>(manifest.ring.capacityBytes);
    const auto available = ringDistance(scanStart, writeIndex, manifest.ring.capacityBytes);

    size_t scanned = 0;
    while (scanned < available)
    {
        const auto offset = (static_cast<size_t>(scanStart) + scanned) % manifest.ring.capacityBytes;
        const auto remaining = available - scanned;
        auto parsed = parseFrameAtRingOffset(bytes, manifest.ring, manifest.frame, offset, remaining);

        if (parsed.oversized)
        {
            error = "oversized AssistantResult frame payload claim exceeds the IPC ring capacity.";
            return std::nullopt;
        }
        if (parsed.incomplete)
            return std::nullopt;
        if (!parsed.frame)
        {
            scanned += 1;
            continue;
        }

        const auto consumedBytes = std::max<size_t>(1, parsed.consumedBytes);
        if (parsed.frame->messageType == manifest.messages.assistantResult
            && parsed.frame->senderId == ozoneId
            && (parsed.frame->targetId == observerId || parsed.frame->targetId == kBroadcastTarget || parsed.frame->targetId == 0))
        {
            auto params = parseAssistantResultPayload(parsed.frame->payload, manifest.assistantResult, error);
            if (!error.empty())
                return std::nullopt;

            return json{
                {"source_instance_id", parsed.frame->senderId},
                {"target_instance_id", parsed.frame->targetId},
                {"parameter_count", params.size()},
                {"parameters", params}
            };
        }

        scanned += consumedBytes;
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
            "Active iZotope IPC write is blocked; set MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1 and allow_unsafe_write=true to enable it.");
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

    const auto requestStartIndex = readU32LE(attached.bytes + manifest.ring.writeIndexOffset)
        % static_cast<uint32_t>(manifest.ring.capacityBytes);
    uint32_t requestWatermark = 0;
    const auto requestWriteResult = writeFrameToRing(
        attached.bytes,
        attached.size,
        manifest.ring,
        requestFrame,
        &requestWatermark);
    if (requestWriteResult == RingWriteResult::full)
        return makeToolError("ipc_ring_full", "Could not write AssistantRequest frame because the manifest-defined ring buffer is full.");
    if (requestWriteResult != RingWriteResult::ok)
        return makeToolError("assistant_request_write_failed", "Could not write AssistantRequest frame to manifest-defined ring buffer.");

    std::optional<json> magicProbe;
    if (envEnabled("MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC"))
    {
        magicProbe = makeMagicProbe(attached.bytes, manifest, requestStartIndex, requestWatermark);
        juce::Logger::writeToLog("MorePhi iZotope IPC magic probe: "
            + juce::String(magicProbe->dump()));
    }

    const auto start = std::chrono::steady_clock::now();
    auto elapsedMs = [&]() -> size_t
    {
        return static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    };

    const bool singleShot = args.timeoutMs == 0;
    const auto deadline = start + std::chrono::milliseconds(args.timeoutMs);

    for (;;)
    {
        std::string parseError;
        if (auto result = findAssistantResult(attached.bytes, manifest, *ozoneId, args.observerId, requestWatermark, parseError))
        {
            const bool observerDeregisterSent = writeObserverDeregister(
                attached.bytes,
                attached.size,
                manifest,
                args.observerId,
                *ozoneId);
            clearRegistryObserver(attached.bytes, attached.size, manifest.registry, args.observerId);

            auto successBody = json{
                {"success", true},
                {"segment_name", segmentName},
                {"schema_path", schemaPath},
                {"platform", attached.fake ? "test" : platformName()},
                {"observer_id", args.observerId},
                {"observer_deregister_sent", observerDeregisterSent},
                {"ozone_instance_id", *ozoneId},
                {"elapsed_ms", elapsedMs()},
                {"assistant_result", *result},
                {"parameters", result->value("parameters", json::array())}
            };
            if (magicProbe)
                successBody["ipc_magic_probe"] = *magicProbe;
            return {std::move(successBody), false};
        }

        if (!parseError.empty())
        {
            auto error = makeToolError("assistant_result_parse_failed", parseError);
            if (magicProbe)
                error.body["ipc_magic_probe"] = *magicProbe;
            return error;
        }

        if (singleShot)
            break;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;

        const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const auto sleepMs = std::min<size_t>(
            std::max<size_t>(1, args.pollIntervalMs),
            static_cast<size_t>(std::max<int64_t>(1, remainingMs)));
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    auto timeout = makeToolError("assistant_timeout", "timeout waiting for AssistantResult frame after " + std::to_string(args.timeoutMs) + " ms.");
    if (magicProbe)
        timeout.body["ipc_magic_probe"] = *magicProbe;
    return timeout;
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
