// The library: past sessions sitting in the configured output folder.
//
// Nothing is indexed or cached — the folder on disk is the database. A session
// is one timestamped directory holding some of audio / transcript.txt /
// transcript.json / summary.txt, exactly as exporter wrote it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/paths.h"

namespace transcriptor::library {

namespace fs = paths::fs;

struct Entry {
    std::string   id;                  // directory name, e.g. "2026-08-25_14-03-11"
    std::string   path;                // absolute, UTF-8
    std::int64_t  mtime = 0;           // seconds since the epoch, for sorting
    bool          has_transcript = false;
    bool          has_summary    = false;
    std::string   audio;               // filename inside the session, "" if none
    std::uint64_t audio_bytes = 0;
    std::string   preview;             // opening words of the transcript
};

// A saved transcript or summary inside a session.
//
// Re-running either one over an archived recording does not have to replace
// what is there: the result can be kept beside it under a name. On disk that
// is a middle segment — transcript.json / transcript.<name>.json,
// summary.txt / summary.<name>.txt — so the originals keep the names every
// earlier version of the app wrote and still reads.
struct Variant {
    std::string  name;         // "" = the original pair
    std::int64_t mtime = 0;
    bool         structured = false;   // transcripts: a .json is there, not only .txt
};

// True for a name that may sit in the middle of a file name: letters, digits,
// spaces, dashes, underscores. No dots, separators or leading/trailing space,
// so a name can never climb out of the session folder or shadow another file,
// and none of the characters Windows refuses in a file name.
bool valid_variant(const std::string& name);

// Where a variant's files live. `name` empty gives the originals.
fs::path transcript_json_file(const fs::path& dir, const std::string& name);
fs::path transcript_txt_file(const fs::path& dir, const std::string& name);
fs::path summary_file(const fs::path& dir, const std::string& name);

// What is actually on disk, original first and the rest newest-first.
std::vector<Variant> transcript_variants(const fs::path& dir);
std::vector<Variant> summary_variants(const fs::path& dir);

// True for a plain directory name: no separators, no "..", nothing exotic.
bool valid_id(const std::string& id);

// `output_dir`/`id`, but only when `id` is valid and the directory really sits
// inside the output folder. Empty path when it does not.
fs::path resolve(const std::string& output_dir, const std::string& id);

// Newest first. A missing or unreadable output folder yields an empty list
// rather than an error — an empty library is the normal first-run state.
std::vector<Entry> list(const std::string& output_dir);

// Fills in one entry from a session directory that is known to exist.
Entry describe(const fs::path& dir);

// The playable file in a session directory, if there is one ("" otherwise).
// `audio.wav` wins when present; otherwise the first media file found.
std::string find_audio(const fs::path& dir);

// Content type for an audio/video filename, for serving it back to an
// <audio> element. Unknown extensions get application/octet-stream.
std::string content_type_for(const std::string& filename);

}  // namespace transcriptor::library
