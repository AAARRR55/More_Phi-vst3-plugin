#pragma once

#include "OzonePluginBackend.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#if JUCE_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif JUCE_MAC || JUCE_LINUX
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace more_phi::standalone_mcp {

struct IpcAttachArgs
{
    std::optional<std::string> segmentName;
    std::optional<uint32_t> dawProcessId;
    size_t mappedSizeBytes = 4u * 1024u * 1024u;
};

struct IpcSnapshotArgs
{
    size_t offset = 0;
    size_t sizeBytes = 1024;
    size_t maxFrames = 16;
};

struct IpcDumpArgs
{
    std::string outputPath;
    size_t offset = 0;
    size_t sizeBytes = 64u * 1024u;
};

struct IpcCaptureArgs
{
    size_t offset = 0;
    size_t sizeBytes = 4096;
    size_t durationMs = 2000;
    size_t intervalMs = 25;
    size_t maxChanges = 64;
    size_t maxRangesPerChange = 64;
    size_t maxFrames = 16;
    bool includeChangedBytes = false;
    std::optional<std::string> baselineBase64;
    std::optional<std::string> outputPath;
};

class IZotopeIPCDiscovery
{
public:
    IZotopeIPCDiscovery() = default;
    ~IZotopeIPCDiscovery();

    ToolCallOutcome attach(const IpcAttachArgs& args);
    ToolCallOutcome detach();
    ToolCallOutcome status() const;
    ToolCallOutcome snapshot(const IpcSnapshotArgs& args) const;
    ToolCallOutcome dump(const IpcDumpArgs& args) const;
    ToolCallOutcome capture(const IpcCaptureArgs& args) const;

    void setFakeSegmentForTests(std::string name, std::vector<uint8_t> bytes);

private:
    static constexpr size_t kDefaultMappedSize = 4u * 1024u * 1024u;
    static constexpr size_t kMaxMappedSize = 64u * 1024u * 1024u;
    static constexpr size_t kMaxSnapshotSize = 256u * 1024u;
    static constexpr size_t kMaxDumpSize = 16u * 1024u * 1024u;
    static constexpr size_t kMaxCaptureSize = 1024u * 1024u;
    static constexpr size_t kMaxCaptureDurationMs = 60u * 1000u;
    static constexpr size_t kMaxCaptureChanges = 1000;
    static constexpr size_t kMaxChangeRanges = 512;
    static constexpr uint32_t kMagicIzot = 0x495A4F54u;

    const uint8_t* mappedBytes = nullptr;
    size_t mappedSize = 0;
    std::string attachedSegmentName;
    std::string lastError;
    bool attachedFromFakeMemory = false;

    std::vector<uint8_t> fakeAttachedBytes;
    std::unordered_map<std::string, std::vector<uint8_t>> fakeSegments;

#if JUCE_WINDOWS
    HANDLE mappingHandle = nullptr;
#elif JUCE_MAC || JUCE_LINUX
    int shmFd = -1;
#endif

    void clearMapping();
    bool isAttached() const noexcept { return mappedBytes != nullptr && mappedSize > 0; }
    std::string resolveSegmentName(const IpcAttachArgs& args) const;
    ToolCallOutcome attachRealReadOnly(const std::string& segmentName, size_t requestedBytes);
    ToolCallOutcome attachFake(const std::string& segmentName);
    ToolCallOutcome rangeError(const char* operation, size_t offset, size_t sizeBytes) const;
    nlohmann::json frameCandidates(size_t absoluteOffset,
                                   const uint8_t* bytes,
                                   size_t sizeBytes,
                                   size_t maxFrames) const;
};

std::unique_ptr<IZotopeIPCDiscovery> createIZotopeIPCDiscovery();

} // namespace more_phi::standalone_mcp
