// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <utility>
#include <vector>

namespace pf::app {

/// A sound preset that comes with the application. It sets the sound only (what the engine models, the voicing,
/// the touch, the tone, the reverb's levels, the room and the listening position): the tuning, the volume, the
/// curves and the engine chosen stay as they are. What it does not name among soundPresetKeys() goes back to its
/// default (the sound as measured).
struct FactoryPreset {
    const char* name;
    const char* description;
    std::vector<std::pair<const char*, juce::var>> values;
};

/// The settings a factory preset governs.
inline const juce::StringArray& soundPresetKeys()
{
    static const juce::StringArray keys { "modalBody", "modalResidual", "modalSympathetic", "modalHeldKeys", "modalPedalCatch",
                                          "modalReleaseNoise", "sympatheticDb", "unisonSpread", "restrikeAbsorption",
                                          "voicingBassDb", "voicingMiddleDb", "voicingTrebleDb", "voicingVelocity", "voicingCorner",
                                          "dynamicRange", "sustainLength", "damperRate", "attackSoftDb", "attackSoftMs", "lidOpen",
                                          "toneBassDb", "tonePresenceDb", "toneAirDb", "convolutionOn", "convolutionWetDb", "convolutionDryDb",
                                          "roomModel", "roomOn", "roomPreset", "roomPresetName", "roomPosition", "roomKeepLoudness",
                                          "roomLevel", "roomSize", "roomDecay", "roomBrightness", "roomWidth", "roomDistance" };
    return keys;
}

// The room's presets (RoomPresets.h): 1 Studio, 2 Chamber, 3 Concert hall, 4 Large hall, 5 Cathedral; the listening
// positions: 1 at the keyboard (1 m), 2 on stage (3 m), 3 front row (6 m), 4 middle of the hall (12 m), 5 back (20 m).
inline const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> presets {
        { "As measured", "The instrument as recorded, heard from the keyboard in a concert hall: every setting at its default.",
          { { "roomModel", 2 } } },
        { "Concert hall, front row", "The same piano from the sixth metre: the hall around it, the lid open toward you.",
          { { "roomModel", 2 }, { "roomPreset", 3 }, { "roomPosition", 3 }, { "roomDistance", 6.0 } } },
        { "Soft and round", "Needled hammers, a gentler attack and a little less presence: a warm, rounded tone in a chamber.",
          { { "roomModel", 2 }, { "roomPreset", 2 }, { "voicingBassDb", -4.0 }, { "voicingMiddleDb", -6.0 }, { "voicingTrebleDb", -9.0 },
            { "attackSoftDb", -6.0 }, { "attackSoftMs", 50.0 }, { "tonePresenceDb", -2.0 } } },
        { "Softened (bright banks)", "For a bank recorded bright or close (Salamander): the treble's hammers needled hard, the "
                                     "attack and the presence tamed.",
          { { "roomModel", 2 }, { "voicingBassDb", -6.0 }, { "voicingMiddleDb", -8.0 }, { "voicingTrebleDb", -10.0 },
            { "attackSoftDb", -4.0 }, { "tonePresenceDb", -1.5 } } },
        { "Bright and present", "Harder hammers, more presence and air, the dynamics a little closer: a piano that cuts through "
                                "a mix (pop, rock), in a studio.",
          { { "roomModel", 2 }, { "roomPreset", 1 }, { "voicingBassDb", 2.0 }, { "voicingMiddleDb", 3.0 }, { "voicingTrebleDb", 3.0 },
            { "tonePresenceDb", 2.0 }, { "toneAirDb", 2.0 }, { "dynamicRange", 0.8 }, { "sustainLength", 0.9 } } },
        { "Jazz club", "On the half stick, slightly soft hammers, a shorter sustain and quicker dampers, heard from the stage of "
                       "a small room.",
          { { "roomModel", 2 }, { "roomPreset", 2 }, { "roomPosition", 2 }, { "roomDistance", 3.0 }, { "lidOpen", 0.5 },
            { "voicingBassDb", -2.0 }, { "voicingMiddleDb", -3.0 }, { "voicingTrebleDb", -4.0 }, { "sustainLength", 0.85 },
            { "damperRate", 1.3 }, { "dynamicRange", 0.85 } } },
        { "Night practice", "Lid closed, soft hammers, the pianissimo brought close to the forte: even and quiet, for headphones "
                            "late at night.",
          { { "roomModel", 2 }, { "roomPreset", 1 }, { "lidOpen", 0.0 }, { "voicingBassDb", -3.0 }, { "voicingMiddleDb", -4.0 },
            { "voicingTrebleDb", -6.0 }, { "attackSoftDb", -4.0 }, { "dynamicRange", 0.6 } } },
        { "Stream", "Closer dynamics and a touch of presence and air, for listeners on small speakers (the stream compressor, "
                    "in the Tone section, goes further).",
          { { "roomModel", 2 }, { "roomPreset", 2 }, { "dynamicRange", 0.7 }, { "tonePresenceDb", 1.0 }, { "toneAirDb", 1.0 } } },
        { "Cathedral", "A vast stone space from the middle of the nave: a longer sustain and slower dampers to let it ring.",
          { { "roomModel", 2 }, { "roomPreset", 5 }, { "roomPosition", 4 }, { "roomDistance", 12.0 }, { "sustainLength", 1.15 },
            { "damperRate", 0.8 } } },
        { "Old upright", "Unisons out of tune, harder hammers, a short sustain and a closed lid: an old upright in a small room.",
          { { "roomModel", 2 }, { "roomPreset", 1 }, { "unisonSpread", 2.5 }, { "lidOpen", 0.0 }, { "voicingBassDb", 2.0 },
            { "voicingMiddleDb", 3.0 }, { "voicingTrebleDb", 2.0 }, { "sustainLength", 0.6 }, { "damperRate", 1.4 },
            { "toneBassDb", -2.0 }, { "tonePresenceDb", 2.0 } } },
    };
    return presets;
}

/// Writes a factory preset into the settings: what it names, and every other sound setting removed (its default).
inline void writeFactoryPreset(juce::PropertiesFile& settings, const FactoryPreset& preset)
{
    for (const auto& key : soundPresetKeys())
        settings.removeValue(key);
    for (const auto& [key, value] : preset.values)
        settings.setValue(key, value);
}

/// The values of `keys` as the settings hold them (a preset's snapshot, JSON).
inline juce::String presetSnapshot(juce::PropertiesFile& settings, const juce::StringArray& keys)
{
    auto* values = new juce::DynamicObject();
    for (const auto& key : keys)
        values->setProperty(key, settings.getValue(key));
    return juce::JSON::toString(juce::var(values), true);
}

/// Whether the settings have moved away from a snapshot (a value of it changed).
inline bool movedFrom(juce::PropertiesFile& settings, const juce::String& snapshot)
{
    const auto parsed = juce::JSON::parse(snapshot);
    if (auto* object = parsed.getDynamicObject())
        for (const auto& p : object->getProperties())
            if (settings.getValue(p.name.toString()) != p.value.toString())
                return true;
    return false;
}

} // namespace pf::app
