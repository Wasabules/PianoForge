// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PipeWireAudio.h"

#include "LinuxAudio.h"

#if PIANOFORGE_PIPEWIRE

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <dlfcn.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

namespace pf::app::pipewire {

namespace {

/// The functions of libpipewire used here, loaded at run time. (The headers' inline functions, the registry's and the
/// format builder's, call through the objects these return: nothing else of the library is needed.)
#define PF_PIPEWIRE_FUNCTIONS(X)                                                                                        \
    X(pw_init) X(pw_check_library_version) X(pw_get_library_version) X(pw_thread_loop_new) X(pw_thread_loop_destroy) \
    X(pw_thread_loop_start) X(pw_thread_loop_stop) X(pw_thread_loop_lock) X(pw_thread_loop_unlock)                    \
    X(pw_thread_loop_timed_wait) X(pw_thread_loop_signal) X(pw_thread_loop_get_loop) X(pw_context_new)                \
    X(pw_context_destroy) X(pw_context_connect) X(pw_core_disconnect) X(pw_properties_new) X(pw_properties_set)       \
    X(pw_proxy_destroy) X(pw_stream_new) X(pw_stream_destroy) X(pw_stream_add_listener) X(pw_stream_connect)          \
    X(pw_stream_dequeue_buffer) X(pw_stream_queue_buffer) X(pw_stream_set_active) X(pw_stream_update_properties)      \
    X(pw_stream_get_time_n)

struct Api {
#define PF_DECLARE(f) decltype(&::f) f = nullptr;
    PF_PIPEWIRE_FUNCTIONS(PF_DECLARE)
#undef PF_DECLARE
    bool usable = false;

    static const Api& get()
    {
        static const Api api = [] {
            Api a;
            void* lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
            if (lib == nullptr)
                return a;
            bool all = true;
#define PF_LOAD(f) all = all && (a.f = reinterpret_cast<decltype(a.f)>(dlsym(lib, #f))) != nullptr;
            PF_PIPEWIRE_FUNCTIONS(PF_LOAD)
#undef PF_LOAD
            if (!all)
                return a;
            a.pw_init(nullptr, nullptr);
            // 0.3.64: target.object (a sink by name), pw_buffer::requested, node.force-quantum honoured.
            a.usable = a.pw_check_library_version(0, 3, 64);
            return a;
        }();
        return api;
    }
};

struct Sink {
    std::string name, description; // node.name (the target), node.description (shown)
    bool bluetooth = false;         // device.api bluez5: 100 to 250 ms of its own
};

/// One connection to PipeWire's daemon (a thread of its own for its events), shared by the type and its devices: the
/// type keeps the list of sinks from it, the devices make their streams on it.
class Connection {
public:
    const Api& api;
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    std::atomic<bool> broken { false };
    std::function<void()> sinksChanged; // on PipeWire's thread, once the first listing is in

    explicit Connection(const Api& a) : api(a) {}
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ~Connection()
    {
        if (loop == nullptr)
            return;
        api.pw_thread_loop_lock(loop);
        if (defaults != nullptr) {
            spa_hook_remove(&defaultsHook);
            api.pw_proxy_destroy(reinterpret_cast<pw_proxy*>(defaults));
        }
        if (registry != nullptr) {
            spa_hook_remove(&registryHook);
            api.pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        }
        if (core != nullptr) {
            spa_hook_remove(&coreHook);
            api.pw_core_disconnect(core);
        }
        api.pw_thread_loop_unlock(loop);
        api.pw_thread_loop_stop(loop);
        if (context != nullptr)
            api.pw_context_destroy(context);
        api.pw_thread_loop_destroy(loop);
    }

