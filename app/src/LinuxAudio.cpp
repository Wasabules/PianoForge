#include "LinuxAudio.h"

#if defined(__linux__)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include <juce_core/juce_core.h>

#include <dlfcn.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace pf::app::linuxaudio {

namespace {

#if defined(__x86_64__)
constexpr const char* kTriplet = "x86_64-linux-gnu";
#elif defined(__aarch64__)
constexpr const char* kTriplet = "aarch64-linux-gnu";
#elif defined(__riscv) && __riscv_xlen == 64
constexpr const char* kTriplet = "riscv64-linux-gnu";
#elif defined(__arm__)
constexpr const char* kTriplet = "arm-linux-gnueabihf";
#elif defined(__i386__)
constexpr const char* kTriplet = "i386-linux-gnu";
#else
constexpr const char* kTriplet = "";
#endif

/// The priority asked for: under PipeWire's own data thread (88) and a JACK server's (often 80 to 95), above
/// anything else of a desktop. rtkit grants at most its MaxRealtimePriority (20 by default).
constexpr int kPriority = 70;

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

/// sd-bus (libsystemd), loaded at run time: rtkit is a D-Bus service. Absent (a system without systemd): no rtkit.
struct SdBus {
    struct Error {
        const char* name = nullptr;
        const char* message = nullptr;
        int needFree = 0;
    };
    using Bus = void*;
    using Message = void*;
    int (*openSystem)(Bus*) = nullptr;
    int (*getPropertyTrivial)(Bus, const char*, const char*, const char*, const char*, Error*, char, void*) = nullptr;
    int (*callMethod)(Bus, const char*, const char*, const char*, const char*, Error*, Message*, const char*, ...) = nullptr;
    void (*errorFree)(Error*) = nullptr;
    Message (*messageUnref)(Message) = nullptr;
    Bus (*flushCloseUnref)(Bus) = nullptr;

    static const SdBus* get()
    {
        static const SdBus bus = [] {
            SdBus b;
            void* handle = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr)
                return b;
            auto load = [&](auto& f, const char* symbol) {
                f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(handle, symbol));
                return f != nullptr;
            };
            if (!(load(b.openSystem, "sd_bus_open_system") && load(b.getPropertyTrivial, "sd_bus_get_property_trivial")
                  && load(b.callMethod, "sd_bus_call_method") && load(b.errorFree, "sd_bus_error_free")
                  && load(b.messageUnref, "sd_bus_message_unref") && load(b.flushCloseUnref, "sd_bus_flush_close_unref")))
                b.openSystem = nullptr;
            return b;
        }();
        return bus.openSystem != nullptr ? &bus : nullptr;
    }
};

constexpr const char* kRtkit = "org.freedesktop.RealtimeKit1";
constexpr const char* kRtkitPath = "/org/freedesktop/RealtimeKit1";

/// Through rtkit: its limits read, RLIMIT_RTTIME brought under its maximum, then the thread made realtime.
Scheduling viaRtkit(std::int64_t tid)
{
    Scheduling s;
    const SdBus* sd = SdBus::get();
    if (sd == nullptr) {
        s.how = "no libsystemd, so no rtkit";
        return s;
    }
    SdBus::Bus bus = nullptr;
    if (sd->openSystem(&bus) < 0 || bus == nullptr) {
        s.how = "the system bus is not reachable, so no rtkit";
        return s;
    }
    SdBus::Error error;
    std::int32_t maxPriority = 0;
    std::int64_t maxRtTime = 0;
    if (sd->getPropertyTrivial(bus, kRtkit, kRtkitPath, kRtkit, "MaxRealtimePriority", &error, 'i', &maxPriority) < 0
        || sd->getPropertyTrivial(bus, kRtkit, kRtkitPath, kRtkit, "RTTimeUSecMax", &error, 'x', &maxRtTime) < 0) {
        s.how = "rtkit is not running" + (error.message != nullptr ? " (" + std::string(error.message) + ")" : std::string());
        sd->errorFree(&error);
        sd->flushCloseUnref(bus);
        return s;
    }
    rlimit rt {};
    if (getrlimit(RLIMIT_RTTIME, &rt) == 0 && maxRtTime > 0
        && (rt.rlim_max == RLIM_INFINITY || rt.rlim_max > (rlim_t) maxRtTime)) {
        rt.rlim_cur = rt.rlim_max = (rlim_t) maxRtTime;
        setrlimit(RLIMIT_RTTIME, &rt);
    }
    const int priority = std::clamp(kPriority, 1, std::max(1, (int) maxPriority));
    SdBus::Message reply = nullptr;
    const int r = sd->callMethod(bus, kRtkit, kRtkitPath, kRtkit, "MakeThreadRealtime", &error, &reply, "tu",
                                 (std::uint64_t) tid, (std::uint32_t) priority);
    if (r >= 0) {
        s.realtime = true;
        s.priority = priority;
        s.how = "realtime, priority " + std::to_string(priority) + " (rtkit)";
    } else {
        s.how = "rtkit refused" + (error.message != nullptr ? ": " + std::string(error.message) : std::string());
    }
    if (reply != nullptr)
        sd->messageUnref(reply);
    sd->errorFree(&error);
    sd->flushCloseUnref(bus);
    return s;
}

