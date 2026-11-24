// SPDX-License-Identifier: AGPL-3.0-only
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "factory_presets.h"
#include "PresetStore.h"

using namespace mfm1;

static juce::String factoryUi (int i)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", "F");
    o->setProperty ("name", juce::String (mfm1_factory::names[i]));
    o->setProperty ("dirty", false);
    return juce::JSON::toString (juce::var (o), true);
}

static bool factoryPatch (int i, mfe_patch_t& p)
{
    return i >= 0 && i < mfm1_factory::count
           && mfe_preset_read (&p, mfm1_factory::images[i], mfm1_factory::size) == 0;
}

VintageChonkProcessor::VintageChonkProcessor()
    : AudioProcessor (BusesProperties().withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    mfe_patch_t p;
    if (! factoryPatch (0, p)) mfe_patch_init (&p);
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        auto attrs = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [&d] (float v, int) { return juce::String (paramText (d, v)); });
        auto* prm = new juce::AudioParameterFloat (juce::ParameterID { d.id, 1 }, d.name,
                                                   juce::NormalisableRange<float> (0.0f, 1.0f),
                                                   valueFromField (d, *d.field (p)), attrs);
        params[(size_t) i] = prm;
        addParameter (prm);
    }
    // ORBIT: the curated host-automatable set (after the CHONK parameters, version hint 2)
    const auto& hp = orbit::hostParams();
    for (int i = 0; i < (int) hp.size(); i++)
    {
        const auto& h = hp[(size_t) i];
        auto attrs = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [this, i] (float v, int) { return orbit::hostText (orbit::hostParams()[(size_t) i], v, osync.load()); });
        auto* prm = new juce::AudioParameterFloat (juce::ParameterID { h.id, h.target >= orbit::spMotion && h.map == orbit::Map::special ? 3 : 2 }, h.name,
                                                   juce::NormalisableRange<float> (0.0f, 1.0f, h.stepped ? 1.0f : 0.0f),
                                                   orbit::hostNormFromSettings (h, osettings), attrs);
        oparams.push_back (prm);
        addParameter (prm);
    }
    current = lastPanel = p;
    uiState = factoryUi (0);
    runner.setPatchHook (&VintageChonkProcessor::patchHook, this);
    runner.postPatch (current);
    events.reserve (4096);
    osettings = orbit::factoryPreset (0);
    orbitApplyAll (osettings);
}

VintageChonkProcessor::~VintageChonkProcessor() = default;

// ------------------------------------------------------------------------------------- audio ---
void VintageChonkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    fs = sampleRate > 1000.0 ? sampleRate : 44100.0;
    runner.prepare (sampleRate, samplesPerBlock);
    orbit.prepare (sampleRate, samplesPerBlock);
    for (auto& s : scratch) s.assign ((size_t) std::max (4096, samplesPerBlock), 0.0f);
    for (auto& s : inScratch) s.assign ((size_t) std::max (8192, 2 * samplesPerBlock), 0.0f);
    audioMode = targetMode.load();
    fadePos = 0;
    fadeLen = std::max (1, (int) (0.03 * fs));
    updateLatency();
    events.reserve (4096);
}

void VintageChonkProcessor::reset()
{
    runner.reset();
    orbit.reset();
}

bool VintageChonkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // the sidechain: stereo, mono or switched off; the output: stereo or mono
    if (layouts.inputBuses.size() > 1) return false;
    if (layouts.inputBuses.size() == 1)
    {
        const auto in = layouts.inputBuses.getReference (0);
        if (! (in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono())) return false;
    }
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

bool VintageChonkProcessor::patchHook (void* user, mfe_patch_t& p, bool)
{
    // audio thread: the engine's patch always carries the host's parameter values
    auto* self = static_cast<VintageChonkProcessor*> (user);
    bool changed = false;
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        const int16_t f = fieldFromValue (d, self->params[(size_t) i]->get());
        int16_t* fld = d.field (p);
        if (*fld != f) { *fld = f; changed = true; }
    }
    return changed;
}

void VintageChonkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
            {
                runner.setTempo (*bpm);
                const auto ppq = pos->getPpqPosition();
                orbit.setTransport (*bpm, pos->getIsPlaying() && ppq.hasValue(), ppq.hasValue() ? *ppq : 0.0);
            }

    events.clear();   // keeps its capacity
    for (const auto meta : midi)
    {
        if (events.size() == events.capacity()) break;
        if (meta.numBytes < 1 || meta.numBytes > 3) continue;   // SysEx etc.: not for the engine
        MidiEvent e {};
        e.offset = meta.samplePosition;
        e.size = (uint8_t) meta.numBytes;
        std::memcpy (e.bytes, meta.data, (size_t) meta.numBytes);
        events.push_back (e);
    }
    // the sidechain input shares channels with the output: copied out first (ORBIT only; REGOLITH ignores it)
    const float* inL = nullptr;
    const float* inR = nullptr;
    if (getBusCount (true) > 0 && (targetMode.load() == orbitMode || audioMode == orbitMode) && n <= (int) inScratch[0].size())
    {
        const auto in = getBusBuffer (buffer, true, 0);
        if (in.getNumChannels() > 0)
        {
            std::memcpy (inScratch[0].data(), in.getReadPointer (0), sizeof (float) * (size_t) n);
            inL = inScratch[0].data();
            if (in.getNumChannels() > 1)
            {
                std::memcpy (inScratch[1].data(), in.getReadPointer (1), sizeof (float) * (size_t) n);
                inR = inScratch[1].data();
            }
        }
    }
    const int outCh = getBusCount (false) > 0 ? getChannelCountOfBus (false, 0) : 0;
    float* l = outCh > 0 ? buffer.getWritePointer (0) : nullptr;
    float* r = outCh > 1 ? buffer.getWritePointer (1) : nullptr;
    for (int i = 0; i < (int) oparams.size(); i++) orbit.setHostNorm (i, oparams[(size_t) i]->get());

    auto render = [&] (int m, float* a, float* b)
    {
        if (m == chonk) runner.process (a, b, n, events.data(), (int) events.size());   // b == nullptr: mono (L+R)/2 into a
        else orbit.process (a, b, inL, inR, n, events.data(), (int) events.size());
    };
    const int target = targetMode.load();
    if (target == audioMode)
    {
        if (fadePos > 0)
        {
            // a switch reversed mid-fade: the other engine goes quiet again
            if (audioMode == chonk) orbit.reset(); else runner.reset();
            fadePos = 0;
        }
        render (audioMode, l, r);
    }
    else if (n > (int) scratch[0].size())
    {
        render (target, l, r);                          // no room for a fade (block larger than announced)
        if (audioMode == chonk) runner.reset(); else orbit.reset();
        audioMode = target;
        fadePos = 0;
    }
    else
    {
        // equal-power cross-fade from the engine that was playing to the new one
        render (audioMode, l, r);
        float* s0 = scratch[0].data();
        float* s1 = r != nullptr ? scratch[1].data() : nullptr;
        render (target, s0, s1);
        for (int i = 0; i < n; i++)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (float) (fadePos + i) / (float) fadeLen);
            const float go = std::cos (t * 1.5707963f), gi = std::sin (t * 1.5707963f);
            if (l != nullptr) l[i] = l[i] * go + s0[i] * gi;
            if (r != nullptr) r[i] = r[i] * go + s1[i] * gi;
        }
        fadePos += n;
        if (fadePos >= fadeLen)
        {
            if (audioMode == chonk) runner.reset(); else orbit.reset();
            audioMode = target;
            fadePos = 0;
        }
    }
    for (int c = std::max (1, outCh); c < buffer.getNumChannels(); c++) buffer.clear (c, 0, n);
}

// ------------------------------------------------------------------------------------- modes ---
void VintageChonkProcessor::updateLatency()
{
    setLatencySamples (mode() == orbitMode ? orbit.latencySamples() : runner.latencySamples());
}

void VintageChonkProcessor::setMode (int m)
{
    m = m == orbitMode ? orbitMode : chonk;
    if (m == targetMode.load()) return;
    // the ORBIT engine is made the first time it is wanted; the audio thread has never touched it then
    if (m == orbitMode && ! orbit.hasEngine()) orbit.createEngine();
    targetMode.store (m);
    modeCount++;
    updateLatency();
    // the current host program follows the mode
    programNotify.triggerAsyncUpdate();
}

void VintageChonkProcessor::panelMidi (const uint8_t* msg, int n)
{
    if (mode() == orbitMode) orbit.postMidi (msg, n);
    else runner.postMidi (msg, n);
}

// ------------------------------------------------------------------------------------- state ---
juce::String VintageChonkProcessor::toB64 (const mfe_patch_t& p)
{
    uint8_t buf[2048];
    const size_t n = mfe_preset_write (&p, buf, sizeof buf);
    return juce::Base64::toBase64 (buf, n);
}

bool VintageChonkProcessor::fromB64 (const juce::String& b64, mfe_patch_t& p, int& err)
{
    juce::MemoryOutputStream mo;
    if (b64.length() > 8192 || ! juce::Base64::convertFromBase64 (mo, b64)) { err = -100; return false; }
    err = mfe_preset_read (&p, static_cast<const uint8_t*> (mo.getData()), mo.getDataSize());
    return err == 0;
}

void VintageChonkProcessor::mergeHostValues (mfe_patch_t& p) const
{
    for (int i = 0; i < numParams; i++)
        *paramDefs[i].field (p) = fieldFromValue (paramDefs[i], params[(size_t) i]->get());
}

void VintageChonkProcessor::setParamField (int i, int16_t f)
{
    auto* prm = params[(size_t) i];
    if (fieldFromValue (paramDefs[i], prm->get()) == f) return;
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (valueFromField (paramDefs[i], f));
    prm->endChangeGesture();
}

