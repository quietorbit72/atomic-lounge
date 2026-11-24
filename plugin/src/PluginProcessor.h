// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk): the audio processor (engines, host parameters, state, programs).
// Two modes: REGOLITH (internally 'chonk': the 1.0.0 synth, EngineRunner) and ORBIT (the poly-convolution engine,
// orbit::Runner).
// Buses (1.2.0): one stereo audio INPUT bus "Sidechain" (input bus 0, VST3 bus type kAux, active by default; mono
// and disabled accepted) for the ORBIT live input, and the stereo (or mono) output. REGOLITH ignores the input.
// The mode is part of the plugin state; switching cross-fades the engines (30 ms) and reports the new
// latency to the host.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "EngineRunner.h"
#include "OrbitRunner.h"
#include "Params.h"

class VintageChonkProcessor final : public juce::AudioProcessor
{
public:
    enum Mode { chonk = 0, orbitMode = 1 };

    VintageChonkProcessor();
    ~VintageChonkProcessor() override;

    // juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Atomic Lounge"; }
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &vst3Ext; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return mode() == orbitMode ? 8.0 : 4.0; }

    int getNumPrograms() override;
    // host programs: the 16 CHONK factory presets, then the ORBIT factory presets ("ORBIT - <name>");
    // selecting one switches the mode. The current program follows the mode.
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- mode (message thread) -------------------------------------------------------------------
    int mode() const { return targetMode.load(); }
    void setMode (int m);
    int modeSerial() const { return modeCount.load(); }

    // ---- the CHONK editor's side (message thread) --------------------------------------------------
    // a patch from the panel (.mfp as base64). hostSerial: the last host push the panel had applied.
    juce::String panelPatch (const juce::String& b64, int hostSerial);
    void panelUiState (const juce::String& json);
    void panelMidi (const uint8_t* msg, int n);
    // host automation not yet shown on the panel: the patch to push (base64), else empty
    juce::String takeHostChanges (int& serialOut);
    // everything the panel needs to show the current sound: {b64, ui, serial}
    juce::var panelState();
    int restoreSerial() const { return restoreCount.load(); }   // bumps when the panel must reload (state, program)
    juce::var status();

    // ---- the ORBIT editor's side (message thread) --------------------------------------------------
    juce::var orbitState();                                   // everything the ORBIT panel shows
    juce::var orbitOp (const juce::String& op, const juce::Array<juce::var>& a);   // an edit; a reply or void
    juce::var orbitHostChanges (std::vector<float>& lastSent); // {index: norm} of moved host parameters
    juce::var orbitMeter();
    juce::var orbitView (int layer);
    int orbitViewSerial() const { return orbit.viewSerial(); }
    int orbitRestoreSerial() const { return orbitRestore.load(); }
    juce::String loadSampleFile (int layer, bool impulse, const juce::File& f);
    juce::String loadSampleBytes (int layer, bool impulse, const juce::String& name, const juce::MemoryBlock& data);

    static juce::String toB64 (const mfe_patch_t& p);
    static bool fromB64 (const juce::String& b64, mfe_patch_t& p, int& err);

    mfm1::EngineRunner& engine() { return runner; }
    orbit::Runner& orbitEngine() { return orbit; }

private:
    static bool patchHook (void* user, mfe_patch_t& p, bool isNew);
    void loadPatch (const mfe_patch_t& p, const juce::String& uiJson);   // state / program
    void mergeHostValues (mfe_patch_t& p) const;
    void setParamField (int i, int16_t f);

    // ORBIT
    void orbitApplyAll (const orbit::Settings& s);   // settings -> runner + host parameters
    void orbitPushRunner();                           // the settings' non-host parts -> runner
    void orbitSyncHost (orbit::Settings& s) const;    // host parameter values -> settings
    void setOrbitHost (int i, float v);
    void updateLatency();

    mfm1::EngineRunner runner;
    std::array<juce::AudioParameterFloat*, mfm1::numParams> params {};

    juce::CriticalSection lock;
    mfe_patch_t current {};        // what is playing (host values merged in when pushed or saved)
    mfe_patch_t lastPanel {};      // the patch the panel last had (sent or pushed): its edits are the diff
    juce::String uiState;          // panel JSON: current preset entry, edited flag
    int hostSerial = 0;            // pushes of host automation to the panel
    int currentProgram = 0;        // CHONK program 0-15 (the state's "program", as in 1.0.0)
    int orbitProgram = 0;          // ORBIT factory preset last chosen (the state's "orbitProgram")
    std::atomic<int> restoreCount { 0 };

    // ORBIT
    orbit::Runner orbit;
    std::vector<juce::AudioParameterFloat*> oparams;
    orbit::Settings osettings;      // message thread (under lock)
    std::atomic<bool> osync { false };
    std::atomic<int> orbitRestore { 0 };
    juce::StringArray orbitProblems;
    bool firstApply = true;

    // modes
    std::atomic<int> targetMode { chonk }, modeCount { 0 };
    int audioMode = chonk;          // audio thread: what is rendering
    int fadePos = 0, fadeLen = 1;
    std::vector<float> scratch[2];
    std::vector<float> inScratch[2];       // the sidechain input, copied before the output overwrites the shared buffer
    double fs = 44100.0;

    // the input bus is a sidechain (VST3 kAux), not a main input: DAWs offer it as "Audio From" / sidechain routing
    struct Vst3Ext final : juce::VST3ClientExtensions
    {
        bool getPluginHasMainInput() const override { return false; }
    } vst3Ext;

    std::vector<mfm1::MidiEvent> events;   // reserved in prepareToPlay; never grows on the audio thread

    // tells the host that the current program changed (on the message thread; cancelled if the plugin goes)
    struct ProgramNotify final : juce::AsyncUpdater
    {
        explicit ProgramNotify (VintageChonkProcessor& p) : owner (p) {}
        void handleAsyncUpdate() override { owner.updateHostDisplay (ChangeDetails().withProgramChanged (true)); }
        VintageChonkProcessor& owner;
    };
    ProgramNotify programNotify { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VintageChonkProcessor)
};
