// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace pf {

/// A gesture on the piano's keys meant as a command, not as music: keys struck together that no piece asks for (a
/// cluster at an end of the keyboard), or one key struck twice quickly.
struct PianoGesture {
    enum class Kind : int { none = 0, chord = 1, doubleTap = 2 };
    Kind kind = Kind::none;
    std::vector<int> keys; // ascending, distinct; one for a double tap

    bool empty() const noexcept { return kind == Kind::none || keys.empty(); }
    bool operator==(const PianoGesture& o) const noexcept { return kind == o.kind && keys == o.keys; }

    /// "chord:21,22,23", "double:108", "" (none): how it is kept in the settings.
    std::string text() const
    {
        if (empty())
            return {};
        std::string out = kind == Kind::chord ? "chord:" : "double:";
        for (std::size_t i = 0; i < keys.size(); ++i)
            out += (i > 0 ? "," : "") + std::to_string(keys[i]);
        return out;
    }
    static PianoGesture parse(const std::string& s)
    {
        PianoGesture g;
        const auto colon = s.find(':');
        if (colon == std::string::npos)
            return g;
        const std::string kind = s.substr(0, colon);
        g.kind = kind == "chord" ? Kind::chord : kind == "double" ? Kind::doubleTap : Kind::none;
        std::size_t at = colon + 1;
        while (at < s.size()) {
            const auto end = std::min(s.find(',', at), s.size());
            const int key = std::atoi(s.substr(at, end - at).c_str());
            if (key >= 0 && key < 128)
                g.keys.push_back(key);
            at = end + 1;
        }
        std::sort(g.keys.begin(), g.keys.end());
        g.keys.erase(std::unique(g.keys.begin(), g.keys.end()), g.keys.end());
        const bool valid = (g.kind == Kind::chord && g.keys.size() >= 2 && g.keys.size() <= 5)
                        || (g.kind == Kind::doubleTap && g.keys.size() == 1);
        return valid ? g : PianoGesture {};
    }
    static std::string keyName(int key)
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return std::string(names[key % 12]) + std::to_string(key / 12 - 1);
    }
    /// "A0 + A#0 + B0 together", "C8 twice", "none".
    std::string describe() const
    {
        if (empty())
            return "none";
        if (kind == Kind::doubleTap)
            return keyName(keys[0]) + " twice";
        std::string out;
        for (std::size_t i = 0; i < keys.size(); ++i)
            out += (i > 0 ? " + " : "") + keyName(keys[i]);
        return out + " together";
    }
};

/// Recognizes the gestures given to actions, in the notes played (times in seconds, any clock that does not go
/// back): a chord gesture, when its keys, and only them, are struck within kChordSeconds and held together; a
/// double tap, when its key is struck, released and struck again within kDoubleSeconds, nothing else struck in
/// between. Its time is that of the gesture's first note (a recording stopped by it ends before). Learning: the
/// next gesture played (keys struck together, two to five, or one key twice) is taken, to be given to an action.
/// Not thread-safe: one thread feeds it (or a lock around it).
class PianoGestures {
public:
    static constexpr double kChordSeconds = 0.15, kDoubleSeconds = 0.35, kLearnSettleSeconds = 0.45;

    void set(int action, const PianoGesture& g)
    {
        if (action < 0)
            return;
        if ((int) actions.size() <= action)
            actions.resize((std::size_t) action + 1);
        actions[(std::size_t) action] = g;
    }
    PianoGesture get(int action) const
    {
        return action >= 0 && action < (int) actions.size() ? actions[(std::size_t) action] : PianoGesture {};
    }
    /// The action another one already has this gesture (-1: none).
    int owner(const PianoGesture& g, int except) const
    {
        for (int a = 0; a < (int) actions.size(); ++a)
            if (a != except && !g.empty() && actions[(std::size_t) a] == g)
                return a;
        return -1;
    }