std::string& jackSetupText()
{
    static std::string text;
    return text;
}

/// The variables PianoForge sets only around a device's opening (ScopedProps): those the user had not set, made
/// empty at start (prepare()), so that later changes replace a value rather than add to the environment.
bool& owned(const char* variable)
{
    static bool alsa = false, props = false;
    return std::strcmp(variable, "PIPEWIRE_ALSA") == 0 ? alsa : props;
}

} // namespace

bool pipewireRunning()
{
    const std::string runtime = env("XDG_RUNTIME_DIR");
    const std::string remote = env("PIPEWIRE_REMOTE");
    return !runtime.empty() && access((runtime + "/" + (remote.empty() ? std::string("pipewire-0") : remote)).c_str(), F_OK) == 0;
}

std::string quantumProps(int block, double rate)
{
    const int samples = std::clamp(block, 8, 8192), hz = rate > 0.0 ? (int) std::lround(rate) : 48000;
    return "{ node.force-quantum = " + std::to_string(samples) + " node.latency = " + std::to_string(samples) + "/"
           + std::to_string(hz) + " }";
}

ScopedProps::ScopedProps(const char* v, const std::string& value) : variable(v)
{
    if (owned(variable))
        setenv(variable, value.c_str(), 1);
}

ScopedProps::~ScopedProps()
{
    if (owned(variable))
        setenv(variable, "", 1);
}

const std::string& jackSetup()
{
    return jackSetupText();
}

PipeWireClock pipewireClock()
{
    PipeWireClock c;
    c.running = pipewireRunning();
    if (!c.running)
        return c;
    juce::ChildProcess p;
    if (!p.start(juce::StringArray { "pw-metadata", "-n", "settings", "0" }, juce::ChildProcess::wantStdOut))
        return c;
    if (!p.waitForProcessToFinish(1000)) {
        p.kill();
        return c;
    }
    // Lines such as: update: id:0 key:'clock.min-quantum' value:'1024' type:''
    for (const auto& line : juce::StringArray::fromLines(p.readAllProcessOutput())) {
        const auto key = line.fromFirstOccurrenceOf("key:'", false, false).upToFirstOccurrenceOf("'", false, false);
        const auto text = line.fromFirstOccurrenceOf("value:'", false, false).upToFirstOccurrenceOf("'", false, false);
        const int value = text.getIntValue();
        if (key == "clock.rate")
            c.rate = value;
        else if (key == "clock.min-quantum")
            c.minQuantum = value;
        else if (key == "clock.max-quantum")
            c.maxQuantum = value;
        else if (key == "clock.force-quantum")
            c.forceQuantum = value;
        else if (key == "clock.force-rate")
            c.forceRate = value;
        else if (key == "clock.allowed-rates") // [ 44100, 48000, ... ]
            for (const auto& r : juce::StringArray::fromTokens(text.removeCharacters("[]"), ", ", ""))
                if (r.getIntValue() > 0)
                    c.allowedRates.push_back(r.getIntValue());
    }
    return c;
}

