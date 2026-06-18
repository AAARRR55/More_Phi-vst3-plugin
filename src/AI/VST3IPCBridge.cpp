/*
 * More-Phi — AI/VST3IPCBridge.cpp
 * Cross-platform IPC bridge between the Python stdio MCP server and More-Phi.
 *
 * Transport: Windows named pipe or POSIX Unix domain socket.
 * Protocol: fixed-size little-endian headers + payload.
 */
#include "AI/VST3IPCBridge.h"
#include "Plugin/PluginProcessor.h"
#include "Host/PluginHostManager.h"
#include "Host/ParameterBridge.h"

#include <nlohmann/json.hpp>

#if JUCE_PLUGINHOST_VST3
    #include <juce_audio_processors/format_types/VST3_SDK/pluginterfaces/base/funknown.h>
    #include <juce_audio_processors/format_types/VST3_SDK/pluginterfaces/vst/ivstcomponent.h>
    #include <juce_audio_processors/format_types/VST3_SDK/pluginterfaces/vst/ivsteditcontroller.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

namespace more_phi {

namespace {

constexpr int kStopTimeoutMs = 500;
constexpr int kReadTimeoutMs = 2000;
constexpr size_t kMaxPayloadBytes = 16 * 1024 * 1024; // 16 MB safety cap

uint64_t nowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double clampNormalized(double v) noexcept
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/// RAII exclusive-plugin-use guard. The IPC bridge runs executeCommand on the
/// message thread, but hosted-plugin load/unload can run concurrently on the MCP
/// TCP connection thread, so any raw hosted-plugin pointer we dereference must
/// be held alive by an exclusive lease (PluginHostManager::unloadPlugin waits on
/// exclusivePluginUseRequested_ before destroying the instance).
struct ScopedExclusivePluginUse
{
    explicit ScopedExclusivePluginUse(PluginHostManager& h) noexcept : host(h) {}
    ~ScopedExclusivePluginUse() noexcept { host.endExclusivePluginUse(); }
    PluginHostManager& host;
};

#if JUCE_PLUGINHOST_VST3
std::string convertVstString(const Steinberg::Vst::String128 src) noexcept
{
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 128 && src[i] != 0; ++i)
    {
        const char16_t c = src[i];
        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

Steinberg::Vst::IEditController* queryEditController(juce::AudioPluginInstance* plugin)
{
    if (plugin == nullptr)
        return nullptr;

    #ifdef _MSC_VER
        #pragma warning(push)
        #pragma warning(disable: 4996)
    #endif
    void* const platform = plugin->getPlatformSpecificData();
    #ifdef _MSC_VER
        #pragma warning(pop)
    #endif

    if (platform == nullptr)
        return nullptr;

    auto* const component = static_cast<Steinberg::Vst::IComponent*>(platform);
    Steinberg::FUnknownPtr<Steinberg::Vst::IEditController> editController(component);
    return editController;
}
#endif

} // namespace

//==============================================================================
// Platform-specific listener/reader thread implementation.
//==============================================================================
class VST3IPCBridge::Impl : private juce::Thread
{
public:
    explicit Impl(VST3IPCBridge& owner)
        : juce::Thread("VST3 IPC Bridge"),
          owner_(owner)
    {
    }

    ~Impl() override
    {
        stop();
    }

    void start(const juce::String& endpoint)
    {
        stop();
        endpoint_ = endpoint;
        stopRequested_.store(false);
        startThread(juce::Thread::Priority::normal);
    }

    void stop()
    {
        stopRequested_.store(true);
        closeTransport();
        stopThread(-1);
    }

