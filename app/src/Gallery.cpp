// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Gallery.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace pf::app {

namespace {

constexpr const char* kAudioOrder[] = { ".wav", ".flac", ".opus", ".mp3" }; // what to listen to, lossless first
constexpr const char* kPatterns = "*.wav;*.flac;*.mp3;*.opus;*.mid;*.json";

/// A file's length (s) and note count (-1: not MIDI), cached by path, size and date.
struct Measure {
    double seconds = 0.0;
    int notes = -1;
};

Measure measure(const juce::File& f)
{
    static std::mutex lock;
    static std::map<juce::String, Measure> cache;
    const auto key = f.getFullPathName() + "|" + juce::String(f.getSize()) + "|" + juce::String(f.getLastModificationTime().toMilliseconds());
    {
        std::lock_guard<std::mutex> guard(lock);
        if (const auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    Measure m;
    if (f.hasFileExtension(".mid")) {
        juce::MidiFile midi;
        if (juce::FileInputStream in(f); in.openedOk() && midi.readFrom(in, false)) {
            midi.convertTimestampTicksToSeconds();
            m.notes = 0;
            for (int t = 0; t < midi.getNumTracks(); ++t)
                for (const auto* ev : *midi.getTrack(t)) {
                    m.notes += ev->message.isNoteOn() ? 1 : 0;
                    m.seconds = std::max(m.seconds, ev->message.getTimeStamp());
                }
        }
    } else {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor(f) }; r != nullptr && r->sampleRate > 0)
            m.seconds = (double) r->lengthInSamples / r->sampleRate;
    }
    std::lock_guard<std::mutex> guard(lock);
    cache[key] = m;
    return m;
}

juce::String suffixOf(const juce::File& f, const juce::String& stem)
{
    return f.getFileName().substring(stem.length());
}

/// A file name Windows accepts, the title kept as written as far as possible (JUCE's createLegalFileName also
/// removes '#' and ',': "C# minor, Op. 27" must stay so): ':' becomes " -", slashes and bars '-', the other
/// forbidden characters go; no trailing dot or space.
juce::String legalName(const juce::String& text)
{
    juce::String out;
    for (auto c : text) {
        if (c < 32 || c == '<' || c == '>' || c == '"' || c == '?' || c == '*')
            continue;
        if (c == ':')
            out << " -";
        else if (c == '/' || c == '\\' || c == '|')
            out << '-';
        else
            out << juce::String::charToString(c);
    }
    while (out.contains("  "))
        out = out.replace("  ", " ");
    return out.trim().trimCharactersAtEnd(". ").substring(0, 180).trim();
}

} // namespace

// ---- Items ----

juce::File GalleryItem::audio() const
{
    for (const auto* ext : kAudioOrder)
        for (const auto& f : files)
            if (f.hasFileExtension(ext) && !f.getFileNameWithoutExtension().endsWith(".master"))
                return f;
    for (const auto& f : files)
        if (f.hasFileExtension(".wav"))
            return f; // only the master
    return {};
}

juce::File GalleryItem::midi() const
{
    for (const auto& f : files)
        if (f.hasFileExtension(".mid"))
            return f;
    return {};
}

juce::String GalleryItem::extensions() const
{
    juce::StringArray e;
    for (const auto& f : files)
        if (!f.hasFileExtension(".json"))
            e.addIfNotAlreadyThere(f.getFileNameWithoutExtension().endsWith(".master") ? "master" : f.getFileExtension().substring(1));
    return e.joinIntoString(" ");
}

bool GalleryItem::has(const juce::String& extension) const
{
    for (const auto& f : files)
        if (f.hasFileExtension(extension))
            return true;
    return false;
}

bool isGalleryFile(const juce::File& f)
{
    return f.hasFileExtension(".wav;.flac;.mp3;.opus;.mid;.json");
}

juce::String galleryStemOf(const juce::File& f)
{
    auto stem = f.getFileNameWithoutExtension();
    return stem.endsWith(".master") ? stem.dropLastCharacters(7) : stem; // the master of a recording made in other formats only
}

std::vector<GalleryItem> scanGallery(const std::vector<std::pair<juce::File, bool>>& roots)
{
    std::map<juce::String, GalleryItem> byKey;
    for (const auto& [root, recordings] : roots) {
        if (!root.isDirectory())
            continue;
        for (const auto& f : root.findChildFiles(juce::File::findFiles, true, kPatterns)) {
            const auto key = f.getParentDirectory().getFullPathName() + "|" + galleryStemOf(f);
            auto& item = byKey[key];
            if (item.files.empty()) {
                item.stem = galleryStemOf(f);
                item.folder = f.getParentDirectory();
                item.recording = recordings;
                item.time = f.getCreationTime();
            }
            item.files.push_back(f);
            if (!f.hasFileExtension(".json"))
                item.time = std::min(item.time, f.getCreationTime());
        }
    }
    std::vector<GalleryItem> items;
    for (auto& [key, item] : byKey) {
        bool sound = false;
        for (const auto& f : item.files)
            sound = sound || !f.hasFileExtension(".json");
        if (!sound)
            continue; // a metadata file alone (its files gone): not an item
        for (const auto& f : item.files)
            item.bytes += f.getSize();
        if (const auto a = item.audio(); a.existsAsFile())
            item.seconds = measure(a).seconds;
        if (const auto m = item.midi(); m.existsAsFile()) {
            const auto mm = measure(m);
            item.notes = mm.notes;
            if (item.seconds <= 0.0)
                item.seconds = mm.seconds;
        }
        if (const auto meta = item.metaFile(); meta.existsAsFile())
            item.meta = readGalleryMeta(meta);
        items.push_back(std::move(item));
    }
    std::stable_sort(items.begin(), items.end(), [](const GalleryItem& a, const GalleryItem& b) { return a.time > b.time; });
    return items;
}