std::string prepare()
{
    // The variables set around a device's opening, unless the user set them: theirs prevail, untouched.
    for (const char* variable : { "PIPEWIRE_ALSA", "PIPEWIRE_PROPS" })
        if (std::getenv(variable) == nullptr) {
            setenv(variable, "", 1);
            owned(variable) = true;
        }
    auto& done = jackSetupText();
    done.clear();
    const std::string chosen = env("PIANOFORGE_JACK_LIBRARY");
    std::string library = chosen;
    if (chosen.empty() && pipewireRunning() && env("LD_LIBRARY_PATH").find("pipewire-0.3/jack") == std::string::npos) { // (pw-jack)
        std::vector<std::string> candidates; // Debian and Ubuntu (multiarch), then Fedora's and Arch's folders
        if (kTriplet[0] != 0)
            candidates.push_back(std::string("/usr/lib/") + kTriplet + "/pipewire-0.3/jack/libjack.so.0");
        candidates.push_back("/usr/lib64/pipewire-0.3/jack/libjack.so.0");
        candidates.push_back("/usr/lib/pipewire-0.3/jack/libjack.so.0");
        for (const auto& path : candidates)
            if (access(path.c_str(), R_OK) == 0) {
                library = path;
                break;
            }
    }
    // Loaded globally, it answers JUCE's dlopen("libjack.so.0") later: the dynamic loader matches loaded libraries by
    // their soname.
    if (!library.empty() && dlopen(library.c_str(), RTLD_LAZY | RTLD_GLOBAL) != nullptr)
        done = chosen.empty() ? "PipeWire's JACK" : "JACK from " + chosen;
    return done;
}

bool pipewireJack()
{
    return jackSetupText().rfind("PipeWire's JACK", 0) == 0;
}

std::int64_t threadId() noexcept
{
    thread_local const std::int64_t id = (std::int64_t) syscall(SYS_gettid);
    return id;
}

Scheduling scheduling(std::int64_t tid)
{
    Scheduling s;
    const int policy = sched_getscheduler((pid_t) tid);
    sched_param p {};
    if (policy < 0 || sched_getparam((pid_t) tid, &p) != 0) {
        s.how = "unknown";
        return s;
    }
    const int base = policy & ~SCHED_RESET_ON_FORK;
    s.realtime = base == SCHED_FIFO || base == SCHED_RR;
    s.priority = p.sched_priority;
    s.how = s.realtime ? "realtime, priority " + std::to_string(p.sched_priority) : std::string("normal priority");
    return s;
}

Scheduling makeRealtime(std::int64_t tid)
{
    if (tid <= 0)
        return {};
    if (auto now = scheduling(tid); now.realtime) {
        now.how += " (its host's)";
        return now;
    }
    // The user's own limit (the audio group's, /etc/security/limits.d): no daemon needed.
    rlimit rt {};
    if (getrlimit(RLIMIT_RTPRIO, &rt) == 0 && rt.rlim_cur > 0) {
        sched_param p {};
        p.sched_priority = std::min(kPriority, (int) std::min<rlim_t>(rt.rlim_cur, 99));
        if (sched_setscheduler((pid_t) tid, SCHED_FIFO | SCHED_RESET_ON_FORK, &p) == 0) {
            Scheduling s;
            s.realtime = true;
            s.priority = p.sched_priority;
            s.how = "realtime, priority " + std::to_string(p.sched_priority) + " (the user's realtime limit)";
            return s;
        }
    }
    return viaRtkit(tid);
}

} // namespace pf::app::linuxaudio

#else

namespace pf::app::linuxaudio {

std::string prepare() { return {}; }
bool pipewireJack() { return false; }
bool pipewireRunning() { return false; }
std::string quantumProps(int, double) { return {}; }
ScopedProps::ScopedProps(const char* v, const std::string&) : variable(v) {}
ScopedProps::~ScopedProps() = default;
const std::string& jackSetup()
{
    static const std::string none;
    return none;
}
PipeWireClock pipewireClock() { return {}; }
std::int64_t threadId() noexcept { return 0; }
Scheduling scheduling(std::int64_t) { return {}; }
Scheduling makeRealtime(std::int64_t) { return {}; }

} // namespace pf::app::linuxaudio

#endif