void VintageChonkProcessor::loadPatch (const mfe_patch_t& p, const juce::String& uiJson)
{
    {
        const juce::ScopedLock sl (lock);
        current = lastPanel = p;
        uiState = uiJson;
        hostSerial++;
    }
    for (int i = 0; i < numParams; i++) setParamField (i, *paramDefs[i].field (const_cast<mfe_patch_t&> (p)));
    runner.postPatch (p);
    restoreCount++;
}

void VintageChonkProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    mfe_patch_t p;
    juce::String ui;
    int prog, oprog;
    orbit::Settings os;
    {
        const juce::ScopedLock sl (lock);
        p = current;
        ui = uiState;
        prog = currentProgram;
        oprog = orbitProgram;
        os = osettings;
    }
    mergeHostValues (p);
    orbitSyncHost (os);
    juce::XmlElement x ("VINTAGECHONK");
    x.setAttribute ("version", 1);
    x.setAttribute ("mfp", toB64 (p));
    x.setAttribute ("ui", ui);
    x.setAttribute ("program", prog);
    x.setAttribute ("mode", mode() == orbitMode ? "orbit" : "chonk");
    x.setAttribute ("orbitProgram", oprog);
    x.addChildElement (orbit::toXml (os).release());
    copyXmlToBinary (x, destData);
}

void VintageChonkProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto x = getXmlFromBinary (data, sizeInBytes);
    if (x == nullptr || ! (x->hasTagName ("VINTAGECHONK")
                           || x->hasTagName ("MOOSIVEFM1"))) return;   // legacy name: the pre-release state tag, still read
    mfe_patch_t p;
    int err = 0;
    if (! fromB64 (x->getStringAttribute ("mfp"), p, err)) return;
    juce::String ui = x->getStringAttribute ("ui");
    if (! juce::JSON::parse (ui).isObject()) ui = "{}";
    currentProgram = juce::jlimit (0, mfm1_factory::count - 1, x->getIntAttribute ("program", 0));
    orbitProgram = juce::jlimit (0, orbit::factoryCount() - 1, x->getIntAttribute ("orbitProgram", 0));   // 1.0.0: absent
    loadPatch (p, ui);
    // ORBIT (absent in states from 1.0.0: those stay CHONK with the ORBIT settings as they are)
    if (auto* oe = x->getChildByName ("ORBIT"))
    {
        orbit::Settings os;
        juce::StringArray problems;
        if (orbit::fromXml (*oe, os, problems)) orbitApplyAll (os);
        orbitProblems = problems;
    }
    const bool wantOrbit = x->getStringAttribute ("mode") == "orbit";
    setMode (wantOrbit ? orbitMode : chonk);
    if (wantOrbit) orbit.waitIdle (20000);      // sources and impulses ready before the host plays
}

// ------------------------------------------------------------------------------------ programs ---
// 0-15: the CHONK factory presets (as in 1.0.0); 16-23: the ORBIT factory presets, "ORBIT - <name>"
int VintageChonkProcessor::getNumPrograms() { return mfm1_factory::count + orbit::factoryCount(); }

int VintageChonkProcessor::getCurrentProgram()
{
    return mode() == orbitMode ? mfm1_factory::count + orbitProgram : currentProgram;
}

const juce::String VintageChonkProcessor::getProgramName (int index)
{
    if (index >= 0 && index < mfm1_factory::count) return juce::String (mfm1_factory::names[index]);
    const int o = index - mfm1_factory::count;
    if (o >= 0 && o < orbit::factoryCount()) return "ORBIT - " + orbit::factoryName (o);
    return {};
}

void VintageChonkProcessor::setCurrentProgram (int index)
{
    // re-selecting the current program is ignored: some hosts call setCurrentProgram after restoring a
    // project, which would otherwise replace the restored (edited) sound with the factory one
    if (index == getCurrentProgram() || index < 0 || index >= getNumPrograms()) return;
    if (index < mfm1_factory::count)
    {
        mfe_patch_t p;
        if (! factoryPatch (index, p)) return;
        currentProgram = index;
        loadPatch (p, factoryUi (index));
        setMode (chonk);
        return;
    }
    const int o = index - mfm1_factory::count;
    orbitProgram = o;
    orbitApplyAll (orbit::factoryPreset (o));
    orbitProblems.clear();
    setMode (orbitMode);
    orbit.waitIdle (20000);      // sources and impulses ready before the host plays (as after a state load)
}

