// SPDX-License-Identifier: AGPL-3.0-only
#include "PluginEditor.h"
#include "PresetStore.h"
#include "WebView2Probe.h"
#include "MFM1Www.h"

static juce::String quoted (const juce::String& s) { return juce::JSON::toString (juce::var (s)); }
static juce::String json (const juce::var& v) { return juce::JSON::toString (v, true); }

static std::map<juce::String, std::pair<const char*, int>> wwwFiles()
{
    std::map<juce::String, std::pair<const char*, int>> m;
    for (int i = 0; i < MFM1Www::namedResourceListSize; i++)
    {
        int size = 0;
        const char* data = MFM1Www::getNamedResource (MFM1Www::namedResourceList[i], size);
        m[juce::String (MFM1Www::getNamedResourceOriginalFilename (MFM1Www::namedResourceList[i]))] = { data, size };
    }
    return m;
}

VintageChonkEditor::VintageChonkEditor (VintageChonkProcessor& p)
    : AudioProcessorEditor (p), proc (p), files (wwwFiles())
{
    seenRestore = proc.restoreSerial();
    fallbackText.setJustificationType (juce::Justification::centred);
    fallbackText.setColour (juce::Label::textColourId, juce::Colour (0xfff3e9d2));
    fallbackText.setFont (juce::FontOptions (17.0f));
    addChildComponent (fallbackText);
    retry.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc8642a));
    retry.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff3e9d2));
    retry.onClick = [this] { startPanel(); };
    addChildComponent (retry);
    // both panels are drawn for 16:10 (1920 x 1200 and 1600 x 1000) and scale to fit: keep that shape
    setResizable (true, true);
    setResizeLimits (720, 450, 3840, 2400);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio (1920.0 / 1200.0);
    setSize (1440, 900);
    dataFolder = mfm1::WebView2Probe::dataFolder();
    testStatus ("folder " + dataFolder.getFullPathName());
    startPanel();
    startTimerHz (30);
}

VintageChonkEditor::~VintageChonkEditor() { stopTimer(); }

void VintageChonkEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff141312));
    if (ui == Ui::probing || (ui == Ui::loading && browser == nullptr))
    {
        g.setColour (juce::Colour (0xff8a8175));
        g.setFont (juce::FontOptions (16.0f));
        g.drawText ("ATOMIC LOUNGE  -  opening the panel ...", getLocalBounds(), juce::Justification::centred);
    }
}

void VintageChonkEditor::resized()
{
    if (browser != nullptr) browser->setBounds (getLocalBounds());
    auto b = getLocalBounds().reduced (getWidth() / 8, getHeight() / 5);
    retry.setBounds (b.removeFromBottom (44).withSizeKeepingCentre (160, 40));
    fallbackText.setBounds (b);
}

void VintageChonkEditor::testStatus (const juce::String& line) const
{
    const auto f = juce::SystemStats::getEnvironmentVariable ("ATOMIC_LOUNGE_TEST_STATUS", {});
    if (f.isNotEmpty()) juce::File (f).appendText (line + "\n");
}

void VintageChonkEditor::startPanel()
{
    // (re)start: drop a browser that never answered, probe WebView2 in this host's folder, then make the panel
    browser.reset();
    fallbackText.setVisible (false);
    retry.setVisible (false);
    ui = Ui::probing;
    attempt++;
    repaint();
    juce::WeakReference<VintageChonkEditor> weak (this);
    const int a = attempt;
    mfm1::WebView2Probe::run (dataFolder,
                              [weak, a] (bool ok, const juce::String& step, long hr)
                              {
                                  if (weak != nullptr && weak->attempt == a) weak->probeDone (ok, step, hr);
                              });
}

void VintageChonkEditor::probeDone (bool ok, const juce::String& step, long hr)
{
    if (! ok)
    {
        showFallback ("WebView2 could not start (" + step + ").", hr);
        return;
    }
    testStatus ("probe ok");
    browser = std::make_unique<juce::WebBrowserComponent> (options());
    addAndMakeVisible (*browser);
    browser->toBack();
    ui = Ui::loading;
    loadStart = juce::Time::getMillisecondCounter();
    resized();
    showMode();
}

void VintageChonkEditor::showFallback (const juce::String& why, long hr)
{
    browser.reset();
    ui = Ui::failed;
    const auto runtime = mfm1::WebView2Probe::runtimeVersion();
    fallbackText.setText ("The Atomic Lounge panel could not open.\n\n" + why + "\n"
                              + (hr != 0 ? "HRESULT " + mfm1::WebView2Probe::describe (hr) + "\n" : juce::String())
                              + "\nWebView2 runtime: " + (runtime.isNotEmpty() ? runtime : juce::String ("not installed"))
                              + "\nData folder: " + dataFolder.getFullPathName()
                              + "\n\nThe sound keeps playing; parameters can still be automated from the host."
                              + "\nClose other programs that show this panel, or press Retry.",
                          juce::dontSendNotification);
    fallbackText.setVisible (true);
    retry.setVisible (true);
    testStatus ("fallback " + why + " hr=0x" + juce::String::toHexString ((juce::uint32) hr).paddedLeft ('0', 8).toUpperCase());
    resized();
    repaint();
}

