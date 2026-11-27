// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace pf::app {

/// What the gallery knows of a recording or a take beyond its files: the piece played, its composer, tags, a rating
/// (0 to 5 stars) and notes. Kept next to the files, in "<name>.json", which follows them when the gallery renames
/// or moves them.
struct GalleryMeta {
    juce::String piece, composer, tags, notes;
    int rating = 0;
    bool empty() const { return piece.isEmpty() && composer.isEmpty() && tags.isEmpty() && notes.isEmpty() && rating == 0; }
};

/// One recording or take: its files (the sounds, the MIDI, the metadata) sharing one name in one folder.
struct GalleryItem {
    juce::String stem;      // the files' common name
    juce::File folder;
    bool recording = true;  // Recordings (Ctrl+R), else Takes (Ctrl+S)
    juce::Time time;        // when it was made (the earliest file)
    double seconds = 0.0;   // its length (the sound, else the MIDI)
    int notes = -1;         // -1: no MIDI
    juce::int64 bytes = 0;  // every file together
    std::vector<juce::File> files;
    GalleryMeta meta;

    juce::File audio() const; // the best one to listen to (lossless first)
    juce::File midi() const;
    juce::File metaFile() const { return folder.getChildFile(stem + ".json"); }
    /// The formats it has ("wav mp3 mid"; "master" for a master kept).
    juce::String extensions() const;
    bool has(const juce::String& extension) const; // ".wav", ".mp3"...
    /// The piece as shown: its metadata's, or none.
    juce::String title() const { return meta.piece; }
};

/// The sound and MIDI files a gallery item is made of, and what each one's stem is.
bool isGalleryFile(const juce::File& f);
juce::String galleryStemOf(const juce::File& f);

/// Every item under these folders (their subfolders too: a folder per piece), newest first. `recordings` says, for
/// each root, whether it holds recordings (else takes). Lengths and note counts are cached per file (path, size,
/// date): reading the folders again is quick.
std::vector<GalleryItem> scanGallery(const std::vector<std::pair<juce::File, bool>>& roots);

GalleryMeta readGalleryMeta(const juce::File& metaFile);
/// Writes the metadata (none: the file removed).
bool writeGalleryMeta(const juce::File& metaFile, const GalleryMeta& meta);

/// How recordings are named and filed. The pattern's tokens: {piece}, {composer}, {date} (2026-11-16), {time}
/// (21-05), {n} (this recording's number among the piece's), {tags}. Without a piece, the automatic name
/// ("PianoForge 2026-11-16 21-05-33"). With folderPerPiece, a named recording goes in a folder named after its piece.
struct GalleryNaming {
    juce::String pattern = "{piece} - {date} {time}";
    bool folderPerPiece = true;
    static juce::String automaticStem(juce::Time t) { return "PianoForge " + t.formatted("%Y-%m-%d %H-%M-%S"); }
};

/// The name the pattern gives (a legal file name; empty tokens and the separators around them left out).
juce::String galleryStem(const GalleryNaming& naming, const GalleryMeta& meta, juce::Time time, int number);
/// The folder a named item goes to under `root`.
juce::File galleryFolder(const GalleryNaming& naming, const GalleryMeta& meta, const juce::File& root);
/// `stem`, or "stem (2)", "stem (3)"... so that no file named so exists in `folder` with these extensions (`except`
/// the item's own files, renamed to it).
juce::String uniqueGalleryStem(const juce::File& folder, const juce::String& stem, const juce::StringArray& suffixes,
                               const std::vector<juce::File>& except = {});
/// The suffixes after the stem of each file (".wav", ".master.wav", ".json"...).
juce::StringArray gallerySuffixes(const GalleryItem& item);

/// Moves every file of `item` to `folder` under `stem` (none replaced: a name taken gets " (2)"...). The item is
/// updated. An empty folder left behind (a piece's, nothing left in it) is removed.
juce::Result moveGalleryItem(GalleryItem& item, const juce::File& folder, const juce::String& stem);

/// What the gallery shows.
struct GalleryFilter {
    enum Kind { all, recordings, takes };
    enum Type { midi = 1, wav = 2, mp3 = 4, flac = 8, opus = 16, anyType = 31 };
    juce::String search;          // in the piece, the composer, the tags, the notes and the name
    Kind kind = all;
    int types = anyType;          // an item with any of these
    bool unnamedOnly = false;     // no piece given yet
    juce::String piece;           // this piece only (empty: any)
    int minRating = 0;
    double minSeconds = 0.0, maxSeconds = -1.0;     // -1: no upper bound
    juce::int64 minBytes = 0, maxBytes = -1;
    juce::Time since;             // made after (0: any time)
    bool matches(const GalleryItem& item) const;
};

/// The pieces of the items (the most recent first), and for each its composer last given.
juce::StringArray galleryPieces(const std::vector<GalleryItem>& items);
juce::StringArray galleryComposers(const std::vector<GalleryItem>& items);
juce::String galleryComposerOf(const std::vector<GalleryItem>& items, const juce::String& piece);
int galleryCount(const std::vector<GalleryItem>& items, const juce::String& piece);

/// "2:41", "1:02:05".
juce::String galleryClock(double seconds);
/// "820 KB", "12.4 MB", "1.2 GB".
juce::String gallerySize(juce::int64 bytes);

} // namespace pf::app
