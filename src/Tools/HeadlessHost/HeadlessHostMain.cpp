// HeadlessHost — a GUI-initialized JUCE host that loads a VST3 plugin, constructs
// its AudioProcessorEditor, streams audio on a REAL high-priority audio thread,
// and runs a message loop so the plugin controller is ALIVE and sees genuine
// real-time streaming + isPlaying.
//
// Why this exists: the stdio MorePhiMcpServer cannot bootstrap a JUCE
// MessageManager (it deadlocks), so plugin controllers are never constructed
// there. A GUI-initialized process can. Streaming on a real audio thread (not a
// message-thread Timer) is the route to make a plugin's assistant/modal
// controller consider the signal valid and open its panel.
//
// Env:
//   MOREPHI_HOST_SECONDS  (default 45) — message-loop duration
//   MOREPHI_HOST_AUDIO_FILE (optional) — WAV/AIFF looped as the stream (else synth)
//   MOREPHI_HOST_BLOCK     (optional) — block size (default 512)
//   MOREPHI_HOST_PLUGIN    (optional) — path to VST3 to load (default iZotope Ozone Pro)
//
// Logs to stderr; prints EDITOR_HWND=0x... for UI-automation drivers.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <typeinfo>

namespace {
void log(const juce::String& s)
{
    std::fprintf(stderr, "[HeadlessHost] %s\n", s.toRawUTF8());
    std::fflush(stderr);
}

void dumpTree(juce::Component* c, int depth)
{
    juce::String pad;
    for (int i = 0; i < depth; ++i)
        pad += "  ";
    log(pad + juce::String(typeid(*c).name()) + " | id=" + c->getComponentID()
        + " | name=\"" + c->getName() + "\" | " + juce::String(c->getWidth()) + "x"
        + juce::String(c->getHeight()) + " @(" + juce::String(c->getX()) + ","
        + juce::String(c->getY()) + ")");
    for (auto* ch : c->getChildren())
        dumpTree(ch, depth + 1);
}

int countTree(juce::Component* c)
{
    int n = 1;
    for (auto* ch : c->getChildren())
        n += countTree(ch);
    return n;
}
} // namespace

// AudioPlayHead reporting a continuously-advancing, playing transport on the
// audio thread. Rich PositionInfo so the target plugin sees a well-formed streaming host.
struct HeadlessPlayHead : juce::AudioPlayHead
{
    juce::AudioPlayHead::PositionInfo info;
    juce::int64 samples = 0;
    double rate = 44100.0;
    double bpm  = 120.0;

    void advance(int blockSize)
    {
        samples += blockSize;
        const double secs = (double) samples / rate;
        info.setTimeInSamples(samples);
        info.setTimeInSeconds(secs);
        info.setBpm(bpm);
        info.setTimeSignature(juce::AudioPlayHead::TimeSignature { 4, 4 });
        info.setIsPlaying(true);
        info.setIsRecording(false);
        info.setEditOriginTime(0.0);
        info.setPpqPosition(secs * bpm / 60.0);
        info.setPpqPositionOfLastBarStart(0.0);
        info.setHostTimeNs(juce::Time::getHighResolutionTicks() * 100);
    }

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return info; }
};

// Real-time audio thread: fills a block, advances the playhead, calls
// processBlock, then paces to the block's real-time duration. This is the
// crucial change vs. the old message-thread Timer — the plugin now sees genuine
// streaming audio delivered on a dedicated high-priority thread.
struct RealtimeAudioThread : juce::Thread
{
    juce::AudioPluginInstance* plugin = nullptr;
    HeadlessPlayHead* playHead = nullptr;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    double sr = 44100.0;
    int bs = 512;
    double phase = 0.0;

    juce::AudioBuffer<float> fileBuf;
    juce::int64 filePos = 0;
    bool useFile = false;
    std::atomic<long long> blocksProcessed { 0 };

    RealtimeAudioThread(double sr_, int bs_, int chans_)
        : juce::Thread("HeadlessHostAudio"), buf(chans_, bs_), sr(sr_), bs(bs_) {}

    void fillBlock()
    {
        buf.clear();
        if (useFile && fileBuf.getNumSamples() > 0)
        {
            const int avail = fileBuf.getNumSamples();
            for (int c = 0; c < buf.getNumChannels(); ++c)
            {
                const int srcC = juce::jmin(c, fileBuf.getNumChannels() - 1);
                auto* w = buf.getWritePointer(c);
                const auto* r = fileBuf.getReadPointer(srcC);
                for (int i = 0; i < bs; ++i)
                    w[i] = r[(int) (((filePos + i) % avail + avail) % avail)];
            }
            filePos = (filePos + bs) % avail;
        }
        else
        {
            for (int c = 0; c < buf.getNumChannels(); ++c)
            {
                auto* w = buf.getWritePointer(c);
                for (int i = 0; i < bs; ++i)
                {
                    phase += 0.06;
                    const float s = 0.28f * (float) std::sin(phase + 0.3 * c)
                                    + 0.18f * (float) std::sin(phase * 1.5 + c)
                                    + 0.05f * ((std::rand() % 1000) / 1000.0f - 0.5f);
                    w[i] = s;
                }
            }
        }
    }