// ------------------------------------------------------------------------------------- panel ---
juce::String VintageChonkProcessor::panelPatch (const juce::String& b64, int serial)
{
    mfe_patch_t p;
    int err = 0;
    if (! fromB64 (b64, p, err)) return "preset error " + juce::String (err);
    mfe_patch_t posted = p;
    std::array<int16_t, numParams> userSet;
    std::array<bool, numParams> isUser {};
    {
        const juce::ScopedLock sl (lock);
        const bool stale = serial != hostSerial;   // sent before the panel saw the last automation push
        for (int i = 0; i < numParams; i++)
        {
            const ParamDef& d = paramDefs[i];
            const int16_t fp = *d.field (p), fl = *d.field (lastPanel);
            if (! stale && fp != fl) { isUser[(size_t) i] = true; userSet[(size_t) i] = fp; }
            else *d.field (posted) = fieldFromValue (d, params[(size_t) i]->get());
        }
        lastPanel = p;
        current = posted;
    }
    for (int i = 0; i < numParams; i++)
        if (isUser[(size_t) i]) setParamField (i, userSet[(size_t) i]);
    runner.postPatch (posted);
    return "ok";
}

void VintageChonkProcessor::panelUiState (const juce::String& json)
{
    if (json.length() > 4096) return;
    auto v = juce::JSON::parse (json);
    if (! v.isObject()) return;
    int prog = -1;
    if (v["kind"].toString() == "F")
        for (int i = 0; i < mfm1_factory::count; i++)
            if (v["name"].toString() == mfm1_factory::names[i]) prog = i;
    const juce::ScopedLock sl (lock);
    uiState = json;
    if (prog >= 0 && prog != currentProgram)
    {
        currentProgram = prog;
        programNotify.triggerAsyncUpdate();
    }
}

juce::String VintageChonkProcessor::takeHostChanges (int& serialOut)
{
    const juce::ScopedLock sl (lock);
    bool changed = false;
    for (int i = 0; i < numParams; i++)
    {
        const ParamDef& d = paramDefs[i];
        const int16_t f = fieldFromValue (d, params[(size_t) i]->get());
        if (*d.field (current) != f) { *d.field (current) = f; changed = true; }
    }
    if (! changed) return {};
    hostSerial++;
    lastPanel = current;
    serialOut = hostSerial;
    return toB64 (current);
}

juce::var VintageChonkProcessor::panelState()
{
    const juce::ScopedLock sl (lock);
    mergeHostValues (current);
    lastPanel = current;
    auto* o = new juce::DynamicObject();
    o->setProperty ("b64", toB64 (current));
    o->setProperty ("ui", juce::JSON::parse (uiState));
    o->setProperty ("serial", hostSerial);
    return juce::var (o);
}

juce::var VintageChonkProcessor::status()
{
    auto* o = new juce::DynamicObject();
    const double rate = runner.hostRate();
    o->setProperty ("voices", (int) runner.activeVoices());
    o->setProperty ("midi", (int) runner.midiCount());
    o->setProperty ("sampleRate", rate);
    o->setProperty ("latencyMs", rate > 0 ? runner.latencySamples() * 1000.0 / rate : 0.0);
    o->setProperty ("underruns", 0);
    o->setProperty ("dropped", (int) runner.droppedEvents());
    o->setProperty ("midiDevices", juce::Array<juce::var> { juce::var ("HOST MIDI") });
    return juce::var (o);
}

// ------------------------------------------------------------------------------------- ORBIT ---
void VintageChonkProcessor::orbitSyncHost (orbit::Settings& s) const
{
    const auto& hp = orbit::hostParams();
    for (int i = 0; i < (int) hp.size(); i++) orbit::hostNormToSettings (hp[(size_t) i], oparams[(size_t) i]->get(), s);
}

void VintageChonkProcessor::setOrbitHost (int i, float v)
{
    if (i < 0 || i >= (int) oparams.size()) return;
    auto* prm = oparams[(size_t) i];
    v = juce::jlimit (0.0f, 1.0f, v);
    if (std::fabs (prm->get() - v) < 1e-7f) return;
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (v);
    prm->endChangeGesture();
}

void VintageChonkProcessor::orbitPushRunner()
{
    orbit::Settings s;
    {
        const juce::ScopedLock sl (lock);
        s = osettings;
    }
    for (int i = 0; i < orbit::numCore; i++) orbit.setCore (i, s.core[(size_t) i]);
    orbit.setPath (s.path != orbit::pathHold ? s.path : s.shape, s.sync, s.drawn);
    orbit.setExtras (s.legacyWobble, s.inWindow);
    orbit.setFx (s.fxOn, s.ensMode, s.revType, s.bbdSync, s.oilSync);
    orbit.setMods (s.mods);
    osync.store (s.sync);
}

static bool sameSample (const orbit::SampleRef& a, const orbit::SampleRef& b)
{
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return a->length() == b->length() && a->channels == b->channels && a->rate == b->rate && a->ch[0] == b->ch[0]
           && (a->channels < 2 || a->ch[1] == b->ch[1]);
}
template <typename Spec>
static bool sameSpec (const Spec& a, const Spec& b)
{
    return juce::JSON::toString (orbit::specVar (a), true) == juce::JSON::toString (orbit::specVar (b), true) && sameSample (a.sample, b.sample);
}