    bool writeResult(const uint8_t* data, size_t size)
    {
        if (data == nullptr || size == 0)
            return true;

#ifdef _WIN32
        HANDLE h = activeHandle_.load();
        if (h == INVALID_HANDLE_VALUE)
            return false;

        size_t written = 0;
        while (written < size)
        {
            DWORD chunk = 0;
            const DWORD toWrite = static_cast<DWORD>(std::min<size_t>(size - written,
                                                                       static_cast<size_t>(std::numeric_limits<DWORD>::max())));
            if (!WriteFile(h, data + written, toWrite, &chunk, nullptr) || chunk == 0)
                return false;
            written += chunk;
        }
        return true;
#else
        int fd = clientFd_.load();
        if (fd < 0)
            return false;

        size_t written = 0;
        while (written < size)
        {
            const ssize_t n = ::send(fd, data + written, size - written, MSG_NOSIGNAL);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            written += static_cast<size_t>(n);
        }
        return true;
#endif
    }

private:
    void run() override
    {
        owner_.running_.store(true);

#ifdef _WIN32
        runWindows();
#else
        runPosix();
#endif

        owner_.running_.store(false);
    }

#ifdef _WIN32
    void runWindows()
    {
        while (!stopRequested_.load())
        {
            const HANDLE hServer = createNamedPipe(owner_.getEndpointPath());
            if (hServer == INVALID_HANDLE_VALUE)
            {
                juce::Thread::sleep(100);
                continue;
            }

            activeHandle_.store(hServer);

            const BOOL connected = ConnectNamedPipe(hServer, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

            if (stopRequested_.load())
            {
                closeActiveHandle();
                break;
            }

            if (!connected)
            {
                closeActiveHandle();
                continue;
            }

            readLoop(hServer);
            closeActiveHandle();
        }
    }

    static HANDLE createNamedPipe(const juce::String& name)
    {
        return CreateNamedPipeW(name.toWideCharPointer(),
                                PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                1,
                                65536,
                                65536,
                                0,
                                nullptr);
    }

    void closeActiveHandle()
    {
        const HANDLE h = activeHandle_.exchange(INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(h);
            CloseHandle(h);
        }
    }

    void closeTransport()
    {
        // Wake up ConnectNamedPipe if it is blocking
        const juce::String pipeName = owner_.getEndpointPath();
        HANDLE hClient = CreateFileW(
            pipeName.toWideCharPointer(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hClient != INVALID_HANDLE_VALUE)
        {
            CloseHandle(hClient);
        }

        closeActiveHandle();
    }

    static bool readExact(HANDLE h, uint8_t* dest, size_t size)
    {
        size_t done = 0;
        while (done < size)
        {
            DWORD chunk = 0;
            const DWORD toRead = static_cast<DWORD>(std::min<size_t>(size - done,
                                                                      static_cast<size_t>(std::numeric_limits<DWORD>::max())));
            if (!ReadFile(h, dest + done, toRead, &chunk, nullptr) || chunk == 0)
                return false;
            done += chunk;
        }
        return true;
    }

    void readLoop(HANDLE hPipe)
    {
        while (!stopRequested_.load())
        {
            CommandPacketHeader header{};
            if (!readExact(hPipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)))
                break;

            if (header.payload_length > kMaxPayloadBytes)
                break;

            CommandPacket command;
            command.header = header;
            if (header.payload_length > 0)
            {
                command.payload.resize(header.payload_length);
                if (!readExact(hPipe, command.payload.data(), header.payload_length))
                    break;
            }

            juce::WeakReference<VST3IPCBridge> weakRef(&owner_);
            juce::MessageManager::callAsync([weakRef, command]() mutable
            {
                if (auto* bridge = weakRef.get())
                {
                    auto result = bridge->executeCommand(command);
                    bridge->sendResult(std::move(result));
                }
            });
        }
    }

    std::atomic<HANDLE> activeHandle_{ INVALID_HANDLE_VALUE };
#else
    void runPosix()
    {
        const juce::String endpoint = owner_.getEndpointPath();
        const int serverFd = createUnixSocket(endpoint);
        if (serverFd < 0)
            return;

        serverFd_.store(serverFd);

        while (!stopRequested_.load())
        {
            const int client = ::accept(serverFd, nullptr, nullptr);
            if (stopRequested_.load() || client < 0)
            {
                if (client >= 0)
                    ::close(client);
                break;
            }

            clientFd_.store(client);
            readLoop(client);
            ::close(client);
            clientFd_.store(-1);
        }

        const int sfd = serverFd_.exchange(-1);
        if (sfd >= 0)
        {
            ::close(sfd);
            juce::File(endpoint).deleteFile();
        }
    }

    static int createUnixSocket(const juce::String& path)
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        ::unlink(path.toUTF8());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.toUTF8(), sizeof(addr.sun_path) - 1);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }

        if (::listen(fd, 1) < 0)
        {
            ::close(fd);
            juce::File(path).deleteFile();
            return -1;
        }

        return fd;
    }

    void closeTransport()
    {
        const int sfd = serverFd_.exchange(-1);
        if (sfd >= 0)
            ::close(sfd);

        const int cfd = clientFd_.exchange(-1);
        if (cfd >= 0)
            ::close(cfd);

        juce::File(owner_.getEndpointPath()).deleteFile();
    }

