#include "UpdateChecker.h"

#include <juce_cryptography/juce_cryptography.h>

#include <array>
#include <memory>

namespace pf::app {

namespace {

bool isSha256(const juce::String& s)
{
    return s.length() == 64 && s.containsOnly("0123456789abcdef");
}

bool isLocal(const juce::String& address)
{
    return address.startsWithIgnoreCase("file:");
}

/// A relative address taken next to `base` (a manifest's folder); an absolute one as it is.
juce::String resolve(const juce::String& address, const juce::String& base)
{
    if (address.isEmpty() || address.contains("://") || isLocal(address))
        return address;
    return base.upToLastOccurrenceOf("/", true, false) + address;
}

std::unique_ptr<juce::InputStream> open(const juce::String& address, int timeoutMs, int* status, juce::String& error)
{
    if (!UpdateChecker::addressAllowed(address)) {
        error = "not an https address: " + address;
        return nullptr;
    }
    if (isLocal(address)) {
        const auto file = juce::URL(address).getLocalFile();
        auto in = file.createInputStream();
        if (in == nullptr)
            error = "cannot read " + file.getFullPathName();
        return in;
    }
    // GitHub's API wants a User-Agent: the application's name and version, nothing about the person.
    auto in = juce::URL(address).createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                       .withExtraHeaders("User-Agent: PianoForge/" PIANOFORGE_VERSION "\r\n"
                                                                         "Accept: application/vnd.github+json, application/json, */*")
                                                       .withConnectionTimeoutMs(timeoutMs)
                                                       .withNumRedirectsToFollow(5)
                                                       .withStatusCode(status));
    if (in == nullptr)
        error = "cannot reach " + address;
    else if (status != nullptr && *status >= 400) {
        error = "the server answered " + juce::String(*status) + " for " + address;
        return nullptr;
    }
    return in;
}

} // namespace

juce::String UpdateChecker::Release::fileName() const
{
    auto name = installer.fromLastOccurrenceOf("/", false, false).upToFirstOccurrenceOf("?", false, false);
    name = juce::URL::removeEscapeChars(name);
    name = juce::File::createLegalFileName(name);
    if (!name.endsWithIgnoreCase(".exe") && !name.endsWithIgnoreCase(".deb"))
        name = "PianoForge-" + juce::File::createLegalFileName(version) + "-setup.exe";
    return name;
}

juce::String UpdateChecker::packageSuffix()
{
#if JUCE_WINDOWS
    return "-setup.exe";
#elif JUCE_LINUX && (defined(__aarch64__) || defined(__arm64__))
    return "_arm64.deb";
#elif JUCE_LINUX
    return "_amd64.deb";
#else
    return {};
#endif
}