void VintageChonkProcessor::orbitApplyAll (const orbit::Settings& s)
{
    orbit::Settings old;
    {
        const juce::ScopedLock sl (lock);
        old = osettings;
        osettings = s;
    }
    const auto& hp = orbit::hostParams();
    for (int i = 0; i < (int) hp.size(); i++) setOrbitHost (i, orbit::hostNormFromSettings (hp[(size_t) i], s));
    orbitPushRunner();
    // what did not change is not rebuilt (a re-applied state keeps the notes playing)
    for (int l = 0; l < orbit::layers; l++)
    {
        if (! firstApply && sameSpec (s.src[l], old.src[l])) {} else orbit.requestSource (l, s.src[l]);
        if (! firstApply && sameSpec (s.imp[l], old.imp[l])) {} else orbit.requestImpulse (l, s.imp[l]);
    }
    firstApply = false;
    orbitRestore++;
}

juce::var VintageChonkProcessor::orbitView (int l)
{
    const auto v = orbit.view (l);
    auto peaks = [] (const std::vector<float>& p)
    {
        juce::Array<juce::var> a;
        a.ensureStorageAllocated ((int) p.size());
        for (float x : p) a.add (std::round (x * 1000.0f) / 1000.0f);
        return a;
    };
    auto* o = new juce::DynamicObject();
    o->setProperty ("layer", l);
    o->setProperty ("srcName", v.srcName);
    o->setProperty ("srcSub", v.srcSub);
    o->setProperty ("srcPeaks", peaks (v.srcPeaks));
    o->setProperty ("srcTrace", v.srcTrace);
    o->setProperty ("srcError", v.srcError);
    o->setProperty ("impName", v.impName);
    o->setProperty ("impSub", v.impSub);
    o->setProperty ("impError", v.impError);
    std::vector<float> ip = v.impPeaks;
    bool impTrace = false;
    {
        const juce::ScopedLock sl (lock);
        if (osettings.imp[l].kind == orbit::Imp::live)
        {
            const auto other = orbit.view (osettings.imp[l].liveLayer == l ? (l + 1) % orbit::layers : osettings.imp[l].liveLayer);
            ip = other.srcPeaks;
            impTrace = other.srcTrace;
        }
    }
    o->setProperty ("impPeaks", peaks (ip));
    o->setProperty ("impTrace", impTrace);
    {
        const auto& hp = orbit::hostParams();
        auto on = [&] (int sp) { const int i = orbit::hostIndexForSpecial (sp); return i >= 0 && oparams[(size_t) i]->get() > 0.5f; };
        juce::ignoreUnused (hp);
        if (on (orbit::spSrcIn0 + l))
        {
            o->setProperty ("srcName", "LIVE INPUT");
            o->setProperty ("srcSub", juce::String::fromUTF8 ("SIDECHAIN \xc2\xb7 ") + (on (orbit::spGate0 + l) ? "NOTE GATE" : "THRU"));
            o->setProperty ("srcPeaks", juce::Array<juce::var>());
            o->setProperty ("srcTrace", false);
            o->setProperty ("srcError", "");
        }
        if (on (orbit::spImpIn0 + l))
        {
            float w;
            {
                const juce::ScopedLock sl (lock);
                w = osettings.inWindow[l];
            }
            o->setProperty ("impName", "LIVE INPUT");
            o->setProperty ("impSub", juce::String::fromUTF8 ("SIDECHAIN \xc2\xb7 ") + juce::String ((int) std::lround (w * 1000.0f)) + " ms WINDOW");
            o->setProperty ("impPeaks", juce::Array<juce::var>());
            o->setProperty ("impTrace", false);
            o->setProperty ("impError", "");
        }
    }
    return juce::var (o);
}

juce::var VintageChonkProcessor::orbitState()
{
    orbit::Settings s;
    {
        const juce::ScopedLock sl (lock);
        s = osettings;
    }
    orbitSyncHost (s);
    auto* o = new juce::DynamicObject();
    o->setProperty ("settings", orbit::toVar (s));
    juce::Array<juce::var> host;
    const auto& hp = orbit::hostParams();
    for (const auto& h : hp)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("id", h.id);
        e->setProperty ("name", h.name);
        e->setProperty ("target", h.target);
        e->setProperty ("map", h.map == orbit::Map::lin ? "lin" : h.map == orbit::Map::sq2 ? "sq2" : h.map == orbit::Map::log ? "log" : "special");
        host.add (juce::var (e));
    }
    o->setProperty ("host", host);
    juce::Array<juce::var> hv;
    for (auto* p : oparams) hv.add (p->get());
    o->setProperty ("hostValues", hv);
    juce::Array<juce::var> core;
    for (int i = 0; i < orbit::numCore; i++)
    {
        const auto* in = mfo_param_info (i);
        auto* e = new juce::DynamicObject();
        e->setProperty ("name", juce::String (in->name));
        e->setProperty ("min", in->min);
        e->setProperty ("max", in->max);
        e->setProperty ("def", in->def);
        e->setProperty ("int", (in->flags & MFO_PF_INT) != 0);
        e->setProperty ("log", (in->flags & MFO_PF_LOG) != 0);
        core.add (juce::var (e));
    }
    o->setProperty ("core", core);
    o->setProperty ("chonk", orbit::chonkPresetList());
    juce::Array<juce::var> fac;
    for (int i = 0; i < orbit::factoryCount(); i++) fac.add (orbit::factoryName (i));
    o->setProperty ("factory", fac);
    o->setProperty ("user", orbit::userPresetList());
    juce::Array<juce::var> gens;
    for (int i = 0; i < MFO_GEN_COUNT; i++) gens.add (juce::String (mfo_generate_name (i)));
    o->setProperty ("gens", gens);
    juce::Array<juce::var> views;
    for (int l = 0; l < orbit::layers; l++) views.add (orbitView (l));
    o->setProperty ("views", views);
    juce::Array<juce::var> probs;
    for (const auto& p : orbitProblems) probs.add (p);
    o->setProperty ("problems", probs);
    o->setProperty ("meter", orbitMeter());
    return juce::var (o);
}