    /// Connected and listed once, or nullptr (no daemon).
    static std::shared_ptr<Connection> open()
    {
        const Api& api = Api::get();
        if (!api.usable)
            return nullptr;
        auto c = std::make_shared<Connection>(api);
        if ((c->loop = api.pw_thread_loop_new("pianoforge-pipewire", nullptr)) == nullptr)
            return nullptr;
        if ((c->context = api.pw_context_new(api.pw_thread_loop_get_loop(c->loop), nullptr, 0)) == nullptr
            || api.pw_thread_loop_start(c->loop) < 0)
            return nullptr;
        Lock lock(*c);
        if ((c->core = api.pw_context_connect(c->context, nullptr, 0)) == nullptr)
            return nullptr;
        static const pw_core_events coreEvents = [] {
            pw_core_events e {};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = &Connection::onDone;
            e.error = &Connection::onError;
            return e;
        }();
        static const pw_registry_events registryEvents = [] {
            pw_registry_events e {};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = &Connection::onGlobal;
            e.global_remove = &Connection::onGlobalRemove;
            return e;
        }();
        pw_core_add_listener(c->core, &c->coreHook, &coreEvents, c.get());
        c->registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(c->registry, &c->registryHook, &registryEvents, c.get());
        if (!c->roundTrip() || !c->roundTrip()) // (the second: what the objects bound in the first said, the default sink)
            return nullptr;
        c->listed = true;
        current() = c;
        return c;
    }

    /// The connection last opened (the type's): for what the application asks of the outputs.
    static std::weak_ptr<Connection>& current()
    {
        static std::weak_ptr<Connection> latest;
        return latest;
    }

    /// The loop's lock, for any call into PipeWire from outside its thread.
    struct Lock {
        Connection& c;
        explicit Lock(Connection& conn) : c(conn) { c.api.pw_thread_loop_lock(c.loop); }
        ~Lock() { c.api.pw_thread_loop_unlock(c.loop); }
    };

    /// Under the lock: waits for `done` (true) or `failed` (false), at most `seconds`.
    template <typename Done, typename Failed>
    bool waitFor(Done done, Failed failed, int seconds)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + 1000.0 * seconds;
        while (!done()) {
            if (failed() || broken.load() || juce::Time::getMillisecondCounterHiRes() > until)
                return false;
            api.pw_thread_loop_timed_wait(loop, 1);
        }
        return true;
    }

    /// Under the lock: everything the daemon had to say so far has been said.
    bool roundTrip()
    {
        const int seq = pw_core_sync(core, PW_ID_CORE, 0);
        return waitFor([&] { return doneSeq.load() >= seq; }, [] { return false; }, 3);
    }

    std::map<std::uint32_t, Sink> sinks() const
    {
        const std::lock_guard<std::mutex> guard(sinksLock);
        return sinkList;
    }

    /// The system's default output (node.name), as PipeWire's "default" metadata says.
    std::string defaultSink() const
    {
        const std::lock_guard<std::mutex> guard(sinksLock);
        return defaultSinkName;
    }

private:
    spa_hook coreHook {}, registryHook {};
    std::atomic<int> doneSeq { -1 };
    bool listed = false;
    mutable std::mutex sinksLock;
    std::map<std::uint32_t, Sink> sinkList; // by global id
    std::string defaultSinkName;
    pw_metadata* defaults = nullptr; // PipeWire's "default" metadata: the default sink
    spa_hook defaultsHook {};

    static int onMetadata(void* data, std::uint32_t, const char* key, const char*, const char* value)
    {
        // default.audio.sink = {"name":"alsa_output..."}
        if (key == nullptr || std::strcmp(key, "default.audio.sink") != 0)
            return 0;
        auto& c = *static_cast<Connection*>(data);
        std::string name;
        if (value != nullptr) {
            const std::string text = value;
            if (const auto at = text.find("\"name\""); at != std::string::npos)
                if (const auto open = text.find('"', text.find(':', at) + 1); open != std::string::npos)
                    name = text.substr(open + 1, text.find('"', open + 1) - open - 1);
        }
        {
            const std::lock_guard<std::mutex> guard(c.sinksLock);
            c.defaultSinkName = name;
        }
        if (c.listed && c.sinksChanged)
            c.sinksChanged();
        return 0;
    }

