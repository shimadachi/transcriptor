#include "llm/chunking.h"

#include <algorithm>
#include <utility>

namespace transcriptor::llm {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::vector<std::string> split_transcript(const std::string& text,
                                          std::size_t max_chars) {
    std::vector<std::string> chunks;
    if (max_chars == 0) return chunks;
    if (text.size() <= max_chars) {
        chunks.push_back(text);
        return chunks;
    }

    std::string current;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos + 1);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;

        // A single line longer than the budget has to be cut mid-sentence.
        while (line.size() > max_chars) {
            if (!current.empty()) {
                chunks.push_back(current);
                current.clear();
            }
            chunks.push_back(line.substr(0, max_chars));
            line = line.substr(max_chars);
        }
        if (current.size() + line.size() > max_chars && !current.empty()) {
            chunks.push_back(current);
            current.clear();
        }
        current += line;
    }
    if (!trim(current).empty()) chunks.push_back(current);
    return chunks;
}

std::vector<std::string> split_to_fit(const std::string& text,
                                      std::size_t budget_chars,
                                      const FitsFn& fits) {
    std::vector<std::string> out;
    std::vector<std::string> work = split_transcript(text, budget_chars);
    std::reverse(work.begin(), work.end());   // pop_back() walks it in order

    while (!work.empty()) {
        std::string piece = std::move(work.back());
        work.pop_back();

        if (fits(piece)) {
            out.push_back(std::move(piece));
            continue;
        }
        // Denser than the guess assumed. Halve this piece and try again; a
        // piece that cannot be halved any further means the fixed part of the
        // prompt is what does not fit, and no split will rescue it.
        const std::size_t half = piece.size() / 2;
        auto parts = (half >= kMinSectionChars) ? split_transcript(piece, half)
                                                : std::vector<std::string>{};
        if (parts.size() < 2) throw SectionTooLarge{};

        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            work.push_back(std::move(*it));
        }
    }
    return out;
}

}  // namespace transcriptor::llm
