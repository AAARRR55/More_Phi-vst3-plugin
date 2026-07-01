import os


def read_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_file(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


# Fix C-2: Pre-cache state for audio-thread callers
pp_h = read_file(r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.h")

old_decl = """    // C-5 FIX: Track currently selected program for DAW preset browser
    std::atomic<int> currentProgram_{0};

    JUCE_DECLARE_WEAK_REFERENCEABLE(MorePhiProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorePhiProcessor)"""

new_decl = """    // C-5 FIX: Track currently selected program for DAW preset browser
    std::atomic<int> currentProgram_{0};

    // C-2 FIX: Cached state for getStateInformation when called from the
    // audio thread. The cache is updated by the message thread and is
    // returned directly to audio-thread callers to avoid heap allocation
    // on the real-time path.
    mutable juce::MemoryBlock cachedSavedState_;
    mutable juce::SpinLock cachedSavedStateMutex_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(MorePhiProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MorePhiProcessor)"""

pp_h = pp_h.replace(old_decl, new_decl)
write_file(r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.h", pp_h)
print("Fixed PluginProcessor.h for C-2 cache")

pp_cpp = read_file(r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.cpp")

old_top = """void MorePhiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // P2 FIX: Detect calling thread so DBG() (which may allocate) is skipped
    // on the audio thread during offline render/export.
    bool isAudioThread = false;
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        isAudioThread = !mm->isThisTheMessageThread();

    auto state = apvts.copyState();
    auto xml = state.createXml();"""

new_top = """void MorePhiProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // P2 FIX: Detect calling thread so DBG() (which may allocate) is skipped
    // on the audio thread during offline render/export.
    bool isAudioThread = false;
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        isAudioThread = !mm->isThisTheMessageThread();

    // C-2 FIX: If called from the audio thread (e.g. Pro Tools offline
    // render or FL Studio export), return the last cached state snapshot
    // that was saved on the message thread. This avoids heavy heap
    // allocations (XML + Base64 encoding) on the real-time path.
    if (isAudioThread)
    {
        juce::MemoryBlock cached;
        {
            const juce::SpinLock::ScopedLockType lock(cachedSavedStateMutex_);
            cached = cachedSavedState_;
        }
        if (cached.getSize() > 0)
        {
            destData = std::move(cached);
            return;
        }
    }

    auto state = apvts.copyState();
    auto xml = state.createXml();"""

pp_cpp = pp_cpp.replace(old_top, new_top)

old_bottom = """    copyXmlToBinary(*xml, destData);
}"""

new_bottom = """    copyXmlToBinary(*xml, destData);

    // C-2 FIX: Cache the built state on the message thread so that
    // future audio-thread callers (e.g. offline render) can
    // return it immediately without building XML or allocating memory.
    if (!isAudioThread)
    {
        const juce::SpinLock::ScopedLockType lock(cachedSavedStateMutex_);
        cachedSavedState_ = destData;
    }
}"""

pp_cpp = pp_cpp.replace(old_bottom, new_bottom)
write_file(r"G:\More_Phi-vst3-plugin\src\Plugin\PluginProcessor.cpp", pp_cpp)
print("Fixed PluginProcessor.cpp for C-2 cache")
print("All C-2 edits applied successfully.")