    static void onDone(void* data, std::uint32_t id, int seq)
    {
        auto& c = *static_cast<Connection*>(data);
        if (id == PW_ID_CORE) {
            c.doneSeq = seq;
            c.api.pw_thread_loop_signal(c.loop, false);
        }
    }

    static void onError(void* data, std::uint32_t id, int, int res, const char*)
    {
        auto& c = *static_cast<Connection*>(data);
        if (id == PW_ID_CORE && res == -EPIPE) { // the daemon went away
            c.broken = true;
            c.api.pw_thread_loop_signal(c.loop, false);
            if (c.sinksChanged)
                c.sinksChanged();
        }
    }

    static void onGlobal(void* data, std::uint32_t id, std::uint32_t, const char* type, std::uint32_t, const spa_dict* props)
    {
        auto& c = *static_cast<Connection*>(data);
        if (props != nullptr && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && c.defaults == nullptr) {
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name != nullptr && std::strcmp(name, "default") == 0) {
                static const pw_metadata_events events = [] {
                    pw_metadata_events e {};
                    e.version = PW_VERSION_METADATA_EVENTS;
                    e.property = &Connection::onMetadata;
                    return e;
                }();
                c.defaults = static_cast<pw_metadata*>(pw_registry_bind(c.registry, id, type, PW_VERSION_METADATA, 0));
                if (c.defaults != nullptr)
                    pw_metadata_add_listener(c.defaults, &c.defaultsHook, &events, &c);
            }
            return;
        }
        if (props == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
            return;
        const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (mediaClass == nullptr || std::strcmp(mediaClass, "Audio/Sink") != 0)
            return;
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (name == nullptr)
            return;
        const char* api = spa_dict_lookup(props, "device.api");
        const bool bluetooth = (api != nullptr && std::strcmp(api, "bluez5") == 0) || std::strncmp(name, "bluez_", 6) == 0;
        {
            const std::lock_guard<std::mutex> guard(c.sinksLock);
            c.sinkList[id] = { name, description != nullptr ? description : name, bluetooth };
        }
        if (c.listed && c.sinksChanged)
            c.sinksChanged();
    }

