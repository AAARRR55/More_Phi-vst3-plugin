#pragma once

#include "IZotopeIPCAssistant.h"
#include "IZotopeIPCDiscovery.h"
#include "OzonePluginBackend.h"

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace more_phi::standalone_mcp {

struct ToolDescriptor
{
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json inputSchema;
    nlohmann::json annotations;
};

class StandaloneMcpServer
{
public:
    explicit StandaloneMcpServer(
        std::unique_ptr<OzonePluginBackend> backend = createOzonePluginBackend(),
        std::unique_ptr<IZotopeIPCDiscovery> ipcDiscovery = createIZotopeIPCDiscovery(),
        std::unique_ptr<IZotopeIPCAssistant> ipcAssistant = createIZotopeIPCAssistant());

    nlohmann::json processJson(const nlohmann::json& request);
    std::optional<nlohmann::json> processLine(const std::string& line);
    void run(std::istream& in, std::ostream& out);

    const std::vector<ToolDescriptor>& getTools() const noexcept { return tools; }

private:
    nlohmann::json handleInitialize(const nlohmann::json& id);
    nlohmann::json handleToolsList(const nlohmann::json& id) const;
    nlohmann::json handleToolsCall(const nlohmann::json& id, const nlohmann::json& params);
    nlohmann::json descriptorToJson(const ToolDescriptor& tool) const;

    static nlohmann::json invalidParams(const nlohmann::json& id, const std::string& message);

    std::unique_ptr<OzonePluginBackend> backend;
    std::unique_ptr<IZotopeIPCDiscovery> ipcDiscovery;
    std::unique_ptr<IZotopeIPCAssistant> ipcAssistant;
    std::vector<ToolDescriptor> tools;
    std::mutex writeMutex;
    bool initialized = false;
};

} // namespace more_phi::standalone_mcp
