#include "IZotopeIPCDiscovery.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace more_phi::standalone_mcp {

using json = nlohmann::json;

namespace {

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

std::string platformName()
{
#if JUCE_WINDOWS
    return "windows";
#elif JUCE_MAC
    return "macos";
#elif JUCE_LINUX
    return "linux";
#else
    return "unknown";
#endif
}

std::string hexEncode(const uint8_t* data, size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i)
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    return out.str();
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

juce::String standardBase64(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
        return {};
    return juce::Base64::toBase64(data, size);
}

} // namespace

IZotopeIPCDiscovery::~IZotopeIPCDiscovery()
{
    clearMapping();
}

void IZotopeIPCDiscovery::clearMapping()
{
    attachedFromFakeMemory = false;
    fakeAttachedBytes.clear();

#if JUCE_WINDOWS
    if (mappedBytes != nullptr)
        UnmapViewOfFile(mappedBytes);
    if (mappingHandle != nullptr)
        CloseHandle(mappingHandle);
    mappingHandle = nullptr;
#elif JUCE_MAC || JUCE_LINUX
    if (mappedBytes != nullptr)
        munmap(const_cast<uint8_t*>(mappedBytes), mappedSize);
    if (shmFd >= 0)
        close(shmFd);
    shmFd = -1;
#endif

    mappedBytes = nullptr;
    mappedSize = 0;
    attachedSegmentName.clear();
}

std::string IZotopeIPCDiscovery::resolveSegmentName(const IpcAttachArgs& args) const
{
    if (args.segmentName && !args.segmentName->empty())
        return *args.segmentName;

    const auto envName = envString("IZOTOPE_IPC_SEGMENT_NAME");
    if (!envName.empty())
        return envName;

    if (args.dawProcessId)
    {
#if JUCE_WINDOWS
        return "Global\\iZotope_IPC_Session_" + std::to_string(*args.dawProcessId);
#else
        return "/izotope_ipc_" + std::to_string(*args.dawProcessId);
#endif
    }

    return {};
}

ToolCallOutcome IZotopeIPCDiscovery::attach(const IpcAttachArgs& args)
{
    clearMapping();
    lastError.clear();

    const auto segmentName = resolveSegmentName(args);
    if (segmentName.empty())
    {
        lastError = "segment_name, IZOTOPE_IPC_SEGMENT_NAME, or daw_process_id is required.";
        return makeToolError("missing_segment_name", lastError);
    }

    const auto requestedBytes = std::clamp(
        args.mappedSizeBytes == 0 ? kDefaultMappedSize : args.mappedSizeBytes,
        static_cast<size_t>(1),
        kMaxMappedSize);

    if (auto fake = attachFake(segmentName); !fake.isError)
        return fake;

    return attachRealReadOnly(segmentName, requestedBytes);
}

ToolCallOutcome IZotopeIPCDiscovery::attachFake(const std::string& segmentName)
{
    const auto it = fakeSegments.find(segmentName);
    if (it == fakeSegments.end())
        return makeToolError("fake_segment_not_found");

    fakeAttachedBytes = it->second;
    if (fakeAttachedBytes.empty())
        return makeToolError("empty_fake_segment");

    mappedBytes = fakeAttachedBytes.data();
    mappedSize = fakeAttachedBytes.size();
    attachedSegmentName = segmentName;
    attachedFromFakeMemory = true;

    return {json{
        {"success", true},
        {"attached", true},
        {"segment_name", attachedSegmentName},
        {"mapped_size_bytes", mappedSize},
        {"platform", "test"},
        {"read_only", true}
    }, false};
}