juce::var VintageChonkProcessor::orbitHostChanges (std::vector<float>& last)
{
    if (last.size() != oparams.size()) last.assign (oparams.size(), -1.0f);
    juce::DynamicObject::Ptr o;
    for (int i = 0; i < (int) oparams.size(); i++)
    {
        const float v = oparams[(size_t) i]->get();
        if (v == last[(size_t) i]) continue;
        last[(size_t) i] = v;
        if (o == nullptr) o = new juce::DynamicObject();
        o->setProperty (juce::String (i), v);
    }
    return o != nullptr ? juce::var (o.get()) : juce::var();
}

juce::var VintageChonkProcessor::orbitMeter()
{
    const auto m = orbit.meter();
    auto* o = new juce::DynamicObject();
    o->setProperty ("cpu", m.cpu);
    o->setProperty ("voices", (int) m.voices);
    o->setProperty ("rate", fs);
    o->setProperty ("latencyMs", fs > 0 ? orbit.latencySamples() * 1000.0 / fs : 0.0);
    o->setProperty ("puck", juce::Array<juce::var> { m.puckX, m.puckY });
    o->setProperty ("w", juce::Array<juce::var> { m.w[0], m.w[1], m.w[2], m.w[3] });
    o->setProperty ("vu", juce::Array<juce::var> { m.vuL, m.vuR });
    o->setProperty ("pk", juce::Array<juce::var> { m.peakL, m.peakR });
    o->setProperty ("midi", (int) orbit.midiCount());
    o->setProperty ("mode", mode() == orbitMode ? "orbit" : "chonk");
    o->setProperty ("in", juce::Array<juce::var> { m.inL, m.inR, m.inPk });
    // the bus as the host has it (the audio thread reports 0 while nothing arrives, e.g. REGOLITH or no bus)
    int inCh = getBusCount (true) > 0 ? getChannelCountOfBus (true, 0) : 0;
    if (auto* b = getBus (true, 0)) if (! b->isEnabled()) inCh = 0;
    o->setProperty ("inCh", inCh);
    o->setProperty ("inLatencyMs", fs > 0 ? orbit.latencySamples() * 1000.0 / fs : 0.0);
    return juce::var (o);
}

juce::String VintageChonkProcessor::loadSampleFile (int l, bool impulse, const juce::File& f)
{
    juce::String err;
    auto d = orbit::decodeFile (f, err);
    if (d == nullptr) return err;
    {
        const juce::ScopedLock sl (lock);
        l = juce::jlimit (0, orbit::layers - 1, l);
        if (impulse)
        {
            auto& s = osettings.imp[l];
            s.kind = orbit::Imp::sample;
            s.from = orbit::SampleFrom::file;
            s.sample = d;
            s.missing = {};
            s.root = d->detectedRoot >= 0 ? std::round (d->detectedRoot) : 60.0f;
            s.start = 0.0f;
            s.length = 0.0f;
        }
        else
        {
            auto& s = osettings.src[l];
            s.kind = orbit::Src::sample;
            s.from = orbit::SampleFrom::file;
            s.sample = d;
            s.missing = {};
            s.root = d->detectedRoot >= 0 ? std::round (d->detectedRoot) : 60.0f;
        }
        osettings.dirty = true;
        if (impulse) orbit.requestImpulse (l, osettings.imp[l]);
        else orbit.requestSource (l, osettings.src[l]);
    }
    orbitRestore++;
    juce::String note;
    if (d->flac.getSize() == 0) note = " (too large to embed in the project: kept by path)";
    return "ok" + note;
}

