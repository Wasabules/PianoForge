#pragma once

#include <array>

namespace pf::app {

/// The built-in rooms and listening positions, shared by the application (Room window, Engine menu) and the plugin.
struct RoomPreset {
    const char* name;
    float size, decay, brightness, width; // pf::Room::Settings' units
};

struct ListeningPosition {
    const char* name;
    float distance; // m from the piano
};

// size: 60 m3 at 0, 40 000 m3 at 1, geometric; decay s (T60 at low frequencies); brightness; width.
inline constexpr std::array<RoomPreset, 5> kRoomPresets { {
    { "Studio", 0.14f, 0.6f, 0.60f, 0.8f },       // 150 m3
    { "Chamber", 0.50f, 1.4f, 0.55f, 1.0f },      // 1 600 m3
    { "Concert hall", 0.85f, 2.2f, 0.50f, 1.0f }, // 15 000 m3
    { "Large hall", 0.93f, 3.0f, 0.45f, 1.0f },   // 25 000 m3
    { "Cathedral", 1.0f, 5.5f, 0.35f, 1.0f },     // 40 000 m3
} };

inline constexpr std::array<ListeningPosition, 5> kListeningPositions { {
    { "At the keyboard", 1.0f },
    { "On stage", 3.0f },
    { "Front row", 6.0f },
    { "Middle of the hall", 12.0f },
    { "Back of the hall", 20.0f },
} };

inline constexpr int kDefaultRoomPreset = 2; // Concert hall
inline constexpr int kDefaultPosition = 0;   // at the keyboard

} // namespace pf::app
