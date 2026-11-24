// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk): the editor = a web panel in JUCE's WebBrowserComponent on WebView2.
//   REGOLITH (internally 'chonk'): the tablet's panel (panel, unchanged files served from binary data)
//          with the desktop bridge (web/desktop_shim.js, which also adds the REGOLITH | ORBIT switch).
//   ORBIT: web/orbit/orbit.html (the "Streamline Lounge" panel), bridged through the same 'mfm1' events.
// Switching the mode navigates between the two pages.
// 1.2.0: WebView2 data folder per host program (WebView2Probe); the browser is made only after a probe has started
// WebView2; if that fails, or the page does not answer, the editor shows the reason and the HRESULT with a Retry
// button instead of a blank window. Tests: ATOMIC_LOUNGE_TEST_STATUS=<file> makes the editor append what happened.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <map>
#include "PluginProcessor.h"

class VintageChonkEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VintageChonkEditor (VintageChonkProcessor&);
    ~VintageChonkEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void onPanelEvent (const juce::var& v);
    void onOrbitEvent (const juce::String& op, const juce::Array<juce::var>& a);
    std::optional<juce::WebBrowserComponent::Resource> resource (const juce::String& url);
    juce::WebBrowserComponent::Options options();
    void js (const juce::String& code) { if (browser != nullptr) browser->evaluateJavascript (code); }
    void pushRestore();
    void pushPresets();
    void pushOrbitState();
    void pickNmsv();
    void pickSample (int layer, bool impulse);
    void showMode();

    // WebView2 start-up
    void startPanel();                       // probe, then the browser
    void probeDone (bool ok, const juce::String& step, long hr);
    void showFallback (const juce::String& why, long hr);
    void testStatus (const juce::String& line) const;
    enum class Ui { probing, loading, ready, failed };
    Ui ui = Ui::probing;
    juce::uint32 loadStart = 0;
    int attempt = 0;
    juce::File dataFolder;
    juce::Label fallbackText;
    juce::TextButton retry { "Retry" };

    VintageChonkProcessor& proc;
    std::map<juce::String, std::pair<const char*, int>> files;   // file name -> binary data
    std::unique_ptr<juce::WebBrowserComponent> browser;
    int seenRestore = 0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    // ORBIT page
    int seenMode = -1;
    bool orbitPage = false;          // the ORBIT page said hello (and is the one loaded)
    int seenOrbitRestore = 0, seenViews = 0;
    std::vector<float> sentHost;
    juce::MemoryBlock drop;          // a file dropped on the panel, arriving in chunks
    juce::String dropName;
    int dropLayer = 0;
    bool dropImpulse = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (VintageChonkEditor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageChonkEditor)
};