    void run() override
    {
        const double periodMs = (double) bs / sr * 1000.0;
        double nextMs = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            fillBlock();
            if (playHead != nullptr)
                playHead->advance(bs);
            try
            {
                if (plugin != nullptr)
                    plugin->processBlock(buf, midi);
            }
            catch (...)
            {
            }
            ++blocksProcessed;

            // Pace to real time so block cadence matches wall clock (genuine streaming).
            nextMs += periodMs;
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const int waitMs = (int) (nextMs - nowMs);
            if (waitMs > 0)
                wait(waitMs);
            else
                nextMs = nowMs; // fell behind; resync to avoid spiral
        }
    }
};

// ponytail: real-audio-file streaming (juce_audio_formats module) deferred — the
// real-time thread + synth signal is the initial approach; add file loading if
// the target plugin still rejects the synthetic stream.

int main()
{
    std::srand((unsigned) std::time(nullptr));
    juce::ScopedJuceInitialiser_GUI guiInit; // creates a MessageManager on this thread
    auto* mm = juce::MessageManager::getInstance();
    log("JUCE GUI initialized.");

    const double sr = 44100.0;
    const int bs = juce::SystemStats::getEnvironmentVariable("MOREPHI_HOST_BLOCK", "512").getIntValue();
    const int chans = 2;

    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    auto pluginPath = juce::SystemStats::getEnvironmentVariable("MOREPHI_HOST_PLUGIN", "");
    juce::File pluginFile;
    if (pluginPath.isNotEmpty())
        pluginFile = juce::File(pluginPath);
    else
        pluginFile = juce::File(R"(C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3)");

    if (! pluginFile.existsAsFile())
    {
        log("Plugin not found at: " + pluginFile.getFullPathName());
        return 1;
    }

    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* fmt : fm.getFormats())
        fmt->findAllTypesForFile(found, pluginFile.getFullPathName());
    if (found.isEmpty())
    {
        log("No plugins discovered in " + pluginFile.getFullPathName());
        return 1;
    }
    log("discovered " + juce::String(found.size()) + " plugin(s); using \"" + found.getFirst()->name
        + "\" (" + found.getFirst()->pluginFormatName + ")");

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> plugin(
        fm.createPluginInstance(*found.getFirst(), sr, bs, err));
    if (plugin == nullptr)
    {
        log("Failed to load plugin: " + err);
        return 1;
    }
    log("Plugin loaded: " + plugin->getName());

    plugin->enableAllBuses();
    plugin->prepareToPlay(sr, bs);

    std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditor());
    if (editor == nullptr)
    {
        log("createEditor() returned null (editor not constructible even GUI-initialized).");
    }
    else
    {
        log("editor constructed: " + juce::String(editor->getWidth()) + "x"
            + juce::String(editor->getHeight()) + " components=" + juce::String(countTree(editor.get())));
        // Give the editor a real native HWND (offscreen) so Windows UI Automation
        // can drive the plugin's Assistant controls with no human interaction.
        editor->setTopLeftPosition(-32000, -32000);
        editor->addToDesktop(0);
        if (auto* peer = editor->getPeer())
        {
            auto hwnd = reinterpret_cast<intptr_t>(peer->getNativeHandle());
            log("EDITOR_HWND=0x" + juce::String::toHexString(hwnd));
        }
    }

    // Stream audio on a real high-priority thread.
    HeadlessPlayHead playHead;
    playHead.rate = sr;
    plugin->setPlayHead(&playHead);

    RealtimeAudioThread audio(sr, bs, chans);
    audio.plugin = plugin.get();
    audio.playHead = &playHead;

    audio.startThread(juce::Thread::Priority::highest);
    log("real-time audio thread started (priority=highest, bs=" + juce::String(bs) + ").");

    // Run the message loop (keeps editor timers + the controller alive) for the
    // configured duration; the audio thread streams concurrently.
    int seconds = juce::SystemStats::getEnvironmentVariable("MOREPHI_HOST_SECONDS", "45").getIntValue();
    if (seconds <= 0)
        seconds = 45;
    log("running message loop for " + juce::String(seconds) + "s. Attach Frida / UI-automate now.");
    const juce::int64 endMs = juce::Time::currentTimeMillis() + (juce::int64) seconds * 1000;
    while (juce::Time::currentTimeMillis() < endMs)
        mm->runDispatchLoopUntil(50);

    log("audio blocks streamed: " + juce::String(audio.blocksProcessed.load()));
    audio.signalThreadShouldExit();
    audio.waitForThreadToExit(2000);

    if (editor != nullptr)
    {
        plugin->editorBeingDeleted(editor.get());
        editor.reset();
    }
    plugin->releaseResources();
    log("done.");
    return 0;
}
