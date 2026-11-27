// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DesktopBus.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pf::app::desktop {

#if defined(__linux__)

namespace {

// GLib and GIO, as much of them as is used here (their C interface has not changed since GLib 2.26, 2010). Opaque
// types are void; the structures given are GLib's own layout.
using gpointer = void*;
using GVariant = void;
using GDBusConnection = void;
using GDBusMethodInvocation = void;
using GDBusNodeInfo = void;
using GDBusInterfaceInfo = void;
using GMainContext = void;
using GMainLoop = void;
using GVariantBuilder = void;
struct GError {
    std::uint32_t domain;
    int code;
    char* message;
};
using MethodCall = void (*)(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*,
                            GDBusMethodInvocation*, gpointer);
using GetProperty = GVariant* (*) (GDBusConnection*, const char*, const char*, const char*, const char*, GError**, gpointer);
using SetProperty = int (*)(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, GError**, gpointer);
struct GDBusInterfaceVTable {
    MethodCall method_call;
    GetProperty get_property;
    SetProperty set_property;
    gpointer padding[8];
};
using NameCallback = void (*)(GDBusConnection*, const char*, gpointer);

constexpr int kSystemBus = 1, kSessionBus = 2;
constexpr int kDoNotQueue = 4; // G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE
constexpr int kTimeoutMs = 3000;

struct Gio {
    GDBusConnection* (*g_bus_get_sync)(int, void*, GError**) = nullptr;
    GVariant* (*g_dbus_connection_call_sync)(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*,
                                             const void*, int, int, void*, GError**) = nullptr;
    int (*g_dbus_connection_emit_signal)(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, GError**) = nullptr;
    unsigned (*g_dbus_connection_register_object)(GDBusConnection*, const char*, GDBusInterfaceInfo*, const GDBusInterfaceVTable*,
                                                  gpointer, void (*)(gpointer), GError**) = nullptr;
    int (*g_dbus_connection_unregister_object)(GDBusConnection*, unsigned) = nullptr;
    unsigned (*g_bus_own_name_on_connection)(GDBusConnection*, const char*, int, NameCallback, NameCallback, gpointer,
                                             void (*)(gpointer)) = nullptr;
    void (*g_bus_unown_name)(unsigned) = nullptr;
    GDBusNodeInfo* (*g_dbus_node_info_new_for_xml)(const char*, GError**) = nullptr;
    GDBusInterfaceInfo* (*g_dbus_node_info_lookup_interface)(GDBusNodeInfo*, const char*) = nullptr;
    void (*g_dbus_node_info_unref)(GDBusNodeInfo*) = nullptr;
    void (*g_dbus_method_invocation_return_value)(GDBusMethodInvocation*, GVariant*) = nullptr;
    void (*g_dbus_method_invocation_return_dbus_error)(GDBusMethodInvocation*, const char*, const char*) = nullptr;
    GVariant* (*g_variant_new)(const char*, ...) = nullptr;
    void (*g_variant_get)(GVariant*, const char*, ...) = nullptr;
    void (*g_variant_unref)(GVariant*) = nullptr;
    GVariantBuilder* (*g_variant_builder_new)(const void*) = nullptr;
    void (*g_variant_builder_add)(GVariantBuilder*, const char*, ...) = nullptr;
    GVariant* (*g_variant_builder_end)(GVariantBuilder*) = nullptr;
    void (*g_variant_builder_unref)(GVariantBuilder*) = nullptr;
    GMainContext* (*g_main_context_new)() = nullptr;
    void (*g_main_context_push_thread_default)(GMainContext*) = nullptr;
    void (*g_main_context_pop_thread_default)(GMainContext*) = nullptr;
    void (*g_main_context_unref)(GMainContext*) = nullptr;
    void (*g_main_context_invoke)(GMainContext*, int (*)(gpointer), gpointer) = nullptr;
    GMainLoop* (*g_main_loop_new)(GMainContext*, int) = nullptr;
    void (*g_main_loop_run)(GMainLoop*) = nullptr;
    void (*g_main_loop_quit)(GMainLoop*) = nullptr;
    void (*g_main_loop_unref)(GMainLoop*) = nullptr;
    void (*g_error_free)(GError*) = nullptr;
    void (*g_free)(gpointer) = nullptr;
    bool ok = false;