    static void onGlobalRemove(void* data, std::uint32_t id)
    {
        auto& c = *static_cast<Connection*>(data);
        bool removed = false;
        {
            const std::lock_guard<std::mutex> guard(c.sinksLock);
            removed = c.sinkList.erase(id) > 0;
        }
        if (removed && c.listed && c.sinksChanged)
            c.sinksChanged();
    }
};

/// The sample rates offered: those PipeWire's graph may run at (clock.allowed-rates), at most 192 kHz.
juce::Array<double> sampleRates()
{
    const auto clock = linuxaudio::pipewireClock();
    juce::Array<double> rates;
    for (const double r : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        if (clock.allowedRates.empty() || std::find(clock.allowedRates.begin(), clock.allowedRates.end(), (int) r) != clock.allowedRates.end())
            rates.add(r);
    if (clock.rate > 0 && !rates.contains((double) clock.rate))
        rates.addUsingDefaultSort((double) clock.rate);
    return rates;
}

class Device final : public juce::AudioIODevice {
public:
    Device(std::shared_ptr<Connection> c, const juce::String& name, std::string target, bool main)
        : juce::AudioIODevice(name, kTypeName), connection(std::move(c)), api(connection->api), targetNode(std::move(target)),
          mainOutput(main), rates(sampleRates())
    {
    }

    ~Device() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return rates; }
    juce::Array<int> getAvailableBufferSizes() override { return { 32, 64, 128, 256, 512, 1024, 2048 }; }
    int getDefaultBufferSize() override { return 128; }

    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double sampleRate, int bufferSizeSamples) override
    {
        close();
        if (connection->broken.load())
            return lastError = "PipeWire's daemon is gone";
        rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        block = bufferSizeSamples > 0 ? std::clamp(bufferSizeSamples, 8, 8192) : getDefaultBufferSize();
        forced = mainOutput && forceQuantum().load();
        xruns = 0;
        lastXrun = -1;
        graphBlock = 0;
        graphRate = 0;
        delayNs = 0;
        state = PW_STREAM_STATE_UNCONNECTED;
        streamError.clear();

        Connection::Lock lock(*connection);
        const auto rateText = std::to_string(juce::roundToInt(rate));
        const auto latency = std::to_string(block) + "/" + rateText;
        pw_properties* props = api.pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                                     PW_KEY_MEDIA_ROLE, "Production", nullptr);
        api.pw_properties_set(props, PW_KEY_APP_NAME, "PianoForge");
        api.pw_properties_set(props, PW_KEY_NODE_NAME, mainOutput ? "PianoForge" : "PianoForge mirror");
        api.pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, mainOutput ? "PianoForge" : "PianoForge (mirror)");
        api.pw_properties_set(props, PW_KEY_MEDIA_NAME, mainOutput ? "PianoForge" : "PianoForge (mirror)");
        if (!targetNode.empty())
            api.pw_properties_set(props, PW_KEY_TARGET_OBJECT, targetNode.c_str());
        if ((stream = api.pw_stream_new(connection->core, "PianoForge", props)) == nullptr)
            return lastError = "PipeWire refused the stream";
        // Set once the stream is made: PipeWire applies the environment's PIPEWIRE_PROPS and PIPEWIRE_LATENCY after the
        // properties given to it, and they would prevail. 0: the graph's own quantum.
        const std::string rateFraction = "1/" + rateText, quantum = forced ? std::to_string(block) : "0";
        const spa_dict_item own[] = { { PW_KEY_NODE_LATENCY, latency.c_str() },
                                      { PW_KEY_NODE_RATE, rateFraction.c_str() },
                                      { "node.force-quantum", quantum.c_str() } };
        const spa_dict dict = SPA_DICT_INIT(own, 3);
        api.pw_stream_update_properties(stream, &dict);

        static const pw_stream_events events = [] {
            pw_stream_events e {};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = &Device::onStateChanged;
            e.io_changed = &Device::onIoChanged;
            e.process = &Device::onProcess;
            return e;
        }();
        api.pw_stream_add_listener(stream, &streamHook, &events, this);

        std::uint8_t podBuffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));
        spa_audio_info_raw info {};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.rate = (std::uint32_t) juce::roundToInt(rate);
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
                                                        | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE);
        if (api.pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
            destroyStream();
            return lastError = "PipeWire could not connect the stream";
        }
        // Connected and linked (paused until start()), or failed.
        const bool ready = connection->waitFor([&] { return state.load() == PW_STREAM_STATE_PAUSED || state.load() == PW_STREAM_STATE_STREAMING; },
                                               [&] { return state.load() == PW_STREAM_STATE_ERROR; }, 5);
        if (!ready) {
            lastError = state.load() == PW_STREAM_STATE_ERROR ? "PipeWire: " + juce::String(streamErrorText())
                        : connection->broken.load()             ? juce::String("PipeWire's daemon is gone")
                                                                : juce::String("PipeWire did not link the stream (no output?)");
            destroyStream();
            return lastError;
        }
        lastError.clear();
        return {};
    }

    void close() override
    {
        stop();
        if (stream != nullptr) {
            Connection::Lock lock(*connection);
            destroyStream();
        }
    }

    bool isOpen() override { return stream != nullptr; }

    void start(juce::AudioIODeviceCallback* newCallback) override
    {
        if (stream == nullptr || newCallback == nullptr)
            return;
        if (callback != nullptr && callback != newCallback)
            stop();
        newCallback->audioDeviceAboutToStart(this);
        {
            const juce::SpinLock::ScopedLockType guard(callbackLock);
            callback = newCallback;
        }
        Connection::Lock lock(*connection);
        api.pw_stream_set_active(stream, true);
        playing = true;
    }

    void stop() override
    {
        if (stream != nullptr && playing) {
            Connection::Lock lock(*connection);
            api.pw_stream_set_active(stream, false);
        }
        playing = false;
        juce::AudioIODeviceCallback* old = nullptr;
        {
            const juce::SpinLock::ScopedLockType guard(callbackLock); // waits for a cycle under way
            old = std::exchange(callback, nullptr);
        }
        if (old != nullptr)
            old->audioDeviceStopped();
    }

    bool isPlaying() override { return playing && callback != nullptr; }
    juce::String getLastError() override { return lastError; }
    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger(3); }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getInputLatencyInSamples() override { return 0; }
    int getOutputLatencyInSamples() override { return juce::roundToInt(1.0e-9 * (double) delayNs.load() * rate); }
    int getXRunCount() const noexcept override { return (int) xruns.load(); }

    const std::string& target() const noexcept { return targetNode; }

    Status status() const
    {
        Status s;
        s.block = block;
        s.graphBlock = (int) graphBlock.load();
        s.graphRate = (double) graphRate.load();
        s.latencyMs = 1.0e-6 * (double) delayNs.load();
        s.forced = forced;
        s.xruns = xruns.load();
        return s;
    }

