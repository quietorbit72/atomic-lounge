// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk), ORBIT mode: the settings model (what a preset / the plugin state holds), the source
// and impulse specs the worker builds from, and the curated host-automatable parameter set.
// Message-thread objects; the audio side is OrbitRunner.
#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include <vector>
extern "C" {
#include "mfo.h"
}

namespace orbit {

constexpr int layers = MFO_LAYERS;
constexpr int numCore = MFO_PARAM_COUNT;    // 150
constexpr int numFx = 6;                     // OCT FUZZ, TAPE, ENSEMBLE, BBD ECHO, OIL CAN, REVERB
constexpr int numMacros = 4;
constexpr int maxDrawn = 64;
inline const char* const fxNames[numFx] = { "OCT FUZZ", "TAPE", "ENSEMBLE", "BBD ECHO", "OIL CAN", "REVERB" };

// ---------------------------------------------------------------------------------- samples ---
// decoded audio (float, its own rate), immutable once made, shared by specs, assets and the state
struct SampleData
{
    juce::String name, path;              // path: where it was loaded from (may be gone on another PC)
    double rate = 48000.0;
    int channels = 1;
    std::vector<float> ch[2];
    float detectedRoot = 60.0f;           // MIDI note from the pitch detector (fractional), -1 = none found
    juce::MemoryBlock flac;               // the audio as FLAC for the plugin state (empty: too big to embed)
    int length() const { return (int) ch[0].size(); }
    double seconds() const { return rate > 0 ? length() / rate : 0.0; }
};
using SampleRef = std::shared_ptr<const SampleData>;

// size cap for embedding a sample in the plugin state / a preset (FLAC bytes); bigger ones are kept by path
constexpr size_t embedCapBytes = 24u << 20;
constexpr double maxSampleSeconds = 120.0;      // longer files are cut (the engine plays from memory)

// ------------------------------------------------------------------------------------ specs ---
enum class Src { off, sample, synth };
enum class SampleFrom { file, print, generated };   // a sample source / impulse: a file, a Chonk print, a factory impulse
enum class Imp { off, sample, synth, live };

struct ChonkPatchRef
{
    juce::String name = "Walnut Bass";
    juce::String mfp;                     // the .mfp as base64 (factory or user preset), empty = factory by name
};

struct SourceSpec
{
    Src kind = Src::synth;
    ChonkPatchRef patch;                  // SYNTH, or SAMPLE printed from a Chonk preset
    SampleFrom from = SampleFrom::file;   // SAMPLE: file or print
    SampleRef sample;                     // SAMPLE from a file (decoded)
    juce::String missing;                 // a file that could not be found / read (shown on the panel)
    int printNote = 60;
    float printSeconds = 2.0f, printHold = 1.2f;
    float root = 60.0f;                   // SAMPLE: the recorded pitch (detected, editable)
};

struct ImpulseSpec
{
    Imp kind = Imp::sample;
    SampleFrom from = SampleFrom::generated;   // SAMPLE: generated (factory impulse) or file
    int genType = MFO_GEN_SPRING;
    float genSeconds = 2.0f;
    uint32_t seed = 1;
    SampleRef sample;                     // SAMPLE from a file
    juce::String missing;
    ChonkPatchRef patch;                  // SYNTH print
    int printNote = 60;
    float printSeconds = 1.5f, printHold = 0.5f;
    int liveLayer = 0;                    // LIVE: the other layer (0..2)
    float root = 60.0f;                   // keytrack reference
    // shaping (mfo_ir_prepare)
    float start = 0.0f, length = 0.0f, pitch = 0.0f, fadeIn = 0.0f, fadeOut = 0.01f;
    float tilt = 0.0f, damp = 0.0f, age = 0.0f;
    bool reverse = false;
    float gain = 0.8f;                    // BODY kernel gain (unit energy x gain)
};

struct ModSlot { int src = 0, dst = 0; float amount = 0.0f; };

struct DrawnPath
{
    int n = 0;
    float x[maxDrawn] {}, y[maxDrawn] {};
};

enum PathMode { pathHold, pathCircle, pathFig8, pathDrawn };

struct Settings
{
    std::array<float, numCore> core {};   // every core parameter (the set values)
    SourceSpec src[layers];
    ImpulseSpec imp[layers];
    juce::String layerName[layers];
    int path = pathHold;
    bool sync = false;
    float rate = 0.35f;                   // normalised (host param orbit_rate)
    float spread = 0.7f;                  // normalised orbit size
    float tapeAge = 0.0f;
    DrawnPath drawn;
    bool fxOn[numFx] {};
    float fxAmt[numFx] { 0.3f, 0.35f, 0.4f, 0.3f, 0.3f, 0.3f };
    int ensMode = MFE_ENS_I, revType = MFE_REV_TWIN_TANK, bbdSync = 0, oilSync = 0;
    float macro[numMacros] { 0.5f, 0.5f, 0.5f, 0.5f };
    juce::String macroName[numMacros] { "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4" };
    std::array<ModSlot, MFO_NMOD> mods {};
    juce::String presetName = "INIT", presetKind = "F";
    bool dirty = false;
    // 1.2.0
    int shape = pathCircle;               // the moving path that ORBIT Motion turns on (path = pathHold while off)
    float puckDrift = 0.0f;               // wobble of a held puck (host param orbit_puck_drift)
    bool legacyWobble = false;            // 1.1.0 states and presets: ORBIT WOBBLE moves a held puck, RATE 0 creeps
    float inTrimDb = 0.0f;                // live input trim, -24..+24 dB
    bool srcInput[layers] {}, impInput[layers] {};   // the layer's SOURCE / IMPULSE is the live input
    int inGate[layers] {};                // SOURCE INPUT: 0 THRU, 1 NOTE
    float inWindow[layers] { 0.25f, 0.25f, 0.25f };  // IMPULSE INPUT: BODY window, seconds