int UpdateChecker::compareVersions(const juce::String& a, const juce::String& b)
{
    auto parts = [](juce::String v) {
        v = v.trim();
        if (v.startsWithIgnoreCase("v"))
            v = v.substring(1);
        const int cut = v.indexOfAnyOf("-+ ");
        const bool prerelease = cut >= 0 && v[cut] == '-';
        if (cut >= 0)
            v = v.substring(0, cut);
        const auto tokens = juce::StringArray::fromTokens(v, ".", "");
        std::array<int, 4> n {}; // major, minor, patch; then 1 for a release, 0 for a pre-release
        for (int i = 0; i < 3; ++i)
            n[(size_t) i] = i < tokens.size() ? tokens[i].getIntValue() : 0;
        n[3] = prerelease ? 0 : 1;
        return n;
    };
    const auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool UpdateChecker::addressAllowed(const juce::String& address)
{
    return address.startsWithIgnoreCase("https://") || isLocal(address) || address.startsWithIgnoreCase("http://localhost")
        || address.startsWithIgnoreCase("http://127.0.0.1");
}

UpdateChecker::Result UpdateChecker::parse(const juce::String& text, const juce::String& current, const juce::String& address,
                                           const juce::String& suffix)
{
    if (suffix.isEmpty()) {
        Result none;
        none.error = "PianoForge has no package for this system";
        return none;
    }
    Result r;
    const auto json = juce::JSON::parse(text);
    if (!json.isObject()) {
        r.error = "not a manifest (JSON expected)";
        return r;
    }
    Release release;
    if (json.hasProperty("tag_name")) { // a GitHub release
        release.version = json["tag_name"].toString().trim();
        if (release.version.startsWithIgnoreCase("v"))
            release.version = release.version.substring(1);
        release.notes = json["body"].toString();
        if (const auto* assets = json["assets"].getArray()) {
            juce::String installerName;
            for (const auto& asset : *assets) {
                const auto name = asset["name"].toString();
                if (name.endsWithIgnoreCase(suffix) && release.installer.isEmpty()) {
                    installerName = name;
                    release.installer = asset["browser_download_url"].toString();
                    release.size = asset.hasProperty("size") ? (juce::int64) asset["size"] : -1;
                    const auto digest = asset["digest"].toString();
                    if (digest.startsWithIgnoreCase("sha256:"))
                        release.sha256 = digest.substring(7).toLowerCase();
                }
            }
            if (release.sha256.isEmpty() && installerName.isNotEmpty())
                for (const auto& asset : *assets)
                    if (asset["name"].toString().equalsIgnoreCase(installerName + ".sha256"))
                        release.sha256Address = asset["browser_download_url"].toString();
        }
    } else { // PianoForge's own manifest: Windows' installer at its top, a Debian package under "deb"
        release.version = json["version"].toString().trim();
        release.notes = json["notes"].toString();
        const auto package = suffix.endsWithIgnoreCase(".deb") ? json["deb"] : json;
        release.installer = resolve(package["url"].toString().trim(), address);
        release.sha256 = package["sha256"].toString().trim().toLowerCase();
        release.size = package.hasProperty("size") ? (juce::int64) package["size"] : -1;
    }
    if (release.version.isEmpty() || !release.version.containsAnyOf("0123456789")) {
        r.error = "no version in the manifest";
        return r;
    }
    if (release.installer.isEmpty()) {
        r.error = "no installer in the manifest";
        return r;
    }
    if (!addressAllowed(release.installer)) {
        r.error = "the installer is not at an https address: " + release.installer;
        return r;
    }
    if (!isSha256(release.sha256) && release.sha256Address.isEmpty()) {
        r.error = "no SHA-256 for the installer: it would not be installed";
        return r;
    }
    r.ok = true;
    r.release = release;
    r.newer = compareVersions(release.version, current) > 0;
    return r;
}

UpdateChecker::Result UpdateChecker::fetch(const juce::String& address, const juce::String& current, int timeoutMs,
                                           const juce::String& suffix)
{
    Result r;
    int status = 0;
    auto in = open(address.trim(), timeoutMs, &status, r.error);
    if (in == nullptr)
        return r;
    r = parse(in->readEntireStreamAsString(), current, address.trim(), suffix);
    if (r.ok && r.release.sha256.isEmpty()) { // GitHub, the SHA-256 in an asset of its own
        juce::String error;
        if (auto sum = open(r.release.sha256Address, timeoutMs, &status, error); sum != nullptr) {
            const auto first = sum->readEntireStreamAsString().trim().upToFirstOccurrenceOf(" ", false, false).toLowerCase();
            if (isSha256(first))
                r.release.sha256 = first;
        }
        if (r.release.sha256.isEmpty()) {
            r.ok = false;
            r.error = "no SHA-256 for the installer: it would not be installed";
        }
    }
    return r;
}

juce::String UpdateChecker::sha256Of(const juce::File& file)
{
    return juce::SHA256(file).toHexString().toLowerCase();
}

juce::String UpdateChecker::download(const Release& release, const juce::File& destination,
                                     const std::function<bool(double)>& progress, int timeoutMs)
{
    if (!isSha256(release.sha256))
        return "no SHA-256 for the installer";
    juce::String error;
    int status = 0;
    auto in = open(release.installer, timeoutMs, &status, error);
    if (in == nullptr)
        return error;
    const auto part = destination.getSiblingFile(destination.getFileName() + ".part");
    destination.getParentDirectory().createDirectory();
    part.deleteFile();
    {
        juce::FileOutputStream out(part);
        if (!out.openedOk())
            return "cannot write " + part.getFullPathName();
        const juce::int64 total = release.size > 0 ? release.size : in->getTotalLength();
        juce::HeapBlock<char> buffer(65536);
        juce::int64 done = 0;
        while (!in->isExhausted()) {
            const int n = in->read(buffer, 65536);
            if (n <= 0)
                break;
            if (!out.write(buffer, (size_t) n)) {
                error = "cannot write " + part.getFullPathName();
                break;
            }
            done += n;
            if (release.size > 0 && done > release.size) {
                error = "the installer is larger than announced";
                break;
            }
            if (progress && !progress(total > 0 ? (double) done / (double) total : 0.0)) {
                error = "cancelled";
                break;
            }
        }
        out.flush();
    }
    if (error.isEmpty() && release.size > 0 && part.getSize() != release.size)
        error = "the installer is " + juce::String(part.getSize()) + " bytes, not " + juce::String(release.size);
    if (error.isEmpty()) {
        if (const auto sum = sha256Of(part); sum != release.sha256)
            error = "the installer's SHA-256 is not the one announced (" + sum.substring(0, 12) + "... against "
                  + release.sha256.substring(0, 12) + "...): a damaged or substituted download, not installed";
    }
    if (error.isNotEmpty()) {
        part.deleteFile();
        return error;
    }
    destination.deleteFile();
    if (!part.moveFileTo(destination)) {
        part.deleteFile();
        return "cannot write " + destination.getFullPathName();
    }
    return {};
}

} // namespace pf::app
