// Cutting text by byte count without splitting a character.
//
// Messages that quote another program's output are trimmed to a few hundred
// bytes, and a cut through the middle of a multi-byte character leaves bytes
// that are not UTF-8 at all. nlohmann::json refuses to serialize those, and the
// messages end up in /api/state -- so one Turkish letter in an ffmpeg error
// was enough to take the whole status endpoint down.
#pragma once

#include <cstddef>
#include <string>

namespace transcriptor::utf8 {

inline bool continuation_byte(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// At most the first `max_bytes`, ending on a character boundary.
inline std::string head(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t cut = max_bytes;
    while (cut > 0 && continuation_byte(s[cut])) --cut;
    return s.substr(0, cut);
}

// At most the last `max_bytes`, starting on a character boundary.
inline std::string tail(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t from = s.size() - max_bytes;
    while (from < s.size() && continuation_byte(s[from])) ++from;
    return s.substr(from);
}

}  // namespace transcriptor::utf8