void VintageChonkEditor::showMode()
{
    seenMode = proc.modeSerial();
    orbitPage = false;
    if (browser == nullptr) return;
    const auto root = juce::WebBrowserComponent::getResourceProviderRoot();
    browser->goToURL (proc.mode() == VintageChonkProcessor::orbitMode ? root + "orbit.html" : root);
}

juce::WebBrowserComponent::Options VintageChonkEditor::options()
{
    auto* init = new juce::DynamicObject();
    init->setProperty ("state", proc.panelState());
    init->setProperty ("presets", mfm1::PresetStore::list());
    init->setProperty ("status", proc.status());
    int n = 0;
    const char* shim = MFM1Www::getNamedResource ("desktop_shim_js", n);
    const juce::String script = "window.MFHostInit = " + json (juce::var (init)) + ";\n" + juce::String::fromUTF8 (shim, n);

    dataFolder.createDirectory();
    using O = juce::WebBrowserComponent::Options;
    return O {}
        .withBackend (O::Backend::webview2)
        .withWinWebView2Options (O::WinWebView2 {}
                                     .withUserDataFolder (dataFolder)
                                     .withBackgroundColour (juce::Colour (0xff141312))
                                     .withStatusBarDisabled()
                                     .withBuiltInErrorPageDisabled())
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withUserScript (script)
        .withEventListener ("mfm1", [this] (const juce::var& v) { onPanelEvent (v); })
        .withResourceProvider ([this] (const juce::String& url) { return resource (url); });
}

std::optional<juce::WebBrowserComponent::Resource> VintageChonkEditor::resource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("#", false, false);
    path = path.trimCharactersAtStart ("/");
    if (path.isEmpty()) path = "index.html";
    if (path == "desktop_shim.js") return std::nullopt;     // injected, not served
    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    const auto it = files.find (name);
    if (it == files.end()) return std::nullopt;
    juce::String mime = "application/octet-stream";
    if (name.endsWith (".html")) mime = "text/html";
    else if (name.endsWith (".css")) mime = "text/css";
    else if (name.endsWith (".js")) mime = "text/javascript";
    const auto* b = reinterpret_cast<const std::byte*> (it->second.first);
    return juce::WebBrowserComponent::Resource { std::vector<std::byte> (b, b + it->second.second), mime };
}

void VintageChonkEditor::pushRestore()
{
    js ("window.MFHost && MFHost.restore(" + json (proc.panelState()) + ")");
}

void VintageChonkEditor::pushPresets()
{
    js ("window.MFHost && MFHost.presets(" + json (mfm1::PresetStore::list()) + ")");
}

void VintageChonkEditor::pushOrbitState()
{
    seenOrbitRestore = proc.orbitRestoreSerial();
    seenViews = proc.orbitViewSerial();
    sentHost.clear();
    proc.orbitHostChanges (sentHost);      // what the state carries is what the panel has
    js ("window.ORB && ORB.state(" + json (proc.orbitState()) + ")");
}

void VintageChonkEditor::timerCallback()
{
    if (browser == nullptr) return;
    if (ui == Ui::loading && juce::Time::getMillisecondCounter() - loadStart > 20000)
    {
        showFallback ("The panel page did not answer within 20 s (WebView2 started, but no page arrived).", 0);
        return;
    }
    if (proc.modeSerial() != seenMode) { showMode(); return; }
    if (orbitPage)
    {
        if (proc.orbitRestoreSerial() != seenOrbitRestore) pushOrbitState();
        else
        {
            const auto ch = proc.orbitHostChanges (sentHost);
            if (! ch.isVoid()) js ("window.ORB && ORB.values(" + json (ch) + ")");
            if (proc.orbitViewSerial() != seenViews)
            {
                seenViews = proc.orbitViewSerial();
                juce::Array<juce::var> v;
                for (int l = 0; l < orbit::layers; l++) v.add (proc.orbitView (l));
                js ("window.ORB && ORB.views(" + json (v) + ")");
            }
        }
        js ("window.ORB && ORB.meter(" + json (proc.orbitMeter()) + ")");
        return;
    }
    if (proc.restoreSerial() != seenRestore)
    {
        seenRestore = proc.restoreSerial();
        pushRestore();
    }
    else
    {
        int serial = 0;
        const auto b64 = proc.takeHostChanges (serial);
        if (b64.isNotEmpty()) js ("window.MFHost && MFHost.patch(" + quoted (b64) + "," + juce::String (serial) + ")");
    }
    if (++ticks % 3 == 0) js ("window.MFHost && MFHost.status(" + json (proc.status()) + ")");
}