GalleryMeta readGalleryMeta(const juce::File& metaFile)
{
    GalleryMeta m;
    const auto json = juce::JSON::parse(metaFile.loadFileAsString());
    if (auto* o = json.getDynamicObject()) {
        m.piece = o->getProperty("piece").toString();
        m.composer = o->getProperty("composer").toString();
        m.tags = o->getProperty("tags").toString();
        m.notes = o->getProperty("notes").toString();
        m.rating = juce::jlimit(0, 5, (int) o->getProperty("rating"));
    }
    return m;
}

bool writeGalleryMeta(const juce::File& metaFile, const GalleryMeta& meta)
{
    if (meta.empty())
        return !metaFile.exists() || metaFile.deleteFile();
    auto* o = new juce::DynamicObject();
    o->setProperty("piece", meta.piece);
    o->setProperty("composer", meta.composer);
    o->setProperty("tags", meta.tags);
    o->setProperty("rating", meta.rating);
    o->setProperty("notes", meta.notes);
    o->setProperty("app", "PianoForge");
    return metaFile.replaceWithText(juce::JSON::toString(juce::var(o), false));
}

// ---- Naming ----

juce::String galleryStem(const GalleryNaming& naming, const GalleryMeta& meta, juce::Time time, int number)
{
    if (meta.piece.trim().isEmpty())
        return GalleryNaming::automaticStem(time);
    const std::pair<const char*, juce::String> tokens[] = {
        { "{piece}", meta.piece.trim() },
        { "{composer}", meta.composer.trim() },
        { "{date}", time.formatted("%Y-%m-%d") },
        { "{time}", time.formatted("%H-%M") },
        { "{n}", number > 0 ? juce::String(number) : juce::String() },
        { "{tags}", meta.tags.trim() },
    };
    // Token by token: an empty one takes the separator before it away (" - {composer}" with no composer: nothing).
    auto pattern = naming.pattern.trim().isEmpty() ? juce::String("{piece} - {date} {time}") : naming.pattern;
    juce::String out;
    int i = 0;
    while (i < pattern.length()) {
        if (pattern[i] == '{') {
            const int close = pattern.indexOfChar(i, '}');
            if (close > i) {
                const auto token = pattern.substring(i, close + 1);
                juce::String value = token; // an unknown token stays as written
                for (const auto& [name, v] : tokens)
                    if (token == name)
                        value = v;
                if (value.isEmpty())
                    out = out.trimCharactersAtEnd(" -_,.;(");
                out << value;
                i = close + 1;
                continue;
            }
        }
        out << pattern[i++];
    }
    out = out.trim().trimCharactersAtStart(" -_,.;)").trimCharactersAtEnd(" -_,.;(");
    out = out.replace("()", "").trim();
    // A bracket left without its pair (an empty token inside it): away.
    while (out.endsWith(")") && out.indexOfChar('(') < 0)
        out = out.dropLastCharacters(1).trim();
    const auto legal = legalName(out);
    return legal.isEmpty() ? GalleryNaming::automaticStem(time) : legal;
}

juce::File galleryFolder(const GalleryNaming& naming, const GalleryMeta& meta, const juce::File& root)
{
    const auto piece = legalName(meta.piece);
    return naming.folderPerPiece && piece.isNotEmpty() ? root.getChildFile(piece) : root;
}

juce::StringArray gallerySuffixes(const GalleryItem& item)
{
    juce::StringArray s;
    for (const auto& f : item.files)
        s.add(suffixOf(f, item.stem));
    return s;
}

juce::String uniqueGalleryStem(const juce::File& folder, const juce::String& stem, const juce::StringArray& suffixes,
                               const std::vector<juce::File>& except)
{
    auto taken = [&](const juce::String& candidate) {
        for (const auto& suffix : suffixes) {
            const auto f = folder.getChildFile(candidate + suffix);
            if (f.exists() && std::find(except.begin(), except.end(), f) == except.end())
                return true;
        }
        return false;
    };
    if (!taken(stem))
        return stem;
    for (int k = 2; k < 1000; ++k)
        if (const auto candidate = stem + " (" + juce::String(k) + ")"; !taken(candidate))
            return candidate;
    return stem + " " + juce::Time::getCurrentTime().formatted("%H%M%S");
}

