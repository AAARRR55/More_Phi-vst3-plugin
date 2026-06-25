#pragma once

#include "MorePhiPluginBackend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace more_phi::standalone_mcp {

struct IpcAssistantRunArgs
{
    std::optional<std::string> schemaPath;
    std::optional<std::string> segmentName;
    std::optional<uint32_t> dawProcessId;
    std::optional<uint32_t> instanceId;
    std::string pluginNameQuery;
    size_t timeoutMs = 10000;
    size_t pollIntervalMs = 10;
    uint32_t observerId = 0xDEADBEEFu;
    bool allowUnsafeWrite = false;
};

class MorePhiIPCAssistant
{
public:
    MorePhiIPCAssistant() = default;
    ~MorePhiIPCAssistant();

    ToolCallOutcome runAssistant(const IpcAssistantRunArgs& args);

    void setFakeSegmentForTests(std::string name, std::vector<uint8_t> bytes);
    const std::vector<uint8_t>* getFakeSegmentForTests(const std::string& name) const;

private:
    std::unordered_map<std::string, std::vector<uint8_t>> fakeSegments;
};

std::unique_ptr<MorePhiIPCAssistant> createMorePhiIPCAssistant();

} // namespace more_phi::standalone_mcp
