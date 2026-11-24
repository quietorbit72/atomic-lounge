// SPDX-License-Identifier: AGPL-3.0-only
#include "PresetStore.h"

namespace mfm1 {

juce::File PresetStore::oldFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Moosive FM-1").getChildFile ("presets");   // legacy name: pre-rename folder, read once
}

juce::File PresetStore::dataFolder (const juce::String& sub)
{
    const auto app = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    const auto now = app.getChildFile ("Atomic Lounge").getChildFile (sub);
    const auto before = app.getChildFile ("Vintage Chonk").getChildFile (sub);   // 1.0.0 / 1.1.0 folder
    return now.isDirectory() || ! before.isDirectory() ? now : before;
}

juce::File PresetStore::folder()
{
    auto d = dataFolder ("presets");
    d.createDirectory();
    migrate (oldFolder(), d);
    return d;
}

int PresetStore::migrate (const juce::File& from, const juce::File& to)
{
    // presets saved before the rename (the legacy name folder above): copied once into the new folder;
    // a preset already there (same file name) is never overwritten, and the old folder is left as it is
    // (a marker file records the copy, so a preset deleted later in the new folder does not come back)
    auto marker = to.getChildFile (".migrated-from-moosive");   // legacy name: marker of that one-time copy
    if (! from.isDirectory() || marker.exists()) return 0;
    int copied = 0;
    for (auto& f : from.findChildFiles (juce::File::findFiles, false, "*.mfp"))
    {
        auto dest = to.getChildFile (f.getFileName());
        if (! dest.exists() && f.getSize() <= 4096 && f.copyFileTo (dest)) copied++;
    }
    marker.replaceWithText (juce::String (copied) + " presets copied from " + from.getFullPathName() + "\n");
    return copied;
}

juce::String PresetStore::safeName (const juce::String& name)
{
    juce::String out;
    for (auto c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                        || c == ' ' || c == '_' || c == '.' || c == '-';
        out << (ok ? juce::String::charToString (c) : juce::String ("_"));
    }
    out = out.trim();
    if (out.isEmpty() || out == "." || out == "..") out = "UNTITLED";
    return out.substring (0, 64);
}

juce::var PresetStore::list()
{
    juce::Array<juce::var> a;
    auto files = folder().findChildFiles (juce::File::findFiles, false, "*.mfp");
    files.sort();
    for (auto& f : files)
    {
        if (f.getSize() > 4096) continue;
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb)) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", f.getFileNameWithoutExtension());
        o->setProperty ("b64", juce::Base64::toBase64 (mb.getData(), mb.getSize()));
        a.add (juce::var (o));
    }
    return juce::var (a);
}

juce::String PresetStore::save (const juce::String& name, const juce::String& b64)
{
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64 (mo, b64) || mo.getDataSize() == 0 || mo.getDataSize() > 4096)
        return "save failed: bad data";
    auto f = folder().getChildFile (safeName (name) + ".mfp");
    return f.replaceWithData (mo.getData(), mo.getDataSize()) ? "ok" : "save failed: cannot write " + f.getFullPathName();
}

juce::String PresetStore::remove (const juce::String& name)
{
    auto f = folder().getChildFile (safeName (name) + ".mfp");
    return f.existsAsFile() && f.deleteFile() ? "ok" : "not found";
}

}