juce::Result moveGalleryItem(GalleryItem& item, const juce::File& folder, const juce::String& wanted)
{
    const auto suffixes = gallerySuffixes(item);
    const auto stem = uniqueGalleryStem(folder, wanted, suffixes, item.files);
    if (stem == item.stem && folder == item.folder)
        return juce::Result::ok();
    if (!folder.isDirectory() && !folder.createDirectory())
        return juce::Result::fail("Cannot create " + folder.getFullPathName());
    const auto oldFolder = item.folder;
    std::vector<juce::File> moved;
    for (size_t i = 0; i < item.files.size(); ++i) {
        const auto target = folder.getChildFile(stem + suffixes[(int) i]);
        if (!item.files[i].moveFileTo(target)) {
            for (size_t k = 0; k < moved.size(); ++k) // back as it was
                moved[k].moveFileTo(oldFolder.getChildFile(item.stem + suffixes[(int) k]));
            return juce::Result::fail("Cannot move " + item.files[i].getFileName() + " (is it open elsewhere?)");
        }
        moved.push_back(target);
    }
    item.files = moved;
    item.folder = folder;
    item.stem = stem;
    // A piece's folder emptied: removed (never a root: it holds other things, or will).
    if (oldFolder != folder && oldFolder.isDirectory() && oldFolder.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0
        && oldFolder.getFileName() != "Recordings" && oldFolder.getFileName() != "Takes")
        oldFolder.deleteFile();
    return juce::Result::ok();
}

// ---- Filters ----

bool GalleryFilter::matches(const GalleryItem& item) const
{
    if ((kind == recordings && !item.recording) || (kind == takes && item.recording))
        return false;
    if (unnamedOnly && item.meta.piece.isNotEmpty())
        return false;
    if (piece.isNotEmpty() && item.meta.piece != piece)
        return false;
    if (item.meta.rating < minRating)
        return false;
    if (item.seconds < minSeconds || (maxSeconds >= 0.0 && item.seconds > maxSeconds))
        return false;
    if (item.bytes < minBytes || (maxBytes >= 0 && item.bytes > maxBytes))
        return false;
    if (since.toMilliseconds() > 0 && item.time < since)
        return false;
    if (types != anyType) {
        const bool any = ((types & midi) && item.has(".mid")) || ((types & wav) && item.has(".wav")) || ((types & mp3) && item.has(".mp3"))
                      || ((types & flac) && item.has(".flac")) || ((types & opus) && item.has(".opus"));
        if (!any)
            return false;
    }
    if (search.trim().isNotEmpty()) {
        const auto haystack = item.meta.piece + " " + item.meta.composer + " " + item.meta.tags + " " + item.meta.notes + " " + item.stem;
        for (const auto& word : juce::StringArray::fromTokens(search, " ", "\""))
            if (word.isNotEmpty() && !haystack.containsIgnoreCase(word))
                return false;
    }
    return true;
}

juce::StringArray galleryPieces(const std::vector<GalleryItem>& items)
{
    juce::StringArray pieces; // items are newest first: the most recent pieces first
    for (const auto& item : items)
        if (item.meta.piece.isNotEmpty())
            pieces.addIfNotAlreadyThere(item.meta.piece);
    return pieces;
}

juce::StringArray galleryComposers(const std::vector<GalleryItem>& items)
{
    juce::StringArray composers;
    for (const auto& item : items)
        if (item.meta.composer.isNotEmpty())
            composers.addIfNotAlreadyThere(item.meta.composer);
    return composers;
}

juce::String galleryComposerOf(const std::vector<GalleryItem>& items, const juce::String& piece)
{
    for (const auto& item : items)
        if (item.meta.piece == piece && item.meta.composer.isNotEmpty())
            return item.meta.composer;
    return {};
}

int galleryCount(const std::vector<GalleryItem>& items, const juce::String& piece)
{
    int n = 0;
    for (const auto& item : items)
        n += item.meta.piece == piece ? 1 : 0;
    return n;
}

juce::String galleryClock(double seconds)
{
    const int s = juce::roundToInt(seconds);
    if (s >= 3600)
        return juce::String(s / 3600) + ":" + juce::String((s / 60) % 60).paddedLeft('0', 2) + ":" + juce::String(s % 60).paddedLeft('0', 2);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

juce::String gallerySize(juce::int64 bytes)
{
    if (bytes < 1000 * 1000)
        return juce::String(juce::jmax<juce::int64>(1, bytes / 1000)) + " KB";
    if (bytes < (juce::int64) 1000 * 1000 * 1000) // (String(x, 0) would be every digit: rounded here)
        return bytes < 10 * 1000 * 1000 ? juce::String((double) bytes / 1e6, 1) + " MB" : juce::String(juce::roundToInt((double) bytes / 1e6)) + " MB";
    return juce::String((double) bytes / 1e9, 1) + " GB";
}

} // namespace pf::app
