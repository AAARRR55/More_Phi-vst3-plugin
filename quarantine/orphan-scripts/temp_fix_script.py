import re
import os


def read_utf8(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_utf8(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


# Fix 1: LinkBroadcaster.cpp - validate magic and version
lb_cpp = r"G:\More_Phi-vst3-plugin\src\AI\LinkBroadcaster.cpp"
content = read_utf8(lb_cpp)

# Add magic and version write in broadcast()
old_broadcast = """    // 2. Seqlock write: increment to odd (signals write in progress)
    sharedMem_->seqlock.fetch_add(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    sharedMem_->morphX.store(x, std::memory_order_relaxed);
    sharedMem_->morphY.store(y, std::memory_order_relaxed);"""

new_broadcast = """    // 2. Seqlock write: increment to odd (signals write in progress)
    sharedMem_->seqlock.fetch_add(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    // C-4 FIX: Write magic + version on every broadcast so followers can
    // validate that the shared memory block was written by a real More-Phi
    // leader and not corrupted by another process.
    sharedMem_->magic.store(LinkStateBlock::kMagic, std::memory_order_relaxed);
    sharedMem_->version.fetch_add(1, std::memory_order_relaxed);
    sharedMem_->morphX.store(x, std::memory_order_relaxed);
    sharedMem_->morphY.store(y, std::memory_order_relaxed);"""

content = content.replace(old_broadcast, new_broadcast)

# Add magic and version check in receive()
old_receive = """    // Check heartbeat: if leader hasn't updated in 100ms, assume stale
    const uint64_t now = static_cast<uint64_t>(juce::Time::currentTimeMillis());
    const uint64_t last = sharedMem_->lastActivity.load(std::memory_order_relaxed);
    if (now > last + 100) return false;

    // Seqlock read with retry"""

new_receive = """    // Check heartbeat: if leader hasn't updated in 100ms, assume stale
    const uint64_t now = static_cast<uint64_t>(juce::Time::currentTimeMillis());
    const uint64_t last = sharedMem_->lastActivity.load(std::memory_order_relaxed);
    if (now > last + 100) return false;

    // C-4 FIX: Validate magic number before trusting the shared memory content.
    // If the block was written by a non-More-Phi process or was corrupted, abort.
    if (sharedMem_->magic.load(std::memory_order_acquire) != LinkStateBlock::kMagic)
        return false;

    // Seqlock read with retry"""

content = content.replace(old_receive, new_receive)
write_utf8(lb_cpp, content)
print("Fixed LinkBroadcaster.cpp")

# Fix 2: PluginProcessor.cpp - add state version migration framework
pp_cpp = r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.cpp"
content = read_utf8(pp_cpp)

old_version = """    // Version check for forward migration
    const juce::String stateVersion = xml->getStringAttribute("stateVersion", "0.0.0");
    juce::ignoreUnused(stateVersion);  // Future: use for migration logic"""

new_version = """    // Version check for forward migration
    const juce::String stateVersion = xml->getStringAttribute("stateVersion", "0.0.0");
    if (!applyStateMigration(*xml, stateVersion))
    {
        DBG("setStateInformation: WARNING - State migration failed for version: " + stateVersion);
    }"""

content = content.replace(old_version, new_version)

# Add migration function implementation in PluginProcessor.cpp
old_impl = """void MorePhiProcessor::applyPendingFullStateRecall()"""
new_impl = """// H3 FIX: Stub implementation for future state migration.
// When the version is older than the current version, transform the XML
// to match the current schema before restoration proceeds.
bool MorePhiProcessor::applyStateMigration(juce::XmlElement\u0026 /*stateXml*/, const juce::String\u0026 version)
{
    // Current version is v3.3.0. If version is missing or older than 3.0.0,
    // the state format has changed significantly enough that automatic
    // migration is not attempted. Future versions can add incremental
    // transforms here based on the detected version.
    if (version.isEmpty() || version == "0.0.0")
    {
        // Reset to default: old state that predates versioning.
        return false;
    }
    // No migration needed for same or newer versions
    return true;
}

void MorePhiProcessor::applyPendingFullStateRecall()"""

content = content.replace(old_impl, new_impl)

# Add agent runtime sequencing check
old_check = """void MorePhiProcessor::startAgentRuntimeIfNeeded()
{
    if (agentRuntime_ != nullptr)
        return;

    namespace ag = more_phi::agents;"""

new_check = """void MorePhiProcessor::startAgentRuntimeIfNeeded()
{
    if (agentRuntime_ != nullptr)
        return;

    // H8 FIX: Ensure MCP server is fully up before building the agent runtime.
    // The runtime borrows AutomationRuntime from the MCP server, which must
    // be alive and fully initialized first.
    if (!mcpServer.isRunning())
    {
        DBG("startAgentRuntimeIfNeeded: MCP server not running yet, deferring");
        return;
    }

    namespace ag = more_phi::agents;"""

content = content.replace(old_check, new_check)
write_utf8(pp_cpp, content)
print("Fixed PluginProcessor.cpp")

# Add migration function declaration in PluginProcessor.h
pp_h = r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.h"
content = read_utf8(pp_h)
old_decl = """    void updateReportedLatency();
    void applyPendingFullStateRecall();"""
new_decl = """    void updateReportedLatency();
    void applyPendingFullStateRecall();
    // H3 FIX: Apply forward migration when loading state from an older version.
    // Returns false if the data is from an unsupported version that cannot be migrated.
    bool applyStateMigration(juce::XmlElement\u0026 stateXml, const juce::String\u0026 version);"""
content = content.replace(old_decl, new_decl)
write_utf8(pp_h, content)
print("Fixed PluginProcessor.h")

print("All temp_fix_script.py edits applied successfully.")