ToolCallOutcome IZotopeIPCDiscovery::attachRealReadOnly(const std::string& segmentName, size_t requestedBytes)
{
#if JUCE_WINDOWS
    const auto wideName = juce::String(segmentName).toWideCharPointer();
    mappingHandle = OpenFileMappingW(FILE_MAP_READ, FALSE, wideName);
    if (mappingHandle == nullptr)
    {
        lastError = "OpenFileMappingW failed for read-only segment.";
        return makeToolError("attach_failed", lastError);
    }

    mappedBytes = static_cast<const uint8_t*>(
        MapViewOfFile(mappingHandle, FILE_MAP_READ, 0, 0, requestedBytes));
    if (mappedBytes == nullptr)
    {
        lastError = "MapViewOfFile failed for read-only segment.";
        clearMapping();
        return makeToolError("attach_failed", lastError);
    }

    mappedSize = requestedBytes;
#elif JUCE_MAC || JUCE_LINUX
    const auto path = segmentName.rfind("/", 0) == 0
        ? segmentName
        : "/" + segmentName;

    shmFd = shm_open(path.c_str(), O_RDONLY, 0666);
    if (shmFd < 0)
    {
        lastError = "shm_open failed for read-only segment.";
        return makeToolError("attach_failed", lastError);
    }

    struct stat st {};
    if (fstat(shmFd, &st) != 0 || st.st_size <= 0)
    {
        lastError = "fstat failed or segment has no readable size.";
        clearMapping();
        return makeToolError("attach_failed", lastError);
    }

    mappedSize = std::min(requestedBytes, static_cast<size_t>(st.st_size));
    mappedBytes = static_cast<const uint8_t*>(
        mmap(nullptr, mappedSize, PROT_READ, MAP_SHARED, shmFd, 0));
    if (mappedBytes == MAP_FAILED)
    {
        mappedBytes = nullptr;
        lastError = "mmap failed for read-only segment.";
        clearMapping();
        return makeToolError("attach_failed", lastError);
    }
#else
    juce::ignoreUnused(segmentName, requestedBytes);
    lastError = "Platform does not support iZotope IPC shared-memory attach.";
    return makeToolError("unsupported_platform", lastError);
#endif

    attachedSegmentName = segmentName;
    return {json{
        {"success", true},
        {"attached", true},
        {"segment_name", attachedSegmentName},
        {"mapped_size_bytes", mappedSize},
        {"platform", platformName()},
        {"read_only", true}
    }, false};
}

ToolCallOutcome IZotopeIPCDiscovery::detach()
{
    const bool wasAttached = isAttached();
    clearMapping();
    return {json{{"success", true}, {"was_attached", wasAttached}, {"attached", false}}, false};
}

ToolCallOutcome IZotopeIPCDiscovery::status() const
{
    return {json{
        {"success", true},
        {"attached", isAttached()},
        {"segment_name", attachedSegmentName},
        {"mapped_size_bytes", mappedSize},
        {"platform", attachedFromFakeMemory ? "test" : platformName()},
        {"read_only", true},
        {"last_error", lastError}
    }, false};
}

ToolCallOutcome IZotopeIPCDiscovery::rangeError(const char* operation, size_t offset, size_t sizeBytes) const
{
    return makeToolError("invalid_range",
        std::string(operation) + " range is outside the mapped segment: offset="
        + std::to_string(offset) + ", size_bytes=" + std::to_string(sizeBytes)
        + ", mapped_size_bytes=" + std::to_string(mappedSize));
}

