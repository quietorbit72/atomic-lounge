// SPDX-License-Identifier: AGPL-3.0-only
// Atomic Lounge plugin (formerly Vintage Chonk): the user preset folder (%APPDATA%\Atomic Lounge\presets\*.mfp, or the
// 1.1.0 folder %APPDATA%\Vintage Chonk\presets while the new one does not exist; presets in the
// pre-rename folder are copied in, never overwriting: PresetStore::oldFolder), the desktop
// counterpart of the tablet's app-private presets folder. Same contract as MainActivity.Bridge.
#pragma once
#include <juce_core/juce_core.h>

namespace mfm1 {

struct PresetStore
{
    static juce::File folder();
    // %APPDATA%\Atomic Lounge\<sub>; while that does not exist and %APPDATA%\Vintage Chonk\<sub> does (1.1.0), the old one
    // (read and used where it is: nothing is moved or deleted)
    static juce::File dataFolder (const juce::String& sub);
    static juce::File oldFolder();                              // pre-rename folder (read for migration)
    static int migrate (const juce::File& from, const juce::File& to);   // copies missing *.mfp; count
    static juce::String safeName (const juce::String& name);   // as the tablet: [A-Za-z0-9 _.-], trimmed
    static juce::var list();                                    // [{name, b64}]
    static juce::String save (const juce::String& name, const juce::String& b64);   // "ok" | error
    static juce::String remove (const juce::String& name);                          // "ok" | "not found"
};

}