private:
    std::shared_ptr<Connection> connection;
    const Api& api;
    std::string targetNode; // node.name of the sink, empty: the default one
    bool mainOutput = true;
    juce::Array<double> rates; // offered (the graph's allowed rates, read once)
    pw_stream* stream = nullptr;
    spa_hook streamHook {};
    std::atomic<spa_io_position*> position { nullptr }; // the graph's clock (read on the data thread)
    std::atomic<int> state { PW_STREAM_STATE_UNCONNECTED };
    mutable std::mutex errorLock;
    std::string streamError;
    double rate = 48000.0;
    int block = 128;
    bool forced = false;
    bool playing = false;
    juce::String lastError;
    juce::SpinLock callbackLock;
    juce::AudioIODeviceCallback* callback = nullptr;
    std::atomic<std::uint32_t> graphBlock { 0 }, graphRate { 0 }, xruns { 0 };
    std::atomic<std::int64_t> delayNs { 0 };
    std::int64_t lastXrun = -1; // the graph's accumulated xrun time when last seen (data thread; -1: not yet)

    std::string streamErrorText() const
    {
        const std::lock_guard<std::mutex> guard(errorLock);
        return streamError;
    }

    void destroyStream() // under the lock
    {
        if (stream == nullptr)
            return;
        spa_hook_remove(&streamHook);
        api.pw_stream_destroy(stream);
        stream = nullptr;
        position.store(nullptr);
    }

    static void onStateChanged(void* data, pw_stream_state, pw_stream_state now, const char* error)
    {
        auto& d = *static_cast<Device*>(data);
        if (error != nullptr) {
            const std::lock_guard<std::mutex> guard(d.errorLock);
            d.streamError = error;
        }
        d.state = now;
        d.api.pw_thread_loop_signal(d.connection->loop, false);
    }

    static void onIoChanged(void* data, std::uint32_t id, void* area, std::uint32_t)
    {
        if (id == SPA_IO_Position)
            static_cast<Device*>(data)->position.store(static_cast<spa_io_position*>(area));
    }

    /// PipeWire's realtime thread, once per graph cycle: the frames the graph asks for, the callback's blocks of at
    /// most `block` (more only if the graph does not run at it).
    static void onProcess(void* data)
    {
        auto& d = *static_cast<Device*>(data);
        pw_buffer* b = d.api.pw_stream_dequeue_buffer(d.stream);
        if (b == nullptr)
            return;
        spa_buffer* buffer = b->buffer;
        const std::uint32_t channels = std::min<std::uint32_t>(buffer->n_datas, 2);
        float* out[2] = { nullptr, nullptr };
        std::uint64_t frames = b->requested > 0 ? b->requested : (std::uint64_t) d.block;
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[c] = static_cast<float*>(buffer->datas[c].data);
            if (out[c] == nullptr)
                frames = 0;
            else
                frames = std::min<std::uint64_t>(frames, buffer->datas[c].maxsize / sizeof(float));
        }
        const int n = (int) frames;
        {
            const juce::SpinLock::ScopedTryLockType guard(d.callbackLock);
            if (guard.isLocked() && d.callback != nullptr && n > 0) {
                for (int done = 0; done < n;) {
                    const int chunk = std::min(n - done, d.block);
                    float* const outs[2] = { out[0] + done, channels > 1 ? out[1] + done : nullptr };
                    d.callback->audioDeviceIOCallbackWithContext(nullptr, 0, outs, (int) channels, chunk, {});
                    done += chunk;
                }
            } else {
                for (std::uint32_t c = 0; c < channels; ++c)
                    if (out[c] != nullptr)
                        std::fill(out[c], out[c] + n, 0.0f);
            }
        }
        for (std::uint32_t c = 0; c < channels; ++c) {
            buffer->datas[c].chunk->offset = 0;
            buffer->datas[c].chunk->stride = sizeof(float);
            buffer->datas[c].chunk->size = (std::uint32_t) (frames * sizeof(float));
        }
        b->size = frames;
        d.api.pw_stream_queue_buffer(d.stream, b);

        // How late what was just written will sound: the way to the device (PipeWire's measure, filters included) and
        // what a rate converter holds. (The block just queued is taken in this very cycle: not a wait.) PipeWire's
        // real time call: no lock, no allocation.
        pw_time time {};
        if (d.api.pw_stream_get_time_n(d.stream, &time, sizeof(time)) == 0 && time.rate.denom != 0) {
            const double tick = (double) time.rate.num / (double) time.rate.denom; // seconds
            const double seconds = (double) time.delay * tick + (double) time.buffered / d.rate;
            d.delayNs.store((std::int64_t) (1.0e9 * seconds), std::memory_order_relaxed);
        }
        if (const spa_io_position* p = d.position.load(std::memory_order_acquire)) { // the graph: quantum, rate, xruns
            const auto& clock = p->clock;
            d.graphBlock.store((std::uint32_t) clock.duration, std::memory_order_relaxed);
            if (clock.rate.num != 0)
                d.graphRate.store(clock.rate.denom / clock.rate.num, std::memory_order_relaxed);
            const auto xrun = (std::int64_t) clock.xrun; // their accumulated duration: it grows by one
            if (d.lastXrun >= 0 && xrun > d.lastXrun)
                d.xruns.fetch_add(1, std::memory_order_relaxed);
            d.lastXrun = xrun;
        }
    }
};

