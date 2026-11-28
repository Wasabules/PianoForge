// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MidiLibrary.h"

namespace pf::app {

juce::File MidiLibrary::folder()
{
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const juce::File candidates[] = {
        exeDir.getChildFile("midi"),                                // Windows: the installer's {app}\midi
        exeDir.getSiblingFile("share").getChildFile("pianoforge/midi"), // Linux: /usr/bin -> /usr/share/pianoforge/midi
        exeDir.getSiblingFile("Resources").getChildFile("midi"),    // macOS: Contents/MacOS -> Contents/Resources/midi
    };
    for (const auto& c : candidates)
        if (c.getChildFile("library.json").existsAsFile())
            return c;
    auto dir = exeDir; // a build folder: the repository's media/midi, a few levels up
    for (int i = 0; i < 10 && dir.exists(); ++i, dir = dir.getParentDirectory())
        if (const auto c = dir.getChildFile("media/midi"); c.getChildFile("library.json").existsAsFile())
            return c;
    return {};
}

const std::vector<LibraryPiece>& MidiLibrary::pieces()
{
    static const std::vector<LibraryPiece> list = [] {
        std::vector<LibraryPiece> out;
        const auto dir = folder();
        if (dir == juce::File())
            return out;
        const auto json = juce::JSON::parse(dir.getChildFile("library.json"));
        const auto* collections = json["collections"].getDynamicObject();
        if (const auto* items = json["pieces"].getArray())
            for (const auto& item : *items) {
                LibraryPiece p;
                p.file = dir.getChildFile(item["file"].toString());
                if (!p.file.existsAsFile())
                    continue;
                p.composer = item["composer"].toString();
                p.title = item["title"].toString();
                p.seconds = (int) item["seconds"];
                if (collections != nullptr) {
                    const auto collection = collections->getProperty(juce::Identifier(item["collection"].toString()));
                    p.credit = collection["credit"].toString();
                    p.license = collection["license"].toString();
                }
                out.push_back(std::move(p));
            }
        return out;
    }();
    return list;
}

} // namespace pf::app
