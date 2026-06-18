/*
 * More-Phi — Python stdio MCP server integration smoke test (Catch2)
 *
 * JUCE's ChildProcess does not expose stdin writing on Windows, so this test
 * performs a compile/import smoke check by running the Python interpreter on
 * the server script. Full stdio round-trip tests live in
 * scripts/vst3-mcp-server/tests/.
 */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include "AI/VST3IPCBridge.h"

using namespace more_phi;

namespace {

juce::File findServerScript()
{
    juce::File cwd = juce::File::getCurrentWorkingDirectory();
    juce::File candidate = cwd.getChildFile("scripts/vst3-mcp-server/server.py");

    for (int i = 0; i < 5 && !candidate.existsAsFile(); ++i)
    {
        cwd = cwd.getParentDirectory();
        candidate = cwd.getChildFile("scripts/vst3-mcp-server/server.py");
    }

    return candidate;
}

juce::String findPythonExecutable()
{
    juce::String python = juce::SystemStats::getEnvironmentVariable("PYTHON", {});
    if (python.isEmpty())
        python = juce::SystemStats::getEnvironmentVariable("PYTHON3", {});
    if (python.isEmpty())
        python = "python";
    return python;
}

} // namespace

TEST_CASE("Python MCP server script exists", "[integration][python-mcp]")
{
    const juce::File serverScript = findServerScript();
    REQUIRE(serverScript.existsAsFile());
}

TEST_CASE("Python MCP server script compiles", "[integration][python-mcp]")
{
    const juce::File serverScript = findServerScript();
    if (!serverScript.existsAsFile())
        SKIP("Python MCP server script not found at " << serverScript.getFullPathName());

    juce::StringArray args;
    args.add(findPythonExecutable());
    args.add("-m");
    args.add("py_compile");
    args.add(serverScript.getFullPathName());

    juce::ChildProcess proc;
    REQUIRE(proc.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr));

    const juce::String output = proc.readAllProcessOutput();
    const bool finished = proc.waitForProcessToFinish(30000);
    const juce::uint32 exitCode = proc.getExitCode();

    INFO("py_compile output: " << output);
    REQUIRE(finished);
    REQUIRE(exitCode == 0);
}
