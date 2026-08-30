#include "util/library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>

namespace transcriptor::library {

namespace {

// Playable in an <audio> element, or at least worth offering back to the user:
// recordings are audio.wav, but an uploaded file keeps whatever name it had.
const std::set<std::string>& media_extensions() {
    static const std::set<std::string> kExt = {
        ".wav", ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".flac",
        ".webm", ".weba", ".mp4", ".m4v", ".mkv", ".mov", ".avi", ".wma",
    };
    return kExt;
}

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// C++17 has no portable file_time_type -> time_t conversion; this is the usual
// workaround. Only used for display, so the small clock skew does not matter.
std::int64_t mtime_seconds(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(sys));
}

// One line of readable text for the list: whitespace collapsed, cut on a word
// boundary. `limit` counts bytes, so a multi-byte character is never split.
std::string snippet(const std::string& text, std::size_t limit = 180) {
    std::string flat;
    flat.reserve(std::min(text.size(), limit + 32));
    bool space = false;
    for (char c : text) {
        const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (ws) { space = !flat.empty(); continue; }
        if (space) { flat += ' '; space = false; }
        flat += c;
        if (flat.size() >= limit + 24) break;
    }
    if (flat.size() <= limit) return flat;

    std::size_t cut = flat.rfind(' ', limit);
    if (cut == std::string::npos || cut < limit / 2) {
        // No word boundary near the limit: back off to a character boundary so
        // the snippet stays valid UTF-8.
        cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80) --cut;
    }
    return flat.substr(0, cut) + "…";
}

}  // namespace

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    if (id == "." || id == "..") return false;
    for (char c : id) {
        if (c == '/' || c == '\\' || c == ':') return false;
        // Control characters have no business in a directory name we built.
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

fs::path resolve(const std::string& output_dir, const std::string& id) {
    if (!valid_id(id) || output_dir.empty()) return {};

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(paths::expand_user(output_dir), ec);
    if (ec) return {};
    const fs::path dir = fs::weakly_canonical(root / paths::from_utf8(id), ec);
    if (ec) return {};

    // Belt and braces: valid_id already rules out traversal, but the resolved
    // path must still be a direct child of the output folder.
    if (dir.parent_path() != root) return {};
    if (!fs::is_directory(dir, ec)) return {};
    return dir;
}

std::string find_audio(const fs::path& dir) {
    std::error_code ec;
    const fs::path preferred = dir / "audio.wav";
    if (fs::is_regular_file(preferred, ec)) return "audio.wav";

    std::string best;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = paths::to_utf8(e.path().filename());
        if (!media_extensions().count(lower(paths::to_utf8(e.path().extension())))) {
            continue;
        }
        if (best.empty() || name < best) best = name;
    }
    return best;
}

std::string content_type_for(const std::string& filename) {
    static const std::map<std::string, std::string> kTypes = {
        {".wav", "audio/wav"},   {".mp3", "audio/mpeg"},  {".m4a", "audio/mp4"},
        {".aac", "audio/aac"},   {".ogg", "audio/ogg"},   {".oga", "audio/ogg"},
        {".opus", "audio/ogg"},  {".flac", "audio/flac"}, {".webm", "audio/webm"},
        {".weba", "audio/webm"}, {".mp4", "video/mp4"},   {".m4v", "video/mp4"},
        {".mkv", "video/x-matroska"}, {".mov", "video/quicktime"},
        {".avi", "video/x-msvideo"},  {".wma", "audio/x-ms-wma"},
    };
    const auto dot = filename.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    auto it = kTypes.find(lower(filename.substr(dot)));
    return it == kTypes.end() ? "application/octet-stream" : it->second;
}

Entry describe(const fs::path& dir) {
    std::error_code ec;
    Entry e;
    e.id    = paths::to_utf8(dir.filename());
    e.path  = paths::to_utf8(dir);
    e.mtime = mtime_seconds(dir);

    e.has_transcript = fs::is_regular_file(dir / "transcript.json", ec) ||
                       fs::is_regular_file(dir / "transcript.txt", ec);
    e.has_summary    = fs::is_regular_file(dir / "summary.txt", ec);

    e.audio = find_audio(dir);
    if (!e.audio.empty()) {
        const auto size = fs::file_size(dir / paths::from_utf8(e.audio), ec);
        if (!ec) e.audio_bytes = static_cast<std::uint64_t>(size);
    }

    // The transcript makes the better preview; the summary is the fallback for
    // a session saved with "save transcript" switched off.
    std::string raw;
    if (paths::read_file(dir / "transcript.txt", &raw) && !raw.empty()) {
        e.preview = snippet(raw);
    } else if (paths::read_file(dir / "summary.txt", &raw)) {
        e.preview = snippet(raw);
    }
    return e;
}

std::vector<Entry> list(const std::string& output_dir) {
    std::vector<Entry> out;
    if (output_dir.empty()) return out;

    std::error_code ec;
    const fs::path root = paths::expand_user(output_dir);
    if (!fs::is_directory(root, ec)) return out;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string id = paths::to_utf8(entry.path().filename());
        if (!valid_id(id) || id[0] == '.') continue;
        Entry e = describe(entry.path());
        // A folder with none of the three artifacts is not a session.
        if (!e.has_transcript && !e.has_summary && e.audio.empty()) continue;
        out.push_back(std::move(e));
    }

    // Session ids are "YYYY-MM-DD_HH-MM-SS", so descending text order is
    // newest-first; mtime only breaks ties for hand-made folder names.
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.id != b.id) return a.id > b.id;
        return a.mtime > b.mtime;
    });
    return out;
}

}  // namespace transcriptor::library
