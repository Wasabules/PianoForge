// The engine's C interface for WebAssembly: one modal engine and its room, driven from the AudioWorklet
// (web/site/worklet.js), on the browser's audio thread. Everything here runs on that thread: no lock, no wait.
//
// The profile arrives as bytes (the page never sends it to a server: the user picks the file, the browser keeps
// it): pf_alloc() a buffer, copy them in, pf_load_profile(), pf_free(). MIDI events queue up with pf_event() and
// take effect at the start of the next pf_render(), which writes pf_left() / pf_right(). A MIDI file (pf_midi_*)
// plays to the sample with them, through the engine's SequencePlayer, the lab's own.
#include <pianoforge/midi_sequence.hpp>
#include <pianoforge/modal_engine.hpp>
#include <pianoforge/modal_profile.hpp>
#include <pianoforge/room.hpp>
#include <pianoforge/velocity_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kMaxFrames = 1024;  // a render quantum is 128 frames today; room for larger ones
constexpr int kMaxEvents = 4608;  // per render: the keyboards', and a release of everything by the file player

pf::ModalEngine* g_engine = nullptr;
pf::Room* g_room = nullptr;
pf::SequencePlayer* g_player = nullptr;
pf::VelocityCurve g_curve;        // MIDI keyboards only (the on-screen keyboard plays its velocity as is)
float g_left[kMaxFrames], g_right[kMaxFrames];
pf::Event g_events[kMaxEvents];
int g_eventCount = 0;
double g_rate = 48000.0;
std::string g_error;

} // namespace