    static const Gio& get()
    {
        static const Gio gio = [] {
            Gio g;
            void* lib = dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL); // (dlsym finds GLib's own through it)
            if (lib == nullptr)
                return g;
            bool all = true;
            auto load = [&](auto& f, const char* name) {
                f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
                all = all && f != nullptr;
            };
#define PF_GIO(f) load(g.f, #f);
            PF_GIO(g_bus_get_sync) PF_GIO(g_dbus_connection_call_sync) PF_GIO(g_dbus_connection_emit_signal)
            PF_GIO(g_dbus_connection_register_object) PF_GIO(g_dbus_connection_unregister_object)
            PF_GIO(g_bus_own_name_on_connection) PF_GIO(g_bus_unown_name) PF_GIO(g_dbus_node_info_new_for_xml)
            PF_GIO(g_dbus_node_info_lookup_interface) PF_GIO(g_dbus_node_info_unref)
            PF_GIO(g_dbus_method_invocation_return_value) PF_GIO(g_dbus_method_invocation_return_dbus_error)
            PF_GIO(g_variant_new) PF_GIO(g_variant_get) PF_GIO(g_variant_unref) PF_GIO(g_variant_builder_new)
            PF_GIO(g_variant_builder_add) PF_GIO(g_variant_builder_end) PF_GIO(g_variant_builder_unref)
            PF_GIO(g_main_context_new) PF_GIO(g_main_context_push_thread_default) PF_GIO(g_main_context_pop_thread_default)
            PF_GIO(g_main_context_unref) PF_GIO(g_main_context_invoke) PF_GIO(g_main_loop_new) PF_GIO(g_main_loop_run)
            PF_GIO(g_main_loop_quit) PF_GIO(g_main_loop_unref) PF_GIO(g_error_free) PF_GIO(g_free)
#undef PF_GIO
            g.ok = all;
            return g;
        }();
        return gio;
    }
};

/// The session and system buses: GIO's shared connections, kept for the application's life (a profile held, or a
/// name owned, lasts as long as the connection).
GDBusConnection* bus(int which)
{
    static GDBusConnection* session = nullptr;
    static GDBusConnection* system = nullptr;
    static std::once_flag sessionOnce, systemOnce;
    const Gio& g = Gio::get();
    if (!g.ok)
        return nullptr;
    auto open = [&] {
        GError* error = nullptr;
        GDBusConnection* c = g.g_bus_get_sync(which, nullptr, &error);
        if (error != nullptr)
            g.g_error_free(error);
        return c;
    };
    if (which == kSessionBus) {
        std::call_once(sessionOnce, [&] { session = open(); });
        return session;
    }
    std::call_once(systemOnce, [&] { system = open(); });
    return system;
}

/// A method call, waited for (at most 3 s); its reply, or nullptr with `error` set. `parameters` is consumed.
GVariant* call(int which, const char* service, const char* path, const char* interface, const char* method, GVariant* parameters,
               std::string* error = nullptr)
{
    const Gio& g = Gio::get();
    GDBusConnection* c = bus(which);
    if (c == nullptr) {
        if (error != nullptr)
            *error = which == kSessionBus ? "no session bus" : "no system bus";
        return nullptr;
    }
    GError* e = nullptr;
    GVariant* reply = g.g_dbus_connection_call_sync(c, service, path, interface, method, parameters, nullptr, 0, kTimeoutMs, nullptr, &e);
    if (e != nullptr) {
        if (error != nullptr)
            *error = e->message != nullptr ? e->message : "failed";
        g.g_error_free(e);
    }
    return reply;
}