    /// A key struck at `t`: the action whose gesture it completes (-1: none), and when its first note was.
    int noteOn(int key, double t, double& start)
    {
        if (key < 0 || key > 127)
            return -1;
        presses.push_back({ key, t });
        while (!presses.empty() && t - presses.front().t > 1.0)
            presses.pop_front();
        down[(std::size_t) key] = true;
        if (learningOn) {
            learnLast = t;
            return -1;
        }
        for (int a = 0; a < (int) actions.size(); ++a) {
            const auto& g = actions[(std::size_t) a];
            if (g.kind == PianoGesture::Kind::chord && matchesChord(g, key, t, start)) {
                presses.clear();
                return a;
            }
        }
        for (int a = 0; a < (int) actions.size(); ++a) {
            const auto& g = actions[(std::size_t) a];
            if (g.kind == PianoGesture::Kind::doubleTap && g.keys[0] == key && presses.size() >= 2) {
                const auto& before = presses[presses.size() - 2];
                if (before.key == key && t - before.t <= kDoubleSeconds && released[(std::size_t) key] > before.t) {
                    start = before.t;
                    presses.clear();
                    return a;
                }
            }
        }
        return -1;
    }
    void noteOff(int key, double t)
    {
        if (key < 0 || key > 127)
            return;
        down[(std::size_t) key] = false;
        released[(std::size_t) key] = t;
    }

    void learn()
    {
        learningOn = true;
        learnLast = -1.0;
        presses.clear();
    }
    void stopLearning() { learningOn = false; }
    bool learning() const noexcept { return learningOn; }
    /// While learning, once the keys have been still for kLearnSettleSeconds: the gesture played, if it is one
    /// (else learning goes on, waiting for one).
    std::optional<PianoGesture> takeLearned(double now)
    {
        if (!learningOn || learnLast < 0.0 || now - learnLast < kLearnSettleSeconds || presses.empty())
            return std::nullopt;
        PianoGesture g;
        const auto first = presses.front();
        bool sameKey = true;
        for (const auto& p : presses)
            sameKey = sameKey && p.key == first.key;
        if (presses.size() == 2 && sameKey && presses[1].t - first.t <= kDoubleSeconds) {
            g.kind = PianoGesture::Kind::doubleTap;
            g.keys = { first.key };
        } else {
            for (const auto& p : presses)
                if (p.t - first.t <= kChordSeconds)
                    g.keys.push_back(p.key);
            std::sort(g.keys.begin(), g.keys.end());
            g.keys.erase(std::unique(g.keys.begin(), g.keys.end()), g.keys.end());
            if (g.keys.size() >= 2 && g.keys.size() <= 5)
                g.kind = PianoGesture::Kind::chord;
            else
                g.keys.clear();
        }
        presses.clear();
        learnLast = -1.0;
        if (g.empty())
            return std::nullopt;
        learningOn = false;
        return g;
    }

private:
    struct Press {
        int key = 0;
        double t = 0.0;
    };

    bool matchesChord(const PianoGesture& g, int key, double t, double& start) const
    {
        if (!std::binary_search(g.keys.begin(), g.keys.end(), key))
            return false;
        for (const int k : g.keys)
            if (!down[(std::size_t) k])
                return false;
        // The keys struck within the window: the gesture's, every one of them, and nothing else.
        std::vector<int> struck;
        double first = t;
        for (const auto& p : presses)
            if (t - p.t <= kChordSeconds) {
                struck.push_back(p.key);
                first = std::min(first, p.t);
            }
        std::sort(struck.begin(), struck.end());
        struck.erase(std::unique(struck.begin(), struck.end()), struck.end());
        if (struck != g.keys)
            return false;
        start = first;
        return true;
    }

    std::vector<PianoGesture> actions;
    std::deque<Press> presses; // the last second's
    std::array<bool, 128> down {};
    std::array<double, 128> released {};
    bool learningOn = false;
    double learnLast = -1.0;
};

} // namespace pf
