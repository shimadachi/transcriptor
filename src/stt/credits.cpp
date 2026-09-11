#include "stt/credits.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transcriptor::stt {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// A credit is never long, so the trailing scan is bounded. Twelve words rather
// than eight because the audio-description disclaimer below is nine on its own.
constexpr std::size_t kMaxTailWords = 12;

// Whole lines, matched exactly.
bool exact_credit(const std::string& key) {
    static const std::unordered_set<std::string> kCredits = {
        // Turkish
        "altyazimk",
        "altyazimkcom",
        "altyaziauthor",
        "aboneolmayiunutmayin",
        "kanalimaaboneolmayiunutmayin",
        "videoyubegendiyseniz",
        // Seen on its own, cut short of the full disclaimer below.
        "bubolumunbetimlemesi",
        // English / generic subtitle-site credits
        "subtitlesbytheamaraorgcommunity",
        "subtitlesbyamaraorg",
        "amaraorg",
        "thanksforwatching",
        "thankyouforwatching",
        "pleasesubscribe",
        "subscribetomychannel",
    };
    return kCredits.count(key) > 0;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// The audio-description disclaimer Turkish broadcasters read over a programme's
// opening titles: "Bu dizinin betimlemesi TRT tarafından Sesli Betimleme
// Derneğine yaptırılmıştır." Whisper has it memorised the way it has the
// subtitle credits memorised, and reaches for it when a recording opens on
// silence.
//
// Matched by its two fixed ends rather than as a line, because the middle names
// whichever broadcaster commissioned the description -- an exact entry would
// catch one channel and miss the next. Both ends are required, and that is the
// point: "Bu dizinin betimlemesi hakkında ne düşünüyorsunuz?" is a real
// question someone can ask in a meeting, and it survives because it does not
// end in the disclaimer's verb.
bool is_description_credit(const std::string& key) {
    static const std::vector<std::string> kOpenings = {
        "budizininbetimlemesi",     // dizi    -- series
        "bufilminbetimlemesi",      // film
        "buprograminbetimlemesi",   // program
        "bubolumunbetimlemesi",     // bölüm   -- episode
    };
    static const std::vector<std::string> kEndings = {
        "yaptirilmistir",           // "was had made"
        "yapilmistir",              // "was made"
    };

    for (const std::string& open : kOpenings) {
        if (!starts_with(key, open)) continue;
        for (const std::string& end : kEndings) {
            if (key.size() >= open.size() + end.size() && ends_with(key, end)) {
                return true;
            }
        }
    }
    return false;
}

// Rebuilds text and end time after words have been removed.
void resync_from_words(TranscriptSegment& seg) {
    std::string text;
    for (const Word& w : seg.words) text += w.text;
    seg.text = trim(text);
    if (!seg.words.empty()) seg.end = seg.words.back().end;
}

// Whisper often appends the credit to real speech in one segment, e.g.
// "...görüşmek üzere. Altyazı M.K." -- trim just the trailing words so the
// real text survives.
void trim_trailing_credit(TranscriptSegment& seg) {
    // Needs at least one word left over; a lone credit word is the whole-segment
    // case, already handled by the caller.
    if (seg.words.size() < 2) return;

    const std::size_t max_tail = std::min(kMaxTailWords, seg.words.size() - 1);
    std::string tail;
    for (std::size_t n = 1; n <= max_tail; ++n) {
        tail = fold(seg.words[seg.words.size() - n].text) + tail;
        if (is_credit(tail)) {
            seg.words.erase(seg.words.end() - static_cast<std::ptrdiff_t>(n),
                            seg.words.end());
            resync_from_words(seg);
            return;
        }
    }
}

}  // namespace

std::string fold(const std::string& s) {
    // Keyed by the two UTF-8 bytes of each Turkish letter, both cases.
    static const std::unordered_map<std::uint16_t, char> kTurkish = {
        {0xC3A7, 'c'}, {0xC387, 'c'},   // ç Ç
        {0xC49F, 'g'}, {0xC49E, 'g'},   // ğ Ğ
        {0xC4B1, 'i'}, {0xC4B0, 'i'},   // ı İ
        {0xC3B6, 'o'}, {0xC396, 'o'},   // ö Ö
        {0xC59F, 's'}, {0xC59E, 's'},   // ş Ş
        {0xC3BC, 'u'}, {0xC39C, 'u'},   // ü Ü
    };

    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) {
                out += static_cast<char>(std::tolower(c));
            }
            ++i;
            continue;
        }
        if (i + 1 < s.size()) {
            const auto pair = static_cast<std::uint16_t>(
                (c << 8) | static_cast<unsigned char>(s[i + 1]));
            const auto it = kTurkish.find(pair);
            if (it != kTurkish.end()) {
                out += it->second;
                i += 2;
                continue;
            }
        }
        // Any other non-ASCII character: drop the whole UTF-8 sequence.
        for (++i; i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; ++i) {
        }
    }
    return out;
}

bool is_credit(const std::string& key) {
    if (key.empty()) return false;
    return exact_credit(key) || is_description_credit(key);
}

bool strip_credits(TranscriptSegment& seg) {
    // A segment that is nothing but a credit is pure hallucination.
    if (is_credit(fold(seg.text))) return false;
    // ...and a credit tacked onto the end of real speech loses just the tail.
    trim_trailing_credit(seg);
    return !seg.text.empty();
}

}  // namespace transcriptor::stt