extern "C" {

/// Creates the engine and the room at the context's rate. Returns 0.
int pf_init(int sampleRate)
{
    delete g_engine;
    delete g_room;
    delete g_player;
    g_rate = sampleRate > 0 ? sampleRate : 48000.0;
    g_engine = new pf::ModalEngine();
    g_room = new pf::Room();
    g_player = new pf::SequencePlayer();
    g_engine->prepare(g_rate, kMaxFrames);
    g_room->prepare(g_rate);
    g_eventCount = 0;
    return 0;
}

void* pf_alloc(int bytes)
{
    return bytes > 0 ? std::malloc((std::size_t) bytes) : nullptr;
}

void pf_free(void* p)
{
    std::free(p);
}

/// Parses a modal profile (the bytes of modal-profile.bin) and installs it. Returns 0, or 1 (pf_error() says why).
int pf_load_profile(const char* data, int size)
{
    if (g_engine == nullptr || data == nullptr || size <= 0) {
        g_error = "no engine or no data";
        return 1;
    }
    g_error.clear();
    auto profile = pf::parseModalProfile(std::vector<char>(data, data + size), "modal-profile.bin", g_error);
    if (profile == nullptr)
        return 1;
    g_engine->release(); // this thread is the audio thread: no other one to wait for
    g_engine->install(std::move(profile));
    g_engine->prepare(g_rate, kMaxFrames);
    return 0;
}

const char* pf_error()
{
    return g_error.c_str();
}

int pf_profile_notes()
{
    return g_engine != nullptr && g_engine->profile() != nullptr ? (int) g_engine->profile()->notes.size() : 0;
}

int pf_profile_layers()
{
    std::size_t layers = 0;
    if (g_engine != nullptr && g_engine->profile() != nullptr)
        for (const auto& n : g_engine->profile()->notes)
            layers = std::max(layers, n.layers.size());
    return (int) layers;
}

/// Queues a MIDI message for the next render; `curved`: from a MIDI keyboard, its note-on velocity goes through
/// the velocity curve.
void pf_event(int status, int data1, int data2, int curved)
{
    if (g_eventCount >= kMaxEvents)
        return;
    if (curved != 0 && (status & 0xF0) == 0x90 && data2 > 0)
        data2 = g_curve.map(data2);
    g_events[g_eventCount++] = { 0, (std::uint8_t) status, (std::uint8_t) (data1 & 0x7F), (std::uint8_t) (data2 & 0x7F) };
}

/// Renders `frames` (at most 1024) into pf_left() / pf_right(), the queued events first.
void pf_render(int frames)
{
    frames = std::clamp(frames, 0, kMaxFrames);
    std::fill(g_left, g_left + frames, 0.0f);
    std::fill(g_right, g_right + frames, 0.0f);
    if (g_engine == nullptr)
        return;
    float* outputs[2] = { g_left, g_right };
    // The keyboards' events (offset 0) first, then the file's, in order: ascending offsets.
    g_eventCount += g_player->render(g_events + g_eventCount, kMaxEvents - g_eventCount, frames, g_rate);
    g_engine->process(outputs, 2, frames, g_events, g_eventCount);
    g_eventCount = 0;
    g_room->process(g_left, g_right, frames);
}

float* pf_left()
{
    return g_left;
}

float* pf_right()
{
    return g_right;
}

void pf_all_sound_off()
{
    if (g_engine != nullptr)
        g_engine->allSoundOff();
}

// ---- Settings ----

void pf_set_volume_db(float db)
{
    if (g_engine != nullptr)
        g_engine->outputGain = std::pow(10.0f, std::clamp(db, -60.0f, 24.0f) / 20.0f);
}

/// 0 soundboard, 1 attack residual, 2 sympathetic resonance, 3 held keys resonate, 4 pedal catches releases,
/// 5 release noise.
void pf_set_option(int option, int on)
{
    if (g_engine == nullptr)
        return;
    const bool v = on != 0;
    switch (option) {
    case 0: g_engine->bodyEnabled = v; break;
    case 1: g_engine->residualEnabled = v; break;
    case 2: g_engine->sympatheticEnabled = v; break;
    case 3: g_engine->heldKeysResonate = v; break;
    case 4: g_engine->pedalCatch = v; break;
    case 5: g_engine->releaseNoiseEnabled = v; break;
    default: break;
    }
}

void pf_set_sympathetic_db(float db)
{
    if (g_engine != nullptr)
        g_engine->sympatheticGain = std::pow(10.0f, std::clamp(db, -40.0f, 12.0f) / 20.0f);
}

/// The strings of each unison detuned by this much against the measured (1; 0: perfect unison; up to 4).
void pf_set_unison(float spread)
{
    if (g_engine != nullptr)
        g_engine->unisonSpread = std::clamp(spread, 0.0f, 4.0f);
}

/// The reference A4, equal temperament.
void pf_set_tuning(float a4)
{
    if (g_engine == nullptr)
        return;
    g_engine->referenceA4 = std::clamp(a4, 400.0f, 480.0f);
    for (auto& c : g_engine->pitchClassCents)
        c = 0.0f;
    g_engine->tuningVersion.fetch_add(1);
}

/// The room and the listening position (pf::Room::Settings): levelDb trims the reflections against the physics.
void pf_set_room(int on, float levelDb, float size, float decaySeconds, float brightness, float distance, int keepLoudness, float width)
{
    if (g_room == nullptr)
        return;
    pf::Room::Settings s;
    s.enabled = on != 0;
    s.levelDb = levelDb;
    s.size = size;
    s.decaySeconds = decaySeconds;
    s.brightness = brightness;
    s.distance = distance;
    s.keepLoudness = keepLoudness != 0;
    s.width = width;
    g_room->applySettings(s);
}

/// The velocity curve of MIDI keyboards: the range they really send, and the output (0..1) at 5 points over it.
void pf_set_velocity_curve(int inputMin, int inputMax, float p0, float p1, float p2, float p3, float p4)
{
    pf::VelocityCurve::Settings s;
    s.inputMin = std::clamp(inputMin, 1, 126);
    s.inputMax = std::clamp(inputMax, s.inputMin + 1, 127);
    s.points = { p0, p1, p2, p3, p4 };
    g_curve.set(s);
}

// ---- The MIDI file player ----

/// Reads a Standard MIDI File from its bytes and makes it the one played (stopped, at its start). Returns 0, or 1
/// (pf_error() says why).
int pf_midi_load(const unsigned char* data, int size)
{
    if (g_player == nullptr || data == nullptr || size <= 0) {
        g_error = "no player or no data";
        return 1;
    }
    g_error.clear();
    auto sequence = pf::readMidiFile(data, (std::size_t) size, g_error);
    if (sequence == nullptr)
        return 1;
    g_player->load(std::move(sequence));
    return 0;
}

void pf_midi_close() { if (g_player != nullptr) g_player->load(nullptr); }
int pf_midi_notes() { return g_player != nullptr && g_player->sequence() ? g_player->sequence()->notes : 0; }
int pf_midi_tracks() { return g_player != nullptr && g_player->sequence() ? g_player->sequence()->tracks : 0; }
int pf_midi_parts() { return g_player != nullptr && g_player->sequence() ? (int) g_player->sequence()->parts.size() : 0; }

namespace {
const pf::MidiSequence::Part* part(int i)
{
    const auto s = g_player != nullptr ? g_player->sequence() : nullptr;
    return s != nullptr && i >= 0 && i < (int) s->parts.size() ? &s->parts[(std::size_t) i] : nullptr;
}
std::string g_text; // the last text handed to the page
} // namespace

/// A part's name (its track's), or "".
const char* pf_midi_part_name(int i)
{
    g_text = part(i) != nullptr ? part(i)->name : std::string();
    return g_text.c_str();
}
/// Its General MIDI instrument ("Drums" on channel 10), or "".
const char* pf_midi_part_instrument(int i)
{
    g_text = part(i) != nullptr ? part(i)->instrument() : std::string();
    return g_text.c_str();
}
int pf_midi_part_track(int i) { return part(i) != nullptr ? part(i)->track : 0; }
int pf_midi_part_channel(int i) { return part(i) != nullptr ? part(i)->channel : 0; }
int pf_midi_part_notes(int i) { return part(i) != nullptr ? part(i)->notes : 0; }
int pf_midi_part_enabled(int i) { return g_player != nullptr && g_player->partEnabled(i) ? 1 : 0; }
void pf_midi_set_part(int i, int on) { if (g_player != nullptr) g_player->setPartEnabled(i, on != 0); }
/// Semitones added to every note (clamped to +-48).
void pf_midi_transpose(int semitones) { if (g_player != nullptr) g_player->setTranspose(semitones); }
int pf_midi_transposition() { return g_player != nullptr ? g_player->getTranspose() : 0; }
double pf_midi_length() { return g_player != nullptr && g_player->sequence() ? g_player->sequence()->length : 0.0; }
void pf_midi_play() { if (g_player != nullptr) g_player->play(); }
void pf_midi_pause() { if (g_player != nullptr) g_player->pause(); }
void pf_midi_stop() { if (g_player != nullptr) g_player->stop(); }
void pf_midi_seek(double seconds) { if (g_player != nullptr) g_player->seek(seconds); }
void pf_midi_speed(double factor) { if (g_player != nullptr) g_player->setSpeed(factor); }
void pf_midi_loop(int on) { if (g_player != nullptr) g_player->setLoop(on != 0); }
double pf_midi_position() { return g_player != nullptr ? g_player->position() : 0.0; }
int pf_midi_playing() { return g_player != nullptr && g_player->wantsToPlay() ? 1 : 0; }
/// The file's notes sounding on a key (for the keyboard's display).
int pf_midi_sounding(int key) { return g_player != nullptr ? g_player->sounding(key) : 0; }

// ---- Information for the page ----

int pf_voices() { return g_engine != nullptr ? g_engine->activeVoices.load() : 0; }
int pf_resonators() { return g_engine != nullptr ? g_engine->activeModes.load() : 0; }
int pf_sustain() { return g_engine != nullptr && g_engine->sustainDown.load() ? 1 : 0; }
int pf_sostenuto() { return g_engine != nullptr && g_engine->sostenutoDown.load() ? 1 : 0; }
int pf_soft() { return g_engine != nullptr && g_engine->softPedalPosition.load() > 0.0f ? 1 : 0; }
int pf_last_note() { return g_engine != nullptr ? g_engine->lastNote.load() : -1; }
int pf_last_velocity() { return g_engine != nullptr ? g_engine->lastVelocity.load() : 0; }
int pf_last_layer() { return g_engine != nullptr ? g_engine->lastLayerVelocity.load() : 0; }

} // extern "C"