class Type final : public juce::AudioIODeviceType {
public:
    Type(std::shared_ptr<Connection> c, bool main) : juce::AudioIODeviceType(kTypeName), connection(std::move(c)), mainOutput(main)
    {
        watch();
    }

    ~Type() override
    {
        if (connection != nullptr) {
            Connection::Lock lock(*connection);
            connection->sinksChanged = nullptr;
        }
        masterReference.clear();
    }

    void scanForDevices() override
    {
        if (connection == nullptr || connection->broken.load()) { // the daemon came back (or never was): again
            if (auto again = Connection::open()) {
                connection = std::move(again);
                watch();
            }
        }
        names.clear();
        targets.clear();
        // No daemon (it restarts, or stopped): no output at all, so that JUCE closes the one playing (its stream is
        // gone) and the application plays elsewhere meanwhile, taking it back once the daemon answers again.
        if (connection == nullptr || connection->broken.load())
            return;
        names.add(kDefaultOutput);
        targets.push_back({});
        for (const auto& [id, sink] : connection->sinks()) {
            const auto shown = juce::String::fromUTF8(sink.description.c_str()) + (sink.bluetooth ? " (Bluetooth)" : "");
            juce::String name = shown;
            for (int k = 2; names.contains(name); ++k) // two sinks with the same description
                name = shown + " (" + juce::String(k) + ")";
            names.add(name);
            targets.push_back(sink.name);
        }
    }

