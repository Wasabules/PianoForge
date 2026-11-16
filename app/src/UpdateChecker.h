#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace pf::app {

/// Updates, anticipated. Where to look is a setting, empty by default: then nothing is checked and nothing leaves
/// the computer. The address gives either
/// - a manifest of PianoForge's own, written next to the installer by the `installer` build target:
///     { "version": "0.2.0", "url": "PianoForge-0.2.0-setup.exe", "sha256": "<64 hex digits>", "size": 9876543,
///       "notes": "What changed" }
///   (a relative url is taken next to the manifest: the folder can be moved, served or shared as it is), or
/// - GitHub's latest release, https://api.github.com/repos/<owner>/<repo>/releases/latest: the tag is the version,
///   the asset whose name ends in "-setup.exe" the installer, its "digest" (or an asset "<name>.sha256") the SHA-256.
/// https, or a local file (http only to this computer, for tests). An installer without a SHA-256 is refused.
/// A newer version is downloaded to the temporary folder, its size and SHA-256 checked, and the installer run
/// silently once the application has closed (it opens it again). No code signature is checked: the SHA-256 guards
/// against a damaged download; the trust is in the address (https).
/// All static, blocking: call from a worker thread.
class UpdateChecker {
public:
    struct Release {
        juce::String version, notes;
        juce::String installer;     // its address (resolved against the manifest's)
        juce::String sha256;        // lowercase hex, 64 digits
        juce::String sha256Address; // GitHub without a digest: an asset holding it (fetch() reads it)
        juce::int64 size = -1;      // bytes, -1: unknown
        juce::String fileName() const; // what to save the installer as
    };
    struct Result {
        bool ok = false;     // read and understood
        juce::String error;  // otherwise, why
        Release release;
        bool newer = false;  // than the version asked about
    };

    /// -1, 0 or 1. "1.10.0" > "1.9.2"; missing parts are 0; a leading "v" is ignored; a pre-release ("2.0.0-beta")
    /// comes before its release.
    static int compareVersions(const juce::String& a, const juce::String& b);
    /// https, a file, or http to this computer.
    static bool addressAllowed(const juce::String& address);
    /// A manifest or a GitHub release (JSON text) read from `address`, against the version `current`.
    /// The end of this system's package's name: Windows' installer ("-setup.exe"), a Debian package on Linux
    /// ("_amd64.deb", "_arm64.deb"); empty where PianoForge has none.
    static juce::String packageSuffix();
    /// `suffix`: the package looked for (a GitHub release's asset; in PianoForge's own manifest, "url" for Windows'
    /// installer, "deb": { "url", "sha256", "size" } for the Debian package).
    static Result parse(const juce::String& json, const juce::String& current, const juce::String& address,
                        const juce::String& suffix = packageSuffix());
    /// Reads `address`, then parse() (and the SHA-256 asset of a GitHub release without a digest).
    static Result fetch(const juce::String& address, const juce::String& current, int timeoutMs = 10000,
                        const juce::String& suffix = packageSuffix());
    /// Downloads the installer to `destination`, checks its size and SHA-256; `progress` (0..1) returns false to
    /// cancel. An error, or empty. Nothing is left at `destination` unless it checks out.
    static juce::String download(const Release& release, const juce::File& destination,
                                 const std::function<bool(double)>& progress = {}, int timeoutMs = 15000);
    static juce::String sha256Of(const juce::File& file);
    /// What to give the installer (Inno Setup): silent, no message boxes, no reboot, close what uses its files, and
    /// open PianoForge again when it is done.
    static juce::String installerArguments() { return "/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /relaunch=1"; }
};

} // namespace pf::app