    static bool readExact(int fd, uint8_t* dest, size_t size, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                              + static_cast<juce::uint32>(timeoutMs);
        size_t done = 0;

        while (done < size)
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= deadline)
                return false;

            pollfd pfd{ fd, POLLIN, 0 };
            const int pollTimeout = static_cast<int>(deadline - now);
            const int ready = ::poll(&pfd, 1, pollTimeout);
            if (ready <= 0)
                return false;

            const ssize_t n = ::read(fd, dest + done, size - done);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }

    void readLoop(int fd)
    {
        while (!stopRequested_.load())
        {
            CommandPacketHeader header{};
            if (!readExact(fd, reinterpret_cast<uint8_t*>(&header), sizeof(header), kReadTimeoutMs))
                break;

            if (header.payload_length > kMaxPayloadBytes)
                break;

            CommandPacket command;
            command.header = header;
            if (header.payload_length > 0)
            {
                command.payload.resize(header.payload_length);
                if (!readExact(fd, command.payload.data(), header.payload_length, kReadTimeoutMs))
                    break;
            }

            juce::WeakReference<VST3IPCBridge> weakRef(&owner_);
            juce::MessageManager::callAsync([weakRef, command]() mutable
            {
                if (auto* bridge = weakRef.get())
                {
                    auto result = bridge->executeCommand(command);
                    bridge->sendResult(std::move(result));
                }
            });
        }
    }

    std::atomic<int> serverFd_{ -1 };
    std::atomic<int> clientFd_{ -1 };
#endif

    VST3IPCBridge& owner_;
    juce::String endpoint_;
    std::atomic<bool> stopRequested_{false};
};

//==============================================================================
// VST3IPCBridge public interface.
//==============================================================================
VST3IPCBridge::VST3IPCBridge(MorePhiProcessor& processor, const InstanceIdentity& identity)
    : processor_(processor),
      identity_(identity)
{
    static_assert(kCommandHeaderSize == 21, "CommandPacketHeader must pack to 21 bytes");
    static_assert(kResultHeaderSize  == 33, "ResultPacketHeader must pack to 33 bytes");
}

VST3IPCBridge::~VST3IPCBridge()
{
    stop();
}

void VST3IPCBridge::start()
{
    if (isRunning())
        return;

    stopRequested_.store(false);
    if (impl_ == nullptr)
        impl_ = std::make_unique<Impl>(*this);

    impl_->start(getEndpointPath());
}

void VST3IPCBridge::stop()
{
    stopRequested_.store(true);
    if (impl_ != nullptr)
        impl_->stop();
    running_.store(false);
}

juce::String VST3IPCBridge::getEndpointPath() const
{
#if JUCE_WINDOWS
    return "\\\\.\\pipe\\more_phi_vst3_mcp_" + identity_.instanceId;
#else
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
           .getChildFile("more_phi_vst3_mcp_" + identity_.instanceId + ".sock")
           .getFullPathName();
#endif
}

juce::File VST3IPCBridge::getParameterRegistryPath() const
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
           .getChildFile("more_phi_vst3_mcp_" + identity_.instanceId + "_registry.json");
}