void drop(GVariant* v)
{
    if (v != nullptr)
        Gio::get().g_variant_unref(v);
}

constexpr const char* kAppId = "org.pianoforge.PianoForge";

} // namespace

bool available()
{
    return bus(kSessionBus) != nullptr;
}

// ---- KeepAwake ----

KeepAwake::KeepAwake() = default;

KeepAwake::~KeepAwake()
{
    set(false, {});
}

bool KeepAwake::set(bool hold, const std::string& reason)
{
    const Gio& g = Gio::get();
    if (hold == holding)
        return holding;
    if (!hold) {
        if (kind == 1)
            drop(call(kSessionBus, "org.freedesktop.portal.Desktop", handle.c_str(), "org.freedesktop.portal.Request", "Close", nullptr));
        else if (kind == 2)
            drop(call(kSessionBus, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", "Uninhibit",
                      g.g_variant_new("(u)", (std::uint32_t) std::stoul(handle))));
        else if (kind == 3)
            drop(call(kSessionBus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver",
                      "UnInhibit", g.g_variant_new("(u)", (std::uint32_t) std::stoul(handle))));
        holding = false;
        kind = 0;
        handle.clear();
        return false;
    }
    if (!g.ok) {
        method = "GIO is missing";
        return false;
    }
    std::string error;
    // The desktop portal: idle (8) and suspend (4).
    GVariantBuilder* options = g.g_variant_builder_new("a{sv}");
    g.g_variant_builder_add(options, "{sv}", "reason", g.g_variant_new("s", reason.c_str()));
    GVariant* reply = call(kSessionBus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Inhibit", "Inhibit", g.g_variant_new("(sua{sv})", "", 12u, options), &error);
    g.g_variant_builder_unref(options);
    if (reply != nullptr) {
        char* path = nullptr;
        g.g_variant_get(reply, "(o)", &path);
        handle = path != nullptr ? path : "";
        g.g_free(path);
        drop(reply);
        kind = 1;
        method = "desktop portal";
        return holding = true;
    }
    // GNOME's session manager: idle (8) and suspend (4).
    reply = call(kSessionBus, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", "Inhibit",
                 g.g_variant_new("(susu)", "PianoForge", 0u, reason.c_str(), 12u), &error);
    if (reply == nullptr) // the screen saver's interface (KDE, Xfce...): idle only
        reply = call(kSessionBus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver",
                     "Inhibit", g.g_variant_new("(ss)", "PianoForge", reason.c_str()), &error);
    else
        kind = 2;
    if (reply == nullptr) {
        method = "no desktop service answered (" + error + ")";
        kind = 0;
        return false;
    }
    if (kind == 0)
        kind = 3;
    std::uint32_t cookie = 0;
    g.g_variant_get(reply, "(u)", &cookie);
    drop(reply);
    handle = std::to_string(cookie);
    method = kind == 2 ? "GNOME's session manager" : "the screen saver's service";
    return holding = true;
}

// ---- PerformanceProfile ----

PerformanceProfile::~PerformanceProfile()
{
    set(false, {});
}

bool PerformanceProfile::set(bool hold, const std::string& reason)
{
    const Gio& g = Gio::get();
    if (hold == (cookie != 0))
        return hold;
    if (!hold) {
        drop(call(kSystemBus, service.c_str(), path.c_str(), interface.c_str(), "ReleaseProfile", g.g_variant_new("(u)", cookie)));
        cookie = 0;
        return false;
    }
    if (!g.ok) {
        method = "GIO is missing";
        return false;
    }
    // power-profiles-daemon, under its current name, then its first one.
    static const struct {
        const char *service, *path, *interface;
    } daemons[] = { { "org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles" },
                    { "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles" } };
    std::string error;
    for (const auto& d : daemons) {
        GVariant* reply = call(kSystemBus, d.service, d.path, d.interface, "HoldProfile",
                               g.g_variant_new("(sss)", "performance", reason.c_str(), kAppId), &error);
        if (reply == nullptr)
            continue;
        g.g_variant_get(reply, "(u)", &cookie);
        drop(reply);
        service = d.service;
        path = d.path;
        interface = d.interface;
        method = "power-profiles-daemon";
        return cookie != 0;
    }
    method = "no power profile service (" + error + ")";
    return false;
}

// ---- notify ----

bool notify(const std::string& summary, const std::string& body)
{
    const Gio& g = Gio::get();
    if (!g.ok)
        return false;
    GVariantBuilder* hints = g.g_variant_builder_new("a{sv}");
    g.g_variant_builder_add(hints, "{sv}", "desktop-entry", g.g_variant_new("s", kAppId));
    GVariant* reply = call(kSessionBus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g.g_variant_new("(susssasa{sv}i)", "PianoForge", 0u, kAppId, summary.c_str(), body.c_str(), nullptr, hints, -1));
    g.g_variant_builder_unref(hints);
    const bool shown = reply != nullptr;
    drop(reply);
    return shown;
}

// ---- MediaControls (MPRIS) ----

namespace {

constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";
constexpr const char* kRootInterface = "org.mpris.MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kTrackId = "/org/pianoforge/PianoForge/track/0";

constexpr const char* kIntrospection = R"(<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg direction="in" type="x" name="Offset"/></method>
    <method name="SetPosition"><arg direction="in" type="o" name="TrackId"/><arg direction="in" type="x" name="Position"/></method>
    <method name="OpenUri"><arg direction="in" type="s" name="Uri"/></method>
    <signal name="Seeked"><arg type="x" name="Position"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Rate" type="d" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="read"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>)";

std::string fileUri(const std::string& path)
{
    return path.empty() ? std::string() : juce::URL(juce::File(path)).toString(false).toStdString();
}

} // namespace

struct MediaControls::Impl {
    const Gio& g = Gio::get();
    Commands commands;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    GDBusConnection* connection = nullptr;
    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    GDBusNodeInfo* node = nullptr;
    unsigned rootId = 0, playerId = 0, nameId = 0;
    std::atomic<bool> registered { false };
    bool triedInstanceName = false;
    std::thread thread;
    std::mutex stateLock;
    PlayerState state, announced; // the latest (under the lock), and the one last announced (message thread)
    double announcedAt = 0.0;     // when `announced` was taken (ms)

    /// On the message thread, while the application is there.
    void post(std::function<void()> f)
    {
        juce::MessageManager::callAsync([a = alive, f = std::move(f)] {
            if (a->load())
                f();
        });
    }

    GVariant* stringArray(std::initializer_list<const char*> items)
    {
        GVariantBuilder* b = g.g_variant_builder_new("as");
        for (const char* s : items)
            g.g_variant_builder_add(b, "s", s);
        GVariant* v = g.g_variant_builder_end(b);
        g.g_variant_builder_unref(b);
        return v;
    }

    GVariant* metadata(const PlayerState& s)
    {
        GVariantBuilder* b = g.g_variant_builder_new("a{sv}");
        g.g_variant_builder_add(b, "{sv}", "mpris:trackid", g.g_variant_new("o", kTrackId));
        if (s.hasFile) {
            g.g_variant_builder_add(b, "{sv}", "xesam:title", g.g_variant_new("s", s.title.c_str()));
            g.g_variant_builder_add(b, "{sv}", "mpris:length", g.g_variant_new("x", (std::int64_t) s.lengthUs));
            g.g_variant_builder_add(b, "{sv}", "xesam:url", g.g_variant_new("s", fileUri(s.path).c_str()));
        }
        GVariant* v = g.g_variant_builder_end(b);
        g.g_variant_builder_unref(b);
        return v;
    }

    static const char* status(const PlayerState& s) { return s.playing ? "Playing" : s.hasFile ? "Paused" : "Stopped"; }

    GVariant* property(const std::string& interface, const std::string& name)
    {
        PlayerState s;
        {
            const std::lock_guard<std::mutex> guard(stateLock);
            s = state;
        }
        if (interface == kRootInterface) {
            if (name == "CanQuit" || name == "HasTrackList")
                return g.g_variant_new("b", 0);
            if (name == "CanRaise")
                return g.g_variant_new("b", 1);
            if (name == "Identity")
                return g.g_variant_new("s", "PianoForge");
            if (name == "DesktopEntry")
                return g.g_variant_new("s", kAppId);
            if (name == "SupportedUriSchemes")
                return stringArray({ "file" });
            if (name == "SupportedMimeTypes")
                return stringArray({ "audio/midi", "audio/x-midi" });
            return nullptr;
        }
        if (name == "PlaybackStatus")
            return g.g_variant_new("s", status(s));
        if (name == "Rate")
            return g.g_variant_new("d", s.rate);
        if (name == "Metadata")
            return metadata(s);
        if (name == "Volume")
            return g.g_variant_new("d", 1.0);
        if (name == "Position")
            return g.g_variant_new("x", (std::int64_t) s.positionUs);
        if (name == "MinimumRate")
            return g.g_variant_new("d", 0.25);
        if (name == "MaximumRate")
            return g.g_variant_new("d", 2.0);
        if (name == "CanGoNext")
            return g.g_variant_new("b", s.canNext ? 1 : 0);
        if (name == "CanGoPrevious")
            return g.g_variant_new("b", s.canPrevious ? 1 : 0);
        if (name == "CanPlay" || name == "CanPause" || name == "CanSeek")
            return g.g_variant_new("b", s.hasFile ? 1 : 0);
        if (name == "CanControl")
            return g.g_variant_new("b", 1);
        return nullptr;
    }

    static void onMethod(GDBusConnection*, const char*, const char*, const char* interface, const char* method, GVariant* parameters,
                         GDBusMethodInvocation* invocation, gpointer data)
    {
        auto& self = *static_cast<Impl*>(data);
        const std::string m = method;
        const auto& c = self.commands;
        auto run = [&](const std::function<void()>& f) {
            if (f)
                self.post(f);
        };
        if (std::strcmp(interface, kRootInterface) == 0) {
            if (m == "Raise")
                run(c.raise);
        } else if (m == "Play")
            run(c.play);
        else if (m == "Pause")
            run(c.pause);
        else if (m == "PlayPause")
            run(c.playPause);
        else if (m == "Stop")
            run(c.stop);
        else if (m == "Next")
            run(c.next);
        else if (m == "Previous")
            run(c.previous);
        else if (m == "Seek" && c.seekBy) {
            std::int64_t offset = 0;
            self.g.g_variant_get(parameters, "(x)", &offset);
            self.post([f = c.seekBy, offset] { f(offset); });
        } else if (m == "SetPosition" && c.seekTo) {
            char* track = nullptr;
            std::int64_t position = 0;
            self.g.g_variant_get(parameters, "(ox)", &track, &position);
            const bool ours = track != nullptr && std::strcmp(track, kTrackId) == 0;
            self.g.g_free(track);
            if (ours)
                self.post([f = c.seekTo, position] { f(position); });
        } else if (m == "OpenUri" && c.open) {
            char* uri = nullptr;
            self.g.g_variant_get(parameters, "(s)", &uri);
            const juce::String text = juce::String::fromUTF8(uri != nullptr ? uri : "");
            self.g.g_free(uri);
            const auto file = text.startsWith("file:") ? juce::URL(text).getLocalFile() : juce::File();
            if (file.existsAsFile())
                self.post([f = c.open, path = file.getFullPathName().toStdString()] { f(path); });
        }
        self.g.g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* onGet(GDBusConnection*, const char*, const char*, const char* interface, const char* name, GError**, gpointer data)
    {
        return static_cast<Impl*>(data)->property(interface, name);
    }

    static void onNameLost(GDBusConnection*, const char*, gpointer data)
    {
        // Another PianoForge has the name: this one takes its own (the controls still find it).
        auto& self = *static_cast<Impl*>(data);
        if (self.triedInstanceName || self.connection == nullptr)
            return;
        self.triedInstanceName = true;
        self.g.g_bus_unown_name(self.nameId);
        const auto name = "org.mpris.MediaPlayer2.PianoForge.instance" + std::to_string(getpid());
        self.nameId = self.g.g_bus_own_name_on_connection(self.connection, name.c_str(), kDoNotQueue, nullptr, nullptr, &self, nullptr);
    }

    /// On the GLib thread: the objects registered, the name asked for.
    static int start(gpointer data)
    {
        auto& self = *static_cast<Impl*>(data);
        static const GDBusInterfaceVTable vtable { &Impl::onMethod, &Impl::onGet, nullptr, {} };
        GError* error = nullptr;
        self.node = self.g.g_dbus_node_info_new_for_xml(kIntrospection, &error);
        if (error != nullptr) {
            self.g.g_error_free(error);
            return 0;
        }
        self.rootId = self.g.g_dbus_connection_register_object(self.connection, kMprisPath,
                                                               self.g.g_dbus_node_info_lookup_interface(self.node, kRootInterface),
                                                               &vtable, &self, nullptr, nullptr);
        self.playerId = self.g.g_dbus_connection_register_object(self.connection, kMprisPath,
                                                                 self.g.g_dbus_node_info_lookup_interface(self.node, kPlayerInterface),
                                                                 &vtable, &self, nullptr, nullptr);
        if (self.rootId == 0 || self.playerId == 0)
            return 0;
        self.nameId = self.g.g_bus_own_name_on_connection(self.connection, "org.mpris.MediaPlayer2.PianoForge", kDoNotQueue, nullptr,
                                                          &Impl::onNameLost, &self, nullptr);
        self.registered = true;
        return 0; // (G_SOURCE_REMOVE)
    }

    static int finish(gpointer data)
    {
        auto& self = *static_cast<Impl*>(data);
        if (self.nameId != 0)
            self.g.g_bus_unown_name(self.nameId);
        if (self.rootId != 0)
            self.g.g_dbus_connection_unregister_object(self.connection, self.rootId);
        if (self.playerId != 0)
            self.g.g_dbus_connection_unregister_object(self.connection, self.playerId);
        self.registered = false;
        self.g.g_main_loop_quit(self.loop);
        return 0;
    }

    void emit(const char* interface, const char* signal, GVariant* parameters)
    {
        GError* error = nullptr;
        g.g_dbus_connection_emit_signal(connection, nullptr, kMprisPath, interface, signal, parameters, &error);
        if (error != nullptr)
            g.g_error_free(error);
    }
};

MediaControls::MediaControls(Commands commands) : impl(std::make_unique<Impl>())
{
    auto& m = *impl;
    m.commands = std::move(commands);
    if (!m.g.ok || (m.connection = bus(kSessionBus)) == nullptr)
        return;
    m.context = m.g.g_main_context_new();
    m.loop = m.g.g_main_loop_new(m.context, 0);
    m.thread = std::thread([&m] {
        m.g.g_main_context_push_thread_default(m.context);
        m.g.g_main_context_invoke(m.context, &Impl::start, &m);
        m.g.g_main_loop_run(m.loop);
        m.g.g_main_context_pop_thread_default(m.context);
    });
}

MediaControls::~MediaControls()
{
    auto& m = *impl;
    m.alive->store(false);
    if (m.thread.joinable()) {
        m.g.g_main_context_invoke(m.context, &Impl::finish, &m);
        m.thread.join();
    }
    if (m.node != nullptr)
        m.g.g_dbus_node_info_unref(m.node);
    if (m.loop != nullptr)
        m.g.g_main_loop_unref(m.loop);
    if (m.context != nullptr)
        m.g.g_main_context_unref(m.context);
}

bool MediaControls::active() const noexcept
{
    return impl->registered.load();
}

void MediaControls::update(const PlayerState& s)
{
    auto& m = *impl;
    {
        const std::lock_guard<std::mutex> guard(m.stateLock);
        m.state = s;
    }
    if (!m.registered.load())
        return;
    const auto& a = m.announced;
    const auto& g = m.g;
    GVariantBuilder* changed = g.g_variant_builder_new("a{sv}");
    int count = 0;
    auto add = [&](const char* name, GVariant* value) {
        g.g_variant_builder_add(changed, "{sv}", name, value);
        ++count;
    };
    if (s.playing != a.playing || s.hasFile != a.hasFile)
        add("PlaybackStatus", g.g_variant_new("s", Impl::status(s)));
    if (s.hasFile != a.hasFile || s.title != a.title || s.path != a.path || s.lengthUs != a.lengthUs)
        add("Metadata", m.metadata(s));
    if (s.hasFile != a.hasFile) {
        for (const char* name : { "CanPlay", "CanPause", "CanSeek" })
            add(name, g.g_variant_new("b", s.hasFile ? 1 : 0));
    }
    if (s.canNext != a.canNext)
        add("CanGoNext", g.g_variant_new("b", s.canNext ? 1 : 0));
    if (s.canPrevious != a.canPrevious)
        add("CanGoPrevious", g.g_variant_new("b", s.canPrevious ? 1 : 0));
    if (std::abs(s.rate - a.rate) > 1.0e-6)
        add("Rate", g.g_variant_new("d", s.rate));
    if (count > 0) {
        GVariant* empty = m.stringArray({});
        m.emit("org.freedesktop.DBus.Properties", "PropertiesChanged",
               g.g_variant_new("(s@a{sv}@as)", kPlayerInterface, g.g_variant_builder_end(changed), empty));
    }
    g.g_variant_builder_unref(changed);
    // A jump (a seek, a new file): "Seeked", as the position is not announced as it runs.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double expected = (double) a.positionUs + (a.playing ? (now - m.announcedAt) * 1000.0 * a.rate : 0.0);
    if (s.hasFile && std::abs((double) s.positionUs - expected) > 1.5e6)
        m.emit(kPlayerInterface, "Seeked", g.g_variant_new("(x)", (std::int64_t) s.positionUs));
    m.announced = s;
    m.announcedAt = now;
}

#else // Windows and macOS: no D-Bus

bool available()
{
    return false;
}

KeepAwake::KeepAwake() = default;
KeepAwake::~KeepAwake()
{
    set(false, {});
}

bool KeepAwake::set(bool hold, const std::string&)
{
#if defined(_WIN32)
    if (hold == holding)
        return holding;
    // The display and the system kept on while this thread (the message thread) says so.
    SetThreadExecutionState(hold ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
    holding = hold;
    kind = hold ? 4 : 0;
    method = "Windows";
    return holding;
#else
    (void) hold;
    method = "not on this system";
    return false;
#endif
}

PerformanceProfile::~PerformanceProfile() = default;
bool PerformanceProfile::set(bool, const std::string&)
{
    method = "not on this system";
    return false;
}

bool notify(const std::string&, const std::string&)
{
    return false;
}

struct MediaControls::Impl {};
MediaControls::MediaControls(Commands) : impl(std::make_unique<Impl>()) {}
MediaControls::~MediaControls() = default;
bool MediaControls::active() const noexcept
{
    return false;
}
void MediaControls::update(const PlayerState&) {}

#endif

} // namespace pf::app::desktop