json IZotopeIPCDiscovery::frameCandidates(size_t absoluteOffset,
                                          const uint8_t* bytes,
                                          size_t sizeBytes,
                                          size_t maxFrames) const
{
    static constexpr size_t headerSize = 28;

    json candidates = json::array();
    if (bytes == nullptr || sizeBytes < headerSize || maxFrames == 0)
        return candidates;

    for (size_t i = 0; i + headerSize <= sizeBytes && candidates.size() < maxFrames; ++i)
    {
        const auto* p = bytes + i;
        const auto magic = readU32LE(p);
        if (magic != kMagicIzot)
            continue;

        const auto version = readU16LE(p + 4);
        const auto messageType = readU16LE(p + 6);
        const auto senderId = readU32LE(p + 8);
        const auto targetId = readU32LE(p + 12);
        const auto payloadSize = readU32LE(p + 16);
        const auto timestamp = readU64LE(p + 20);
        const auto totalSize = headerSize + static_cast<size_t>(payloadSize);

        if (version == 0 || version > 64)
            continue;
        if (payloadSize > mappedSize || i + totalSize > sizeBytes)
            continue;

        candidates.push_back({
            {"offset", absoluteOffset + i},
            {"magic", "IZOT"},
            {"version", version},
            {"message_type", messageType},
            {"sender_id", senderId},
            {"target_id", targetId},
            {"payload_size", payloadSize},
            {"timestamp", timestamp},
            {"total_size", totalSize}
        });
    }

    return candidates;
}

ToolCallOutcome IZotopeIPCDiscovery::snapshot(const IpcSnapshotArgs& args) const
{
    if (!isAttached())
        return makeToolError("not_attached", "Attach to an iZotope IPC segment before snapshot.");

    if (args.sizeBytes == 0 || args.sizeBytes > kMaxSnapshotSize)
        return makeToolError("invalid_range", "snapshot size_bytes must be between 1 and 262144.");

    if (args.offset > mappedSize || args.sizeBytes > mappedSize - args.offset)
        return rangeError("snapshot", args.offset, args.sizeBytes);

    const auto* begin = mappedBytes + args.offset;
    return {json{
        {"success", true},
        {"segment_name", attachedSegmentName},
        {"offset", args.offset},
        {"size_bytes", args.sizeBytes},
        {"data_hex", hexEncode(begin, args.sizeBytes)},
        {"data_base64", standardBase64(begin, args.sizeBytes).toStdString()},
        {"frame_candidates", frameCandidates(args.offset, begin, args.sizeBytes, args.maxFrames)}
    }, false};
}

ToolCallOutcome IZotopeIPCDiscovery::dump(const IpcDumpArgs& args) const
{
    if (!isAttached())
        return makeToolError("not_attached", "Attach to an iZotope IPC segment before dump.");

    if (args.outputPath.empty())
        return makeToolError("missing_output_path");
    if (args.sizeBytes == 0 || args.sizeBytes > kMaxDumpSize)
        return makeToolError("invalid_range", "dump size_bytes must be between 1 and 16777216.");
    if (args.offset > mappedSize || args.sizeBytes > mappedSize - args.offset)
        return rangeError("dump", args.offset, args.sizeBytes);

    const juce::File outputFile{juce::String(args.outputPath)};
    if (auto parent = outputFile.getParentDirectory(); parent.getFullPathName().isNotEmpty())
        parent.createDirectory();

    auto stream = std::unique_ptr<juce::FileOutputStream>(outputFile.createOutputStream());
    if (stream == nullptr || !stream->openedOk())
        return makeToolError("dump_open_failed", "Could not open output_path for writing.");

    const auto* begin = mappedBytes + args.offset;
    if (!stream->write(begin, args.sizeBytes))
        return makeToolError("dump_write_failed");

    stream->flush();
    return {json{
        {"success", true},
        {"segment_name", attachedSegmentName},
        {"output_path", outputFile.getFullPathName().toStdString()},
        {"offset", args.offset},
        {"size_bytes", args.sizeBytes},
        {"exists", outputFile.existsAsFile()}
    }, false};
}

#if MORE_PHI_TEST_MODE
void IZotopeIPCDiscovery::setFakeSegmentForTests(std::string name, std::vector<uint8_t> bytes)
{
    fakeSegments[std::move(name)] = std::move(bytes);
}
#endif

std::unique_ptr<IZotopeIPCDiscovery> createIZotopeIPCDiscovery()
{
    return std::make_unique<IZotopeIPCDiscovery>();
}

} // namespace more_phi::standalone_mcp