bool VST3IPCBridge::exportParameterRegistry()
{
    using json = nlohmann::json;
    json registry = json::array();

#if JUCE_PLUGINHOST_VST3
    if (auto* plugin = processor_.getHostManager().getPlugin())
    {
        if (auto* editController = queryEditController(plugin))
        {
            const auto count = editController->getParameterCount();
            for (Steinberg::int32 i = 0; i < count; ++i)
            {
                Steinberg::Vst::ParameterInfo info{};
                if (editController->getParameterInfo(i, info) != Steinberg::kResultOk)
                    continue;

                json entry;
                entry["index"] = i;
                entry["id"] = info.id;
                entry["name"] = convertVstString(info.title);
                entry["shortName"] = convertVstString(info.shortTitle);
                entry["units"] = convertVstString(info.units);
                entry["defaultValue"] = info.defaultNormalizedValue;
                entry["stepCount"] = info.stepCount;
                entry["discrete"] = info.stepCount != 0;
                entry["boolean"] = info.stepCount == 1;
                entry["stableId"] = juce::String(info.id).toStdString();
                registry.push_back(entry);
            }
        }
        else
        {
            for (const auto& descriptor : processor_.getParameterBridge().getParameterDescriptors())
            {
                json entry;
                entry["index"] = descriptor.index;
                entry["id"] = descriptor.index;
                entry["name"] = descriptor.name.toStdString();
                entry["units"] = descriptor.label.toStdString();
                entry["defaultValue"] = descriptor.defaultValue;
                entry["stepCount"] = descriptor.numSteps;
                entry["discrete"] = descriptor.discrete;
                entry["boolean"] = descriptor.boolean;
                entry["stableId"] = descriptor.stableId.toStdString();
                registry.push_back(entry);
            }
        }
    }
#endif

    try
    {
        getParameterRegistryPath().replaceWithText(juce::String(registry.dump(2)));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

ResultPacket VST3IPCBridge::executeCommand(const CommandPacket& command)
{
    const auto commandId = command.header.command_id;

    switch (static_cast<VST3IPCCommandType>(command.header.command_type))
    {
        case VST3IPCCommandType::SetParameter:
        {
            double before = 0.0, after = 0.0;
            std::string error;
            if (!applySetParameter(command.header.param_id,
                                   command.header.normalized_value,
                                   before, after, error))
            {
                return makeErrorResult(commandId, VST3IPCResultStatus::Failure, error);
            }
            return makeSuccessResult(commandId, before, after);
        }

        case VST3IPCCommandType::Batch:
        {
            std::vector<BatchParamDiff> diffs;
            std::string error;
            if (!applyBatch(command.payload, diffs, error))
                return makeErrorResult(commandId, VST3IPCResultStatus::Failure, error);
            auto diffPayload = serializeBatchDiffs(diffs);
            return makeSuccessResult(commandId, 0.0, 0.0, std::move(diffPayload));
        }

        case VST3IPCCommandType::GetState:
        {
            std::vector<uint8_t> payload;
            std::string error;
            if (!captureState(payload, error))
                return makeErrorResult(commandId, VST3IPCResultStatus::Failure, error);
            return makeSuccessResult(commandId, 0.0, 0.0, std::move(payload));
        }

        case VST3IPCCommandType::LoadPreset:
        {
            std::vector<double> before;
            snapshotParameters(before);

            std::string error;
            if (!loadPresetFromPayload(command.payload, error))
                return makeErrorResult(commandId, VST3IPCResultStatus::Failure, error);

            std::vector<double> after;
            snapshotParameters(after);

            std::vector<BatchParamDiff> diffs;
            const size_t n = std::min(before.size(), after.size());
            diffs.reserve(n);
            for (size_t i = 0; i < n; ++i)
                if (before[i] != after[i])
                    diffs.push_back({ static_cast<uint32_t>(i), before[i], after[i] });

            return makeSuccessResult(commandId, 0.0, 0.0, serializeBatchDiffs(diffs));
        }

        default:
            return makeErrorResult(commandId, VST3IPCResultStatus::Failure,
                                   "unknown command type");
    }
}

std::vector<uint8_t> VST3IPCBridge::serializeCommand(const CommandPacket& packet)
{
    std::vector<uint8_t> result(kCommandHeaderSize + packet.payload.size());
    std::memcpy(result.data(), &packet.header, kCommandHeaderSize);
    if (!packet.payload.empty())
        std::memcpy(result.data() + kCommandHeaderSize, packet.payload.data(), packet.payload.size());
    return result;
}

bool VST3IPCBridge::deserializeCommandHeader(const uint8_t* data,
                                             size_t size,
                                             CommandPacketHeader& out)
{
    if (data == nullptr || size < kCommandHeaderSize)
        return false;
    std::memcpy(&out, data, kCommandHeaderSize);
    return true;
}

std::vector<uint8_t> VST3IPCBridge::serializeResult(const ResultPacket& packet)
{
    std::vector<uint8_t> result(kResultHeaderSize + packet.payload.size());
    std::memcpy(result.data(), &packet.header, kResultHeaderSize);
    if (!packet.payload.empty())
        std::memcpy(result.data() + kResultHeaderSize, packet.payload.data(), packet.payload.size());
    return result;
}

bool VST3IPCBridge::deserializeResultHeader(const uint8_t* data,
                                            size_t size,
                                            ResultPacketHeader& out)
{
    if (data == nullptr || size < kResultHeaderSize)
        return false;
    std::memcpy(&out, data, kResultHeaderSize);
    return true;
}

std::vector<uint8_t> VST3IPCBridge::serializeBatchDiffs(const std::vector<BatchParamDiff>& diffs)
{
    std::vector<uint8_t> out;
    out.reserve(diffs.size() * kBatchDiffSize);
    uint8_t buf[kBatchDiffSize];
    for (const auto& d : diffs)
    {
        std::memcpy(buf, &d.paramId, sizeof(uint32_t));
        std::memcpy(buf + sizeof(uint32_t), &d.before, sizeof(double));
        std::memcpy(buf + sizeof(uint32_t) + sizeof(double), &d.after, sizeof(double));
        out.insert(out.end(), buf, buf + kBatchDiffSize);
    }
    return out;
}

std::vector<BatchParamDiff> VST3IPCBridge::deserializeBatchDiffs(const uint8_t* data, size_t size)
{
    std::vector<BatchParamDiff> out;
    if (data == nullptr || kBatchDiffSize == 0)
        return out;
    const size_t count = size / kBatchDiffSize;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        BatchParamDiff d;
        const uint8_t* p = data + i * kBatchDiffSize;
        std::memcpy(&d.paramId, p, sizeof(uint32_t));
        std::memcpy(&d.before, p + sizeof(uint32_t), sizeof(double));
        std::memcpy(&d.after, p + sizeof(uint32_t) + sizeof(double), sizeof(double));
        out.push_back(d);
    }
    return out;
}

void VST3IPCBridge::sendResult(ResultPacket result)
{
    if (impl_ == nullptr)
        return;

    const auto data = serializeResult(result);
    impl_->writeResult(data.data(), data.size());
}

ResultPacket VST3IPCBridge::makeErrorResult(uint32_t commandId,
                                            VST3IPCResultStatus status,
                                            const std::string& message) const
{
    ResultPacket result;
    result.header.command_id = commandId;
    result.header.status = static_cast<uint8_t>(status);
    result.header.value_before = 0.0;
    result.header.value_after = 0.0;
    result.header.timestamp_ns = nowNs();
    result.header.payload_length = static_cast<uint32_t>(message.size());
    result.payload.assign(message.begin(), message.end());
    return result;
}

ResultPacket VST3IPCBridge::makeSuccessResult(uint32_t commandId,
                                              double before,
                                              double after,
                                              std::vector<uint8_t> payload) const
{
    ResultPacket result;
    result.header.command_id = commandId;
    result.header.status = static_cast<uint8_t>(VST3IPCResultStatus::Success);
    result.header.value_before = before;
    result.header.value_after = after;
    result.header.timestamp_ns = nowNs();
    result.header.payload_length = static_cast<uint32_t>(payload.size());
    result.payload = std::move(payload);
    return result;
}

bool VST3IPCBridge::applySetParameter(uint32_t paramId,
                                      double normalizedValue,
                                      double& outBefore,
                                      double& outAfter,
                                      std::string& outError)
{
    auto& hostManager = processor_.getHostManager();
    const auto clamped = clampNormalized(normalizedValue);

#if JUCE_PLUGINHOST_VST3
    // VST3 direct path. Acquire exclusive plugin use so a concurrent hosted-plugin
    // load/unload (which can run on the MCP TCP connection thread) cannot
    // invalidate this raw plugin pointer mid-edit (use-after-free). The fallback
    // path below does NOT dereference the plugin pointer directly (it routes
    // through ParameterBridge's own acquire) and must NOT hold exclusive -- its
    // flush re-acquires exclusive (non-reentrant).
    if (auto* exclusivePlugin = hostManager.beginExclusivePluginUse(200))
    {
        ScopedExclusivePluginUse guard(hostManager);
        if (auto* editController = queryEditController(exclusivePlugin))
        {
            const auto count = editController->getParameterCount();
            if (paramId >= static_cast<uint32_t>(count))
            {
                outError = "param_id out of range";
                return false;
            }

            Steinberg::Vst::ParameterInfo info{};
            if (editController->getParameterInfo(static_cast<Steinberg::int32>(paramId), info) != Steinberg::kResultOk)
            {
                outError = "failed to read parameter info";
                return false;
            }

            outBefore = static_cast<double>(editController->getParamNormalized(info.id));

            // IEditController does not expose beginEdit/performEdit/endEdit; those are
            // part of the host-side IComponentHandler.  setParamNormalized is the
            // controller API for applying a normalized value.  When the optional
            // IEditControllerHostEditing extension is present we wrap the edit in the
            // host-gesture begin/end calls so the plug-in can avoid redundant automation feedback.
            Steinberg::FUnknownPtr<Steinberg::Vst::IEditControllerHostEditing> hostEditing(editController);
            if (hostEditing != nullptr)
                hostEditing->beginEditFromHost(info.id);

            editController->setParamNormalized(info.id, static_cast<Steinberg::Vst::ParamValue>(clamped));

            if (hostEditing != nullptr)
                hostEditing->endEditFromHost(info.id);

            outAfter = static_cast<double>(editController->getParamNormalized(info.id));
            return true;
        }
    }
#endif

    // Fallback: queue the edit and flush it synchronously from the message thread.
    // Only a presence null-check on the raw pointer here; all plugin access goes
    // through ParameterBridge (which acquires internally), so no exclusive lease
    // is needed (and must not be held -- flush is non-reentrant on exclusive).
    auto* plugin = hostManager.getPlugin();
    if (plugin == nullptr)
    {
        outError = "no hosted plugin loaded";
        return false;
    }

    auto& bridge = processor_.getParameterBridge();
    if (paramId >= static_cast<uint32_t>(bridge.getParameterCount()))
    {
        outError = "param_id out of range";
        return false;
    }

    outBefore = static_cast<double>(bridge.getParameterNormalized(static_cast<int>(paramId)));
    processor_.enqueueParameterSet(static_cast<int>(paramId),
                                   static_cast<float>(clamped),
                                   MorePhiProcessor::ParameterEditSource::MCP,
                                   false);
    processor_.flushPendingParameterCommandsForAssistant();
    outAfter = static_cast<double>(bridge.getParameterNormalized(static_cast<int>(paramId)));
    return true;
}

bool VST3IPCBridge::snapshotParameters(std::vector<double>& outValues) const
{
    auto* plugin = processor_.getHostManager().getPlugin();
    if (plugin == nullptr)
        return false;

    auto& bridge = processor_.getParameterBridge();
    const int count = bridge.getParameterCount();
    outValues.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        outValues[static_cast<size_t>(i)] = bridge.getParameterNormalized(i);
    return true;
}

bool VST3IPCBridge::applyBatch(const std::vector<uint8_t>& payload,
                               std::vector<BatchParamDiff>& outDiffs,
                               std::string& outError)
{
    constexpr size_t pairSize = sizeof(uint32_t) + sizeof(double);
    if (payload.size() % pairSize != 0)
    {
        outError = "batch payload size is not a multiple of 12 bytes";
        return false;
    }

    for (size_t offset = 0; offset < payload.size(); offset += pairSize)
    {
        uint32_t paramId = 0;
        double value = 0.0;
        std::memcpy(&paramId, payload.data() + offset, sizeof(paramId));
        std::memcpy(&value, payload.data() + offset + sizeof(paramId), sizeof(value));

        double before = 0.0, after = 0.0;
        if (!applySetParameter(paramId, value, before, after, outError))
            return false;

        outDiffs.push_back({ paramId, before, after });
    }

    return true;
}

bool VST3IPCBridge::loadPresetFromPayload(const std::vector<uint8_t>& payload,
                                          std::string& outError)
{
    auto& hostManager = processor_.getHostManager();
    auto* plugin = hostManager.beginExclusivePluginUse(500);
    if (plugin == nullptr)
    {
        outError = "no hosted plugin loaded";
        return false;
    }

    ScopedExclusivePluginUse guard(hostManager);
    try
    {
        plugin->setStateInformation(payload.data(), static_cast<int>(payload.size()));
        return true;
    }
    catch (const std::exception& e)
    {
        outError = std::string("setStateInformation threw: ") + e.what();
        return false;
    }
    catch (...)
    {
        outError = "setStateInformation threw an unknown exception";
        return false;
    }
}

bool VST3IPCBridge::captureState(std::vector<uint8_t>& outPayload,
                                 std::string& outError)
{
    auto& hostManager = processor_.getHostManager();
    auto* plugin = hostManager.beginExclusivePluginUse(500);
    if (plugin == nullptr)
    {
        outError = "no hosted plugin loaded";
        return false;
    }

    ScopedExclusivePluginUse guard(hostManager);
    try
    {
        juce::MemoryBlock chunk;
        plugin->getStateInformation(chunk);
        outPayload.assign(static_cast<const uint8_t*>(chunk.getData()),
                          static_cast<const uint8_t*>(chunk.getData()) + chunk.getSize());
        return true;
    }
    catch (const std::exception& e)
    {
        outError = std::string("getStateInformation threw: ") + e.what();
        return false;
    }
    catch (...)
    {
        outError = "getStateInformation threw an unknown exception";
        return false;
    }
}

} // namespace more_phi