    juce::StringArray getDeviceNames(bool wantInputNames) const override { return wantInputNames ? juce::StringArray() : names; }
    int getDefaultDeviceIndex(bool forInput) const override { return forInput ? -1 : 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        return asInput || device == nullptr ? -1 : names.indexOf(device->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName, const juce::String&) override
    {
        if (connection == nullptr || connection->broken.load())
            scanForDevices();
        if (connection == nullptr || connection->broken.load())
            return nullptr;
        if (names.isEmpty())
            scanForDevices();
        const int index = outputDeviceName.isEmpty() ? 0 : names.indexOf(outputDeviceName);
        if (index < 0 || index >= names.size() || (std::size_t) index >= targets.size())
            return nullptr;
        return new Device(connection, names[index], targets[(std::size_t) index], mainOutput);
    }

private:
    std::shared_ptr<Connection> connection;
    bool mainOutput = true;
    juce::StringArray names;
    std::vector<std::string> targets; // node.name per name, empty for the default
    JUCE_DECLARE_WEAK_REFERENCEABLE(Type)

    /// Sinks coming and going (a USB interface plugged, the daemon gone): the listeners told, on the message thread.
    void watch()
    {
        if (connection == nullptr)
            return;
        juce::WeakReference<Type> self(this);
        Connection::Lock lock(*connection);
        connection->sinksChanged = [self] {
            juce::MessageManager::callAsync([self] {
                if (auto* t = self.get()) {
                    t->scanForDevices();
                    t->callDeviceChangeListeners();
                }
            });
        };
    }
};

} // namespace

std::atomic<bool>& forceQuantum()
{
    static std::atomic<bool> force { true };
    return force;
}

std::unique_ptr<juce::AudioIODeviceType> createDeviceType(bool mainOutput)
{
    if (!linuxaudio::pipewireRunning())
        return nullptr;
    auto connection = Connection::open();
    if (connection == nullptr)
        return nullptr;
    return std::make_unique<Type>(std::move(connection), mainOutput);
}

std::optional<Status> status(juce::AudioIODevice* device)
{
    if (auto* d = dynamic_cast<Device*>(device))
        return d->status();
    return std::nullopt;
}

Output output(juce::AudioIODevice* device)
{
    Output out;
    const auto connection = Connection::current().lock();
    if (connection == nullptr || connection->broken.load())
        return out;
    auto* own = dynamic_cast<Device*>(device);
    const std::string target = own != nullptr && !own->target().empty() ? own->target() : connection->defaultSink();
    for (const auto& [id, sink] : connection->sinks())
        if (sink.name == target) {
            out.known = true;
            out.name = sink.name;
            out.description = sink.description;
            out.bluetooth = sink.bluetooth;
        }
    return out;
}

} // namespace pf::app::pipewire

#else // no PipeWire (another system, or built without its headers)

namespace pf::app::pipewire {

std::atomic<bool>& forceQuantum()
{
    static std::atomic<bool> force { true };
    return force;
}
std::unique_ptr<juce::AudioIODeviceType> createDeviceType(bool) { return nullptr; }
std::optional<Status> status(juce::AudioIODevice*) { return std::nullopt; }
Output output(juce::AudioIODevice*) { return {}; }

} // namespace pf::app::pipewire

#endif