juce::String VintageChonkProcessor::loadSampleBytes (int l, bool impulse, const juce::String& name, const juce::MemoryBlock& data)
{
    // dropped on the panel: kept in %APPDATA%\Atomic Lounge\samples (or the 1.1.0 folder) so the project can reference it by path
    auto dir = orbit::droppedFolder();
    dir.createDirectory();
    const auto safe = juce::File::createLegalFileName (name.isNotEmpty() ? name : juce::String ("dropped.wav"));
    auto f = dir.getChildFile (safe);
    if (f.existsAsFile())
    {
        juce::MemoryBlock old;
        if (! f.loadFileAsData (old) || old != data) f = f.getNonexistentSibling (false);
    }
    if (! f.existsAsFile() && ! f.replaceWithData (data.getData(), data.getSize())) return "cannot write " + f.getFullPathName();
    return loadSampleFile (l, impulse, f);
}

juce::var VintageChonkProcessor::orbitOp (const juce::String& op, const juce::Array<juce::var>& a)
{
    auto arg = [&a] (int i) { return i < a.size() ? a[i] : juce::var(); };
    const int l = juce::jlimit (0, orbit::layers - 1, (int) arg (0));
    bool edited = true, restore = false;
    if (op == "o.host")
    {
        setOrbitHost ((int) arg (0), (float) (double) arg (1));
    }
    else if (op == "o.set")
    {
        const int id = (int) arg (0);
        if (id < 0 || id >= orbit::numCore) return {};
        const int hi = orbit::hostIndexForCore (id);
        if (hi >= 0) setOrbitHost (hi, orbit::normFromCore (orbit::hostParams()[(size_t) hi], (float) (double) arg (1)));
        else
        {
            const auto* in = mfo_param_info (id);
            const float v = juce::jlimit (in->min, in->max, (float) (double) arg (1));
            {
                const juce::ScopedLock sl (lock);
                osettings.core[(size_t) id] = v;
            }
            orbit.setCore (id, v);
        }
    }
    else if (op == "o.motion" || op == "o.park")
    {
        // motion on / off; park: the puck where the hand put it, motion off (the panel's drag while it moves)
        const bool on = op == "o.motion" && (bool) arg (0);
        if (op == "o.park")
        {
            const auto& hp = orbit::hostParams();
            const int hx = orbit::hostIndexForCore (MFO_P_PUCK_X), hy = orbit::hostIndexForCore (MFO_P_PUCK_Y);
            if (hx >= 0) setOrbitHost (hx, orbit::normFromCore (hp[(size_t) hx], (float) (double) arg (0)));
            if (hy >= 0) setOrbitHost (hy, orbit::normFromCore (hp[(size_t) hy], (float) (double) arg (1)));
        }
        {
            const juce::ScopedLock sl (lock);
            if (osettings.path != orbit::pathHold) osettings.shape = osettings.path;
            osettings.path = on ? osettings.shape : (int) orbit::pathHold;
        }
        setOrbitHost (orbit::hostIndexForSpecial (orbit::spMotion), on ? 1.0f : 0.0f);
        orbitPushRunner();
    }
    else if (op == "o.window")
    {
        {
            const juce::ScopedLock sl (lock);
            osettings.inWindow[l] = orbit::windowFromChoice ((float) (double) arg (1));
        }
        orbitPushRunner();
    }
    else if (op == "o.path" || op == "o.sync" || op == "o.drawn")
    {
        bool motionOn = false, motionSet = false;
        {
            const juce::ScopedLock sl (lock);
            if (op == "o.path")
            {
                osettings.path = juce::jlimit (0, 3, (int) arg (0));
                if (osettings.path != orbit::pathHold) osettings.shape = osettings.path;
                motionOn = osettings.path != orbit::pathHold;
                motionSet = true;
            }
            else if (op == "o.sync") osettings.sync = (bool) arg (0);
            else if (const auto* pts = arg (0).getArray())
            {
                auto& d = osettings.drawn;
                d.n = 0;
                for (const auto& p : *pts)
                {
                    if (d.n >= orbit::maxDrawn || ! p.isArray() || p.size() < 2) continue;
                    d.x[d.n] = juce::jlimit (-1.0f, 1.0f, (float) (double) p[0]);
                    d.y[d.n] = juce::jlimit (-1.0f, 1.0f, (float) (double) p[1]);
                    d.n++;
                }
                osettings.path = orbit::pathDrawn;
                osettings.shape = orbit::pathDrawn;
                motionOn = motionSet = true;
            }
        }
        if (motionSet) setOrbitHost (orbit::hostIndexForSpecial (orbit::spMotion), motionOn ? 1.0f : 0.0f);
        orbitPushRunner();
    }
    else if (op == "o.fx" || op == "o.fxopt")
    {
        {
            const juce::ScopedLock sl (lock);
            if (op == "o.fx") { const int i = (int) arg (0); if (i >= 0 && i < orbit::numFx) osettings.fxOn[i] = (bool) arg (1); }
            else
            {
                const auto o = arg (0);
                if (o.hasProperty ("ens")) osettings.ensMode = juce::jlimit (1, 4, (int) o["ens"]);
                if (o.hasProperty ("rev")) osettings.revType = juce::jlimit (0, 1, (int) o["rev"]);
                if (o.hasProperty ("bbdSync")) osettings.bbdSync = juce::jlimit (0, MFE_SYNC_DIVS, (int) o["bbdSync"]);
                if (o.hasProperty ("oilSync")) osettings.oilSync = juce::jlimit (0, MFE_SYNC_DIVS, (int) o["oilSync"]);
            }
        }
        orbitPushRunner();
    }
    else if (op == "o.src" || op == "o.imp")
    {
        const juce::ScopedLock sl (lock);
        if (op == "o.src")
        {
            orbit::specFromVar (arg (1), osettings.src[l]);
            orbit.requestSource (l, osettings.src[l]);
            // keytrack sets depend on the source kind: the impulse follows
            orbit.requestImpulse (l, osettings.imp[l]);
        }
        else
        {
            orbit::specFromVar (arg (1), osettings.imp[l]);
            orbit.requestImpulse (l, osettings.imp[l]);
        }
        restore = true;
    }
    else if (op == "o.layerName")
    {
        const juce::ScopedLock sl (lock);
        osettings.layerName[l] = arg (1).toString().toUpperCase().substring (0, 20);
    }
    else if (op == "o.macroName")
    {
        const juce::ScopedLock sl (lock);
        const int k = juce::jlimit (0, orbit::numMacros - 1, (int) arg (0));
        osettings.macroName[k] = arg (1).toString().toUpperCase().substring (0, 14);
    }
    else if (op == "o.assign" || op == "o.mod" || op == "o.unassign")
    {
        {
            const juce::ScopedLock sl (lock);
            auto& m = osettings.mods;
            if (op == "o.assign")
            {
                const int src = juce::jlimit (1, MFO_MS_COUNT - 1, (int) arg (0));
                const int dst = juce::jlimit (0, orbit::numCore - 1, (int) arg (1));
                int slot = -1;
                for (int i = 0; i < MFO_NMOD && slot < 0; i++) if (m[(size_t) i].src == src && m[(size_t) i].dst == dst) slot = i;
                for (int i = 0; i < MFO_NMOD && slot < 0; i++) if (m[(size_t) i].src == 0 || m[(size_t) i].amount == 0.0f) slot = i;
                if (slot < 0) return "all 32 modulation slots are in use";
                m[(size_t) slot] = { src, dst, a.size() > 2 ? juce::jlimit (-1.0f, 1.0f, (float) (double) arg (2)) : 0.5f };
            }
            else if (op == "o.mod")
            {
                const int slot = (int) arg (0);
                if (slot < 0 || slot >= MFO_NMOD) return {};
                m[(size_t) slot] = { juce::jlimit (0, MFO_MS_COUNT - 1, (int) arg (1)), juce::jlimit (0, orbit::numCore - 1, (int) arg (2)),
                                     juce::jlimit (-1.0f, 1.0f, (float) (double) arg (3)) };
            }
            else
            {
                const int slot = (int) arg (0);
                if (slot >= 0 && slot < MFO_NMOD) m[(size_t) slot] = {};
            }
        }
        orbitPushRunner();
        restore = op != "o.mod";
    }
    else if (op == "o.preset")
    {
        orbit::Settings s;
        juce::String err;
        if (arg (0).toString() == "F")
        {
            const int i = (int) arg (1);
            if (i < 0 || i >= orbit::factoryCount()) return "no such preset";
            s = orbit::factoryPreset (i);
            if (orbitProgram != i)
            {
                orbitProgram = i;
                programNotify.triggerAsyncUpdate();
            }
        }
        else if (! orbit::loadUserPreset (arg (1).toString(), s, err)) return err;
        orbitApplyAll (s);
        orbitProblems.clear();
        if (err.isNotEmpty()) orbitProblems.add (err);
        return {};
    }
    else if (op == "o.save")
    {
        orbit::Settings s;
        {
            const juce::ScopedLock sl (lock);
            s = osettings;
        }
        orbitSyncHost (s);
        const auto res = orbit::saveUserPreset (arg (0).toString(), s);
        if (res != "ok") return "Not saved: " + res;
        {
            const juce::ScopedLock sl (lock);
            osettings.presetName = mfm1::PresetStore::safeName (arg (0).toString());
            osettings.presetKind = "U";
            osettings.dirty = false;
        }
        orbitRestore++;
        return {};
    }
    else if (op == "o.delete")
    {
        orbit::deleteUserPreset (arg (0).toString());
        orbitRestore++;
        return {};
    }
    else if (op == "o.mode")
    {
        setMode (arg (0).toString() == "orbit" ? orbitMode : chonk);
        return {};
    }
    else
        return "unknown op " + op;
    if (edited)
    {
        const juce::ScopedLock sl (lock);
        osettings.dirty = true;
    }
    if (restore) orbitRestore++;
    return {};
}

juce::AudioProcessorEditor* VintageChonkProcessor::createEditor() { return new VintageChonkEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VintageChonkProcessor(); }
