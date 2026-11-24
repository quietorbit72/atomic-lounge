// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin: WebView2 start-up for the editor (1.2.0).
//
// Why: WebView2 runs one browser process per user data folder. 1.1.0 used one folder for every program
// (%LOCALAPPDATA%\Vintage Chonk\WebView2): a standalone app that owned it could keep a DAW's editor from ever getting
// a browser (Ableton Live 10 showed an empty window while the plugin played). Now:
//   * the folder is per host program: %LOCALAPPDATA%\Atomic Lounge\WebView2\<host exe name without extension>
//     (several editors inside one host share it, as WebView2 allows within one process);
//   * before the panel is made, a probe creates a WebView2 environment and a controller (on a hidden window) and
//     reports the step and HRESULT that failed, so the editor can show the reason instead of a blank window.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace mfm1 {

struct WebView2Probe
{
    static juce::String hostName();                 // this process's exe name without extension, legal as a folder
    static juce::File dataFolder();                 // %LOCALAPPDATA%\Atomic Lounge\WebView2\<hostName()>
    static juce::String runtimeVersion();           // installed WebView2 runtime, empty if none
    // asynchronous (message thread); done (ok, failed step, HRESULT) is called once on the message thread,
    // at the latest after timeoutMs
    using Done = std::function<void (bool ok, const juce::String& step, long hr)>;
    static void run (const juce::File& folder, Done done, int timeoutMs = 20000);
    static juce::String describe (long hr);          // "0x80070002 (The system cannot find the file specified.)"
};

}
