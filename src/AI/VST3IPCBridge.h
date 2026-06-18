/*
 * More-Phi — AI/VST3IPCBridge.h
 * Cross-platform IPC bridge between the Python stdio MCP server and More-Phi.
 *
 * Transport: Windows named pipe or POSIX Unix domain socket.
 * Protocol: fixed-size little-endian headers + payload.
 */
#pragma once

#include "InstanceIdentity.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace more_phi {

class MorePhiProcessor;

/** Binary command types shared with the Python server. */
enum class VST3IPCCommandType : uint8_t
{
    SetParameter = 1,
    LoadPreset   = 2,
    GetState     = 3,
    Batch        = 4
};

/** Result status codes shared with the Python server. */
enum class VST3IPCResultStatus : uint8_t
{
    Success = 0,
    Failure = 1,
    Timeout = 2
};

#pragma pack(push, 1)

/** 21-byte little-endian command header. */
struct CommandPacketHeader
{
    uint32_t command_id = 0;
    uint8_t  command_type = 0;
    uint32_t param_id = 0;
    double   normalized_value = 0.0;
    uint32_t payload_length = 0;
};

/** 33-byte little-endian result header.
 *
 *  NOTE: The integration spec lists the ResultPacketHeader as 29 bytes, which
 *  omits the 4-byte payload_length field. The field is included here so that
 *  both sides can read the payload size from the header, making the actual
 *  packed size 33 bytes. The C++ and Python serializers use identical layouts.
 */
struct ResultPacketHeader
{
    uint32_t command_id = 0;
    uint8_t  status = 0;
    double   value_before = 0.0;
    double   value_after = 0.0;
    uint64_t timestamp_ns = 0;
    uint32_t payload_length = 0;
};

#pragma pack(pop)

struct CommandPacket
{
    CommandPacketHeader header;
    std::vector<uint8_t> payload;
};

struct ResultPacket
{
    ResultPacketHeader header;
    std::vector<uint8_t> payload;
};

/** One parameter's verified before/after, returned in a BATCH result payload.
 *  Serialized little-endian as (uint32 paramId, double before, double after) = 20 bytes.
 */
struct BatchParamDiff
{
    uint32_t paramId = 0;
    double before = 0.0;
    double after = 0.0;
};

/**
 * IPC bridge that exposes the hosted VST3/AU plugin to the Python MCP server.
 *
 * The bridge runs a reader thread that blocks on the transport. Each incoming
 * command is forwarded to the JUCE message thread via
 * juce::MessageManager::callAsync, where it is safe to call hosted plugin
 * parameter gesture methods and state capture/restore.
 */
class VST3IPCBridge
{
public:
    VST3IPCBridge(MorePhiProcessor& processor, const InstanceIdentity& identity);
    virtual ~VST3IPCBridge();

    /** Start the listener thread. Safe to call multiple times (idempotent). */
    void start();

    /** Request a bounded shutdown (up to ~500 ms). */
    void stop();

    /** True between a successful start() and stop(). */
    bool isRunning() const noexcept { return running_.load(); }

    /** Full transport endpoint path (pipe name on Windows, socket path otherwise). */
    juce::String getEndpointPath() const;

    /** Directory/file where the parameter registry JSON is exported. */
    juce::File getParameterRegistryPath() const;

    /** Export the current hosted plugin parameter descriptors to JSON.
     *  Returns true if the file was written (including an empty registry).
     */
    bool exportParameterRegistry();

    /** Synchronously execute one command. Intended to run on the message thread.
     *  Tests may call this directly; the transport calls it via callAsync.
     */
    ResultPacket executeCommand(const CommandPacket& command);

    // ------------------------------------------------------------------
    // Serialization helpers (also exercised from unit tests).
    // ------------------------------------------------------------------
    static std::vector<uint8_t> serializeCommand(const CommandPacket& packet);
    static bool deserializeCommandHeader(const uint8_t* data,
                                         size_t size,
                                         CommandPacketHeader& out);
    static std::vector<uint8_t> serializeResult(const ResultPacket& packet);
    static bool deserializeResultHeader(const uint8_t* data,
                                        size_t size,
                                        ResultPacketHeader& out);

    static std::vector<uint8_t> serializeBatchDiffs(const std::vector<BatchParamDiff>& diffs);
    static std::vector<BatchParamDiff> deserializeBatchDiffs(const uint8_t* data, size_t size);

    static constexpr size_t kCommandHeaderSize = sizeof(CommandPacketHeader);
    static constexpr size_t kResultHeaderSize  = sizeof(ResultPacketHeader);
    static constexpr size_t kBatchDiffSize = sizeof(uint32_t) + sizeof(double) + sizeof(double);

protected:
    /** Seams for the hosted-plugin command handlers. Default implementations wrap
     *  MorePhiProcessor (production); tests override these to inject an in-memory
     *  fake plugin so executeCommand success paths can be exercised without a real
     *  hosted plugin loaded. applyBatch() calls applySetParameter(), so overriding
     *  applySetParameter alone redirects batch application. */
    virtual bool applySetParameter(uint32_t paramId, double normalizedValue,
                                   double& outBefore, double& outAfter,
                                   std::string& outError);
    virtual bool captureState(std::vector<uint8_t>& outPayload,
                              std::string& outError);
    virtual bool loadPresetFromPayload(const std::vector<uint8_t>& payload,
                                       std::string& outError);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    MorePhiProcessor& processor_;
    InstanceIdentity identity_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    ResultPacket makeErrorResult(uint32_t commandId,
                                 VST3IPCResultStatus status,
                                 const std::string& message) const;
    ResultPacket makeSuccessResult(uint32_t commandId,
                                   double before,
                                   double after,
                                   std::vector<uint8_t> payload = {}) const;

    bool applyBatch(const std::vector<uint8_t>& payload,
                    std::vector<BatchParamDiff>& outDiffs,
                    std::string& outError);

    /** Writes a result packet back to the current IPC client.
     *  Called on the message thread by the async command dispatcher.
     */
    void sendResult(ResultPacket result);

    JUCE_DECLARE_WEAK_REFERENCEABLE(VST3IPCBridge)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VST3IPCBridge)
};

} // namespace more_phi