    Settings();                           // core at the engine defaults, a usable init sound
};

// --------------------------------------------------------------------------- host parameters ---
enum class Map { lin, sq2, log, special };
enum Special { spRate = 1000, spSpread, spTapeAge, spFx0, spMacro0 = spFx0 + numFx,
               // 1.2.0 (appended: the existing host indices stay)
               spMotion = spMacro0 + numMacros, spPuckDrift, spInTrim, spSrcIn0, spImpIn0 = spSrcIn0 + layers,
               spGate0 = spImpIn0 + layers };
constexpr float inTrimRange = 24.0f;                   // ORBIT Input Trim: -24..+24 dB
inline float inTrimDbFromNorm (float v) { return (v * 2.0f - 1.0f) * inTrimRange; }
inline float windowFromChoice (float s) { return s <= 0.17f ? 0.1f : s <= 0.37f ? 0.25f : s <= 0.75f ? 0.5f : 1.0f; }
struct HostParam
{
    juce::String id, name;
    int target;                           // core id, or Special
    Map map;
    bool stepped = false;                 // on / off (VST3 step count 1)
};
const std::vector<HostParam>& hostParams();           // the curated set (42 in 1.1.0, 54 from 1.2.0)
int hostIndexForSpecial (int special);                // -1 if none
int hostIndexForCore (int coreId);                    // -1 if the core param is not host-automatable
float hostNormFromSettings (const HostParam& h, const Settings& s);
void hostNormToSettings (const HostParam& h, float v, Settings& s);
float coreFromNorm (const HostParam& h, float v);    // core targets only
float normFromCore (const HostParam& h, float c);
juce::String hostText (const HostParam& h, float v, bool sync);

// orbit rate: free Hz or synced beats per cycle
float rateHz (float norm);
int rateBeats (float norm);                           // 1..64
juce::String rateText (float norm, bool sync);

// ------------------------------------------------------------------------------- serialising ---
// JSON for the panel and the presets (samples by path + reference id); the plugin state adds the audio
juce::var toVar (const Settings& s);
bool fromVar (const juce::var& v, Settings& s, juce::String& err);
juce::var specVar (const SourceSpec& s);
juce::var specVar (const ImpulseSpec& s);
void specFromVar (const juce::var& v, SourceSpec& s);
void specFromVar (const juce::var& v, ImpulseSpec& s);

// the plugin state element <ORBIT> with embedded samples (FLAC, base64) and the path fallback
std::unique_ptr<juce::XmlElement> toXml (const Settings& s);
bool fromXml (const juce::XmlElement& x, Settings& s, juce::StringArray& problems);

// -------------------------------------------------------------------------------- utilities ---
SampleRef decodeFile (const juce::File& f, juce::String& err);                 // WAV / AIFF / FLAC (+ root, FLAC copy)
SampleRef decodeMemory (const void* data, size_t size, const juce::String& name, const juce::String& path, juce::String& err);
float detectRoot (const SampleData& s);               // YIN on the steady part; MIDI note (fractional) or -1
bool chonkPatch (const ChonkPatchRef& r, mfe_patch_t& p);   // the referenced Chonk preset
juce::var chonkPresetList();                          // [{name, kind F|U, mfp}] factory + user presets

// factory presets (generated impulses and Chonk synth sources only)
int factoryCount();
Settings factoryPreset (int i);
juce::String factoryName (int i);

// user presets: %APPDATA%\Vintage Chonk\orbit\*.orbit (JSON)
juce::File userFolder();
juce::var userPresetList();                           // [{name}]
juce::String saveUserPreset (const juce::String& name, const Settings& s);
bool loadUserPreset (const juce::String& name, Settings& s, juce::String& err);
juce::String deleteUserPreset (const juce::String& name);

// dropped files are kept here so a project can reference them by path
juce::File droppedFolder();

}
