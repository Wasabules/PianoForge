// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SingleInstance.h"

#include <juce_events/juce_events.h>

#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#endif

namespace pf::app::instance {

juce::StringArray portable(const juce::StringArray& arguments)
{
    juce::StringArray out;
    for (const auto& a : arguments) {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(a.unquoted());
        out.add(!a.startsWith("--") && file.exists() ? file.getFullPathName() : a);
    }
    return out;
}

#if defined(__linux__)

namespace {

constexpr const char* kHello = "PIANOFORGE 1";

/// The socket's name: per user and settings folder (another HOME, another PianoForge), in the abstract namespace.
sockaddr_un address(socklen_t& length)
{
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getFullPathName();
    const auto name = "pianoforge-" + juce::String((int) getuid()) + "-" + juce::String::toHexString(folder.hashCode64());
    const auto utf8 = name.toStdString();
    std::memcpy(a.sun_path + 1, utf8.data(), std::min(utf8.size(), sizeof(a.sun_path) - 2)); // sun_path[0] = 0: abstract
    length = (socklen_t) (offsetof(sockaddr_un, sun_path) + 1 + std::min(utf8.size(), sizeof(a.sun_path) - 2));
    return a;
}

bool waitFor(int fd, short events, int ms)
{
    pollfd p { fd, events, 0 };
    return poll(&p, 1, ms) > 0 && (p.revents & events) != 0;
}

} // namespace

bool forward(const juce::StringArray& arguments)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    socklen_t length = 0;
    const auto a = address(length);
    bool taken = false;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&a), length) == 0) {
        juce::String message = kHello + juce::String("\n");
        for (const auto& line : portable(arguments))
            message << line.replaceCharacters("\r\n", "  ") << "\n";
        message << "\n";
        const auto utf8 = message.toStdString();
        if (::send(fd, utf8.data(), utf8.size(), MSG_NOSIGNAL) == (ssize_t) utf8.size() && waitFor(fd, POLLIN, 2000)) {
            char reply[8] = {};
            taken = ::recv(fd, reply, sizeof(reply) - 1, 0) >= 2 && std::strncmp(reply, "ok", 2) == 0;
        }
    }
    ::close(fd);
    return taken;
}

Server::Server(std::function<void(const juce::StringArray&)> f) : onArguments(std::move(f))
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    socklen_t length = 0;
    const auto a = address(length);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), length) != 0 || ::listen(fd, 4) != 0) {
        ::close(fd); // (another one listens: it did not answer forward(), so this one plays on its own)
        return;
    }
    socket = fd;
    thread = std::thread([this] { run(); });
}

Server::~Server()
{
    alive->store(false);
    stopping = true;
    if (thread.joinable())
        thread.join();
    if (socket >= 0)
        ::close(socket);
}

void Server::run()
{
    while (!stopping.load()) {
        if (!waitFor(socket, POLLIN, 200))
            continue;
        const int client = ::accept4(socket, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0)
            continue;
        std::string text;
        char buffer[4096];
        while (text.size() < 65536 && text.find("\n\n") == std::string::npos && waitFor(client, POLLIN, 1000)) {
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            text.append(buffer, (std::size_t) n);
        }
        auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(text.data(), (int) text.size()));
        if (!lines.isEmpty() && lines[0] == kHello) {
            lines.remove(0);
            lines.removeEmptyStrings();
            ::send(client, "ok\n", 3, MSG_NOSIGNAL);
            juce::MessageManager::callAsync([alive = alive, f = onArguments, lines] {
                if (alive->load() && f)
                    f(lines);
            });
        }
        ::close(client);
    }
}

#else

bool forward(const juce::StringArray&)
{
    return false;
}

Server::Server(std::function<void(const juce::StringArray&)> f) : onArguments(std::move(f)) {}
Server::~Server() = default;
void Server::run() {}

#endif

} // namespace pf::app::instance