void VintageChonkEditor::onPanelEvent (const juce::var& v)
{
    const auto op = v["op"].toString();
    const auto* a = v["a"].getArray();
    auto arg = [a] (int i) { return a != nullptr && i < a->size() ? (*a)[i] : juce::var(); };
    if (op.startsWith ("o."))
    {
        onOrbitEvent (op, a != nullptr ? *a : juce::Array<juce::var>());
        return;
    }
    if (op == "patch")
    {
        const auto res = proc.panelPatch (arg (0).toString(), (int) arg (1));
        if (res != "ok") js ("window.MFHost && MFHost.toast(" + quoted ("Engine: " + res) + ")");
    }
    else if (op == "midi")
    {
        uint8_t m[3] = { (uint8_t) (int) arg (0), (uint8_t) ((int) arg (1) & 127), (uint8_t) ((int) arg (2) & 127) };
        const uint8_t st = m[0] & 0xF0;
        if (st == 0x80 || st == 0x90 || st == 0xB0 || st == 0xE0) proc.panelMidi (m, 3);
    }
    else if (op == "ui") proc.panelUiState (arg (0).toString());
    else if (op == "save")
    {
        const auto res = mfm1::PresetStore::save (arg (0).toString(), arg (1).toString());
        if (res != "ok") js ("window.MFHost && MFHost.toast(" + quoted ("Not saved: " + res) + ")");
        pushPresets();
    }
    else if (op == "delete")
    {
        mfm1::PresetStore::remove (arg (0).toString());
        pushPresets();
    }
    else if (op == "pick") pickNmsv();
    else if (op == "mode") proc.setMode (arg (0).toString() == "orbit" ? VintageChonkProcessor::orbitMode : VintageChonkProcessor::chonk);
    else if (op == "hello")
    {
        if (ui != Ui::ready) testStatus ("loaded regolith");
        ui = Ui::ready;
        orbitPage = false;
        pushPresets();
        pushRestore();
    }
}

void VintageChonkEditor::onOrbitEvent (const juce::String& op, const juce::Array<juce::var>& a)
{
    auto arg = [&a] (int i) { return i < a.size() ? a[i] : juce::var(); };
    if (op == "o.hello")
    {
        if (ui != Ui::ready) testStatus ("loaded orbit");
        ui = Ui::ready;
        orbitPage = true;
        pushOrbitState();
        return;
    }
    if (op == "o.pick") { pickSample ((int) arg (0), arg (1).toString() == "imp"); return; }
    if (op == "o.dropBegin")
    {
        drop.reset();
        dropName = arg (2).toString();
        dropLayer = (int) arg (0);
        dropImpulse = arg (1).toString() == "imp";
        return;
    }
    if (op == "o.dropChunk")
    {
        juce::MemoryOutputStream mo (drop, true);
        if (drop.getSize() < (size_t) (256 << 20)) juce::Base64::convertFromBase64 (mo, arg (0).toString());
        return;
    }
    if (op == "o.dropEnd")
    {
        const auto res = proc.loadSampleBytes (dropLayer, dropImpulse, dropName, drop);
        drop.reset();
        if (! res.startsWith ("ok")) js ("window.ORB && ORB.toast(" + quoted ("Not loaded: " + res) + ")");
        else if (res.length() > 2) js ("window.ORB && ORB.toast(" + quoted (dropName + res.substring (2)) + ")");
        return;
    }
    const auto r = proc.orbitOp (op, a);
    if (r.isString() && r.toString().isNotEmpty()) js ("window.ORB && ORB.toast(" + quoted (r.toString()) + ")");
}

void VintageChonkEditor::pickSample (int layer, bool impulse)
{
    chooser = std::make_unique<juce::FileChooser> (impulse ? "Load an impulse (WAV, AIFF, FLAC)" : "Load a sample (WAV, AIFF, FLAC)",
                                                   juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    juce::Component::SafePointer<VintageChonkEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe, layer, impulse] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              const auto res = safe->proc.loadSampleFile (layer, impulse, f);
                              if (! res.startsWith ("ok")) safe->js ("window.ORB && ORB.toast(" + quoted ("Not loaded: " + res) + ")");
                              else if (res.length() > 2) safe->js ("window.ORB && ORB.toast(" + quoted (f.getFileName() + res.substring (2)) + ")");
                          });
}

void VintageChonkEditor::pickNmsv()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a Massive preset (.nmsv)", juce::File(), "*.nmsv");
    juce::Component::SafePointer<VintageChonkEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f == juce::File())
                              {
                                  safe->js ("window.onNmsvPicked && onNmsvPicked(null, 'cancelled')");
                                  return;
                              }
                              juce::MemoryBlock mb;
                              if (f.getSize() > (4 << 20) || ! f.loadFileAsData (mb))
                              {
                                  safe->js ("window.onNmsvPicked && onNmsvPicked(null, " + quoted ("Could not read " + f.getFileName()) + ")");
                                  return;
                              }
                              safe->js ("window.onNmsvPicked && onNmsvPicked(" + quoted (f.getFileName()) + ", "
                                        + quoted (juce::Base64::toBase64 (mb.getData(), mb.getSize())) + ")");
                          });
}
